#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_task_wdt.h"
#include "cJSON.h"
#include "lvgl.h"
#include "lv_port_disp.h"
#include "lv_port_indev.h"
#include "ds3231.h"
#include "wifi_manager.h"
#include "weather_api.h"
#include "ec11.h"
#include "speech_recognition.h"
#include "driver/ledc.h"
#include "driver/adc.h"  // ADC相关头文件
#include "esp_adc_cal.h"
#include "audio_player.h"
#include "secrets.h"

// MQ2烟雾传感器相关定义
#define MQ2_ADC_CHANNEL      ADC1_CHANNEL_0  // IO1对应的ADC通道
#define MQ2_ADC_WIDTH        ADC_WIDTH_BIT_12
#define MQ2_ADC_ATTEN        ADC_ATTEN_DB_12
#define MQ2_SAMPLE_COUNT     64
#define MQ2_DEFAULT_VREF     1100
#define MQ2_ALARM_THRESHOLD  1800   // 烟雾报警阈值，根据实际情况调整
#define MQ2_UPDATE_INTERVAL  2000   // 更新间隔2秒
#define MQ2_ALARM_COUNT      2      // 连续超出阈值次数触发报警

static esp_adc_cal_characteristics_t mq2_adc_chars;
static lv_obj_t *mq2_label = NULL;       // 烟雾值显示标签
static uint32_t mq2_value = 0;           // 烟雾传感器当前读数
static bool mq2_alarm_state = false;     // 烟雾报警状态
static uint8_t mq2_alarm_counter = 0;    // 连续超出阈值计数器
static bool mq2_audio_alarm_triggered = false; // 音频报警是否已触发

// 报警提示音数据 - 紧急警报"呜-呜-呜"声音
static const uint16_t mq2_alarm_tone_data[] = {
    // 第一个"呜"音 - 快速上升
    0x0000, 0x2000, 0x4000, 0x6000, 0x7FFF, 0x7FFF, 0x7FFF, 0x7FFF,
    0x7FFF, 0x7FFF, 0x7FFF, 0x7FFF, 0x6000, 0x4000, 0x2000, 0x0000,
    // 短暂停顿
    0x0000, 0x0000, 0x0000, 0x0000,
    
    // 第二个"呜"音 - 更高频率
    0x0000, 0x3000, 0x6000, 0x7FFF, 0x7FFF, 0x7FFF, 0x7FFF, 0x7FFF,
    0x7FFF, 0x7FFF, 0x7FFF, 0x7FFF, 0x6000, 0x3000, 0x0000, 0x0000,
    // 短暂停顿
    0x0000, 0x0000, 0x0000, 0x0000,
    
    // 第三个"呜"音 - 低频率但音量大
    0x0000, 0x4000, 0x7FFF, 0x7FFF, 0x7FFF, 0x7FFF, 0x7FFF, 0x7FFF,
    0x7FFF, 0x7FFF, 0x7FFF, 0x7FFF, 0x7FFF, 0x4000, 0x0000, 0x0000,
    
    // 快速高频警报音
    0x7FFF, 0x0000, 0x7FFF, 0x0000, 0x7FFF, 0x0000, 0x7FFF, 0x0000,
    0x7FFF, 0x0000, 0x7FFF, 0x0000, 0x7FFF, 0x0000, 0x7FFF, 0x0000,
};
static const size_t mq2_alarm_tone_size = sizeof(mq2_alarm_tone_data);
#include "audio_data.h"
#include "web_server.h"
#include "driver/gpio.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "esp_netif.h" // 添加网络接口头文件
#include "wifi_status_task.h" // 添加WiFi状态更新任务


/* 外部字体声明 */
LV_FONT_DECLARE(lv_font_simsun_16_cjk);
LV_FONT_DECLARE(my_font_1);

static const char *TAG = "MAIN";

/* 桌面切换相关变量 */
static int current_desktop = 0;     // 当前桌面编号 (0:桌面1, 1:桌面2, 2:桌面3, 3:桌面4)
#define DESKTOP_COUNT 4             // 总桌面数量
static lv_obj_t *desktop_screens[DESKTOP_COUNT];  // 桌面屏幕数组
static lv_obj_t *desktop_dots[DESKTOP_COUNT][DESKTOP_COUNT]; // 每个桌面的点状指示器
static lv_obj_t *dot_containers[DESKTOP_COUNT]; // 每个桌面的点状指示器容器

/* 标题标签 - 设置为全局变量以便修改颜色 */
lv_obj_t *title_label = NULL; // 导出为全局变量以便修改颜色

/* 连接监控相关变量 - 导出为全局变量 */
bool phone_connected = false;        // 标记是否有手机连接
uint16_t connected_stations = 0;     // 连接的设备数量
TickType_t last_station_check = 0;   // 上次检查时间
#define STATION_CHECK_INTERVAL_MS 3000      // 每3秒检查一次连接



/* 定时器相关变量 */
typedef enum {
    TIMER_STATE_MAIN,           // 主界面状态
    TIMER_STATE_MENU,           // 菜单状态
    TIMER_STATE_SET_HOUR,       // 设置小时
    TIMER_STATE_SET_MINUTE,     // 设置分钟  
    TIMER_STATE_SET_SECOND,     // 设置秒
    TIMER_STATE_COUNTDOWN,      // 倒计时状态
    TIMER_STATE_TIME_UP         // 时间到状态
} timer_state_t;

static timer_state_t timer_state = TIMER_STATE_MAIN;
static int timer_hours = 0;
static int timer_minutes = 0;
static int timer_seconds = 0;
static int countdown_hours = 0;
static int countdown_minutes = 0;
static int countdown_seconds = 0;
static bool timer_running = false;
static TickType_t timer_start_tick = 0;

/* 闹钟相关变量 */
typedef enum {
    ALARM_STATE_MAIN,           // 主界面状态
    ALARM_STATE_MENU,           // 菜单状态
    ALARM_STATE_SET_HOUR,       // 设置小时
    ALARM_STATE_SET_MINUTE,     // 设置分钟
    ALARM_STATE_ALARM_SET,      // 闹钟已设置状态
    ALARM_STATE_RINGING         // 闹钟响铃状态
} alarm_state_t;

static alarm_state_t alarm_state = ALARM_STATE_MAIN;
static int alarm_hours = 7;         // 默认闹钟时间 7:00
static int alarm_minutes = 0;
static bool alarm_enabled = false;  // 闹钟是否启用
static bool alarm_ringing = false;  // 闹钟是否正在响铃

/* 事件提醒相关变量 */
static char reminder_title[64] = {0};
static char reminder_description[128] = {0};
static char reminder_datetime[32] = {0};
static bool reminder_valid = false;  // 标记是否有有效的提醒

/* 桌面1设置功能相关变量 */
typedef enum {
    SETTING_STATE_MAIN,         // 主界面状态
    SETTING_STATE_MENU,         // 主菜单状态
    SETTING_STATE_TIME_MENU,    // 时间设置菜单
    SETTING_STATE_SET_YEAR,     // 设置年份
    SETTING_STATE_SET_MONTH,    // 设置月份
    SETTING_STATE_SET_DAY,      // 设置日期
    SETTING_STATE_SET_HOUR,     // 设置小时
    SETTING_STATE_SET_MINUTE,   // 设置分钟
    SETTING_STATE_SET_SECOND,   // 设置秒钟
    SETTING_STATE_TIME_CONFIRM, // 时间设置确认
    SETTING_STATE_TIME_COMPLETE,// 时间设置完成
    SETTING_STATE_PREF_MENU,    // 偏好设置菜单
    SETTING_STATE_TIME_FORMAT,  // 时间格式设置
    SETTING_STATE_NETWORK_TIME, // 网络时间设置
    SETTING_STATE_VOLUME,       // 音量设置
    SETTING_STATE_RINGTONE,     // 铃声设置
    SETTING_STATE_SPEECH_REC    // AI助手状态
} setting_state_t;

static setting_state_t setting_state = SETTING_STATE_MAIN;
static int setting_year = 2024;
static int setting_month = 12;
static int setting_day = 27;
static int setting_hour = 12;
static int setting_minute = 0;
static int setting_second = 0;

/* 天气预报相关变量 */
typedef struct {
    char date[16];           // 日期 YYYY-MM-DD
    char week[8];            // 星期几
    char dayweather[32];     // 白天天气
    char nightweather[32];   // 晚上天气
    char daytemp[8];         // 白天温度
    char nighttemp[8];       // 晚上温度
    char daywind[16];        // 白天风向
    char nightwind[16];      // 晚上风向
    char daypower[8];        // 白天风力
    char nightpower[8];      // 晚上风力
} weather_forecast_t;

static weather_forecast_t forecast_data[4];  // 存储4天预报数据
static bool forecast_updated = false;        // 预报数据更新标志
static char forecast_city[32] = "";          // 预报城市名
static char forecast_update_time[32] = "";   // 预报更新时间

/* LVGL相关变量 - 桌面1 */
static lv_obj_t *time_label;
static lv_obj_t *date_label;
lv_obj_t *wifi_status_label;
lv_obj_t *wifi_scan_label;  // WiFi扫描结果显示
static lv_obj_t *weather_label;    // 天气信息显示
static lv_obj_t *sync_status_label; // 时间同步状态显示
static lv_obj_t *lunar_date_label;  // 农历日期显示
static lv_obj_t *setting_display_label;  // 设置页面显示标签
static lv_obj_t *setting_hint_label;     // 设置页面提示标签
static lv_obj_t *setting_title_label = NULL; // AI助手页面标题
static lv_obj_t *user_message_label = NULL;  // 用户消息标签（蓝色）
static lv_obj_t *ai_message_label = NULL;    // AI消息标签（绿色）

/* 设置页面相关变量 */
static lv_obj_t *setting_screen = NULL;  // 设置页面屏幕
static bool setting_page_active = false; // 设置页面是否激活

/* 偏好设置变量 */
static bool use_24hour_format = true;   // true: 24小时制, false: 12小时制
static bool use_network_time = true;    // true: 使用网络时间, false: 手动时间

/* 菜单选择变量 */
static int main_menu_selection = 0;     // 主菜单选择: 0=时间设置, 1=偏好设置, 2=AI助手
static int pref_menu_selection = 0;     // 偏好菜单选择: 0=时间格式, 1=网络时间, 2=音量设置, 3=铃声设置

/* 音量设置变量 */
static int system_volume = 20;          // 系统音量设置 (0-100)，默认20%

/* 铃声选择变量 */
typedef enum {
    RINGTONE_WAV_FILE = 0,              // 使用WAV文件（ring.wav）
    RINGTONE_BUILTIN_TONE = 1           // 使用内置双音调铃声
} ringtone_type_t;

static ringtone_type_t selected_ringtone = RINGTONE_WAV_FILE;  // 默认使用WAV文件

/* AI助手相关变量 */
static bool speech_rec_active = false;  // AI助手是否激活
static TaskHandle_t speech_display_task_handle = NULL;  // AI助手显示更新任务

/* MQ2烟雾传感器相关代码已删除 */

/* DHT11温湿度传感器相关变量和配置 */
#define DHT11_PIN 4  // DHT11 data引脚连接到GPIO4
#define DHT11_UPDATE_INTERVAL_MS 3000 // 3秒更新一次

static lv_obj_t *indoor_temp_label;    // 室内温度显示标签
static lv_obj_t *indoor_humid_label;   // 室内湿度显示标签
static float indoor_temperature = 0.0;  // 室内温度
static float indoor_humidity = 0.0;     // 室内湿度
static bool dht11_initialized = false;  // DHT11初始化状态
// MQ2相关变量已删除

/* 双击检测变量 */
static uint32_t last_key_press_time = 0;

static bool waiting_for_double_click = false;
static TimerHandle_t single_click_timer = NULL;
#define DOUBLE_CLICK_INTERVAL_MS 500  // 双击间隔时间

/* 延迟删除设置页面变量 */
static bool setting_page_delete_requested = false;
static TimerHandle_t setting_delete_timer = NULL;
#define SETTING_DELETE_DELAY_MS 200  // 延迟删除时间

/* 时间设置完成定时器变量 */
static TimerHandle_t time_complete_timer = NULL;
#define TIME_COMPLETE_DELAY_MS 2000  // 时间设置完成后延迟返回时间

/* 函数声明 */
static void handle_setting_button_press_delayed(void);
static void setting_delete_timer_callback(TimerHandle_t xTimer);
static void time_setting_complete_callback(TimerHandle_t xTimer);
static void speech_display_update_task(void *arg);
static void start_speech_recognition(void);
static void stop_speech_recognition(void);
static void start_vibration(void);
// MQ2相关函数声明已删除
static void timer_audio_task(void *arg);
static void alarm_audio_task(void *arg);
static void memory_monitor_task(void *arg);
static esp_err_t dht11_init(void);
static esp_err_t dht11_read_data(float *temperature, float *humidity);
static void dht11_update_task(void *arg);
static void update_indoor_temp_humid_display(void);


/* LVGL相关变量 - 桌面2 */
static lv_obj_t *timer_display_label;     // 定时器显示标签
static lv_obj_t *timer_status_label;      // 定时器状态标签
static lv_obj_t *timer_hint_label;        // 操作提示标签

/* LVGL相关变量 - 桌面3 */
static lv_obj_t *alarm_display_label;     // 闹钟显示标签
static lv_obj_t *alarm_status_label;      // 闹钟状态标签
static lv_obj_t *alarm_hint_label;        // 操作提示标签
static lv_obj_t *reminder_display_label;  // 事件提醒标题标签
static lv_obj_t *reminder_time_label;     // 事件提醒时间标签
static lv_obj_t *reminder_content_label;  // 事件提醒内容标签
static lv_obj_t *reminder_alert_label;    // 桌面1临近提醒标签

/* LVGL相关变量 - 桌面4 */
static lv_obj_t *forecast_display_label;  // 天气预报显示标签

/* 桌面4状态管理 */
typedef enum {
    FORECAST_STATE_TEXT      // 文字预报状态
} forecast_state_t;

static forecast_state_t forecast_state = FORECAST_STATE_TEXT;

/* WiFi扫描相关变量 */
bool wifi_scan_requested = false;  // 导出为全局变量
TickType_t last_scan_time = 0;      // 导出为全局变量
#define SCAN_INTERVAL_MS 30000  // 30秒扫描一次

/* 天气更新相关变量 */
static TickType_t last_weather_update = 0;
#define WEATHER_UPDATE_INTERVAL_MS 60000 // 60秒更新一次天气

/* 农历更新相关变量 */
static TickType_t last_lunar_update = 0;
static bool lunar_first_update = false;  // 标记是否已进行首次农历更新
#define LUNAR_UPDATE_INTERVAL_MS 3600000  // 30秒更新一次农历（调试用，正式版本可改为3600000）
#define LUNAR_API_URL "http://api.tiax.cn/almanac/"

/* 农历缓存相关变量 */
typedef struct {
    int year;
    int month;
    int day;
    char lunar_date[64];      // 农历日期（如"二月十五"）
    bool valid;               // 数据是否有效
} lunar_cache_t;

#define LUNAR_CACHE_DAYS 7
static lunar_cache_t lunar_cache[LUNAR_CACHE_DAYS];
static time_t lunar_cache_timestamp = 0;  // 缓存时间戳
#define LUNAR_CACHE_VALID_SECONDS (24 * 3600)  // 缓存24小时有效

/* 天气缓存相关变量 */
static char cached_weather_str[256] = "正在获取天气信息...";
static bool weather_cache_valid = false;
static time_t weather_cache_time = 0;
#define WEATHER_CACHE_TIMEOUT (6 * 3600)  // 缓存6小时有效

/* 时间同步相关变量 */
static bool time_synced = false;
static TickType_t last_time_sync = 0;
#define TIME_SYNC_INTERVAL_MS 3600000  // 1小时同步一次时间
#define TIME_SYNC_URL "http://f.m.suning.com/api/ct.do"

/* 星期名称数组 - 中文显示 */
static const char *weekdays[] = {
    "", "星期一", "星期二", "星期三", "星期四", "星期五", "星期六", "星期天"
};

/* HTTP响应数据结构 */
typedef struct {
    char *data;
    size_t len;
} http_response_t;

/* HTTP事件处理函数 */
static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    http_response_t *response = (http_response_t *)evt->user_data;
    
    switch (evt->event_id) {
        case HTTP_EVENT_ERROR:
            ESP_LOGE(TAG, "HTTP_EVENT_ERROR");
            break;
        case HTTP_EVENT_ON_CONNECTED:
            ESP_LOGI(TAG, "HTTP_EVENT_ON_CONNECTED");
            break;
        case HTTP_EVENT_HEADER_SENT:
            ESP_LOGI(TAG, "HTTP_EVENT_HEADER_SENT");
            break;
        case HTTP_EVENT_ON_HEADER:
            ESP_LOGI(TAG, "HTTP_EVENT_ON_HEADER, key=%s, value=%s", evt->header_key, evt->header_value);
            break;
        case HTTP_EVENT_ON_DATA:
            ESP_LOGI(TAG, "HTTP_EVENT_ON_DATA: 接收到 %d 字节数据", evt->data_len);
            if (response->data == NULL) {
                response->data = malloc(evt->data_len + 1);
                response->len = 0;
                if (response->data == NULL) {
                    ESP_LOGE(TAG, "HTTP响应数据内存分配失败");
                    break;
                }
            } else {
                // 重新分配内存以容纳新数据
                response->data = realloc(response->data, response->len + evt->data_len + 1);
                if (response->data == NULL) {
                    ESP_LOGE(TAG, "HTTP响应数据内存重新分配失败");
                    break;
                }
            }
            memcpy(response->data + response->len, evt->data, evt->data_len);
            response->len += evt->data_len;
            response->data[response->len] = '\0';
            ESP_LOGI(TAG, "HTTP数据累计长度: %d", response->len);
            break;
        case HTTP_EVENT_ON_FINISH:
            ESP_LOGI(TAG, "HTTP_EVENT_ON_FINISH");
            break;
        case HTTP_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "HTTP_EVENT_DISCONNECTED");
            break;
        default:
            break;
    }
    return ESP_OK;
}

/* 时间戳转换为ds3231_time_t结构 */
static void timestamp_to_ds3231_time(long long timestamp_ms, ds3231_time_t *time_struct)
{
    time_t timestamp_sec = timestamp_ms / 1000;
    
    /* 转换为北京时间 (+8小时) */
    timestamp_sec += 8 * 3600;
    
    struct tm *timeinfo = gmtime(&timestamp_sec);
    
    time_struct->year = timeinfo->tm_year + 1900;
    time_struct->month = timeinfo->tm_mon + 1;
    time_struct->date = timeinfo->tm_mday;
    time_struct->hour = timeinfo->tm_hour;
    time_struct->minute = timeinfo->tm_min;
    time_struct->second = timeinfo->tm_sec;
    time_struct->day_of_week = timeinfo->tm_wday == 0 ? 7 : timeinfo->tm_wday; // 调整星期格式
}

/* 网络时间同步函数 */
static esp_err_t sync_time_from_network(void)
{
    // 如果用户选择不使用网络时间，直接返回
    if (!use_network_time) {
        ESP_LOGI(TAG, "网络时间同步已禁用");
        return ESP_OK;
    }
    
    ESP_LOGI(TAG, "开始网络时间同步...");
    
    /* 先获取DS3231当前时间 */
    ds3231_time_t ds3231_time;
    bool ds3231_valid = (ds3231_get_time(&ds3231_time) == ESP_OK);
    
    http_response_t response = {0};
    
    esp_http_client_config_t config = {
        .url = TIME_SYNC_URL,
        .event_handler = http_event_handler,
        .user_data = &response,
        .timeout_ms = 10000,
    };
    
    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_err_t err = esp_http_client_perform(client);
    
    if (err == ESP_OK) {
        int status_code = esp_http_client_get_status_code(client);
        if (status_code == 200 && response.data != NULL) {
            ESP_LOGI(TAG, "时间API响应: %s", response.data);
            
            /* 解析JSON响应 */
            cJSON *json = cJSON_Parse(response.data);
            if (json != NULL) {
                cJSON *current_time = cJSON_GetObjectItem(json, "currentTime");
                cJSON *code = cJSON_GetObjectItem(json, "code");
                
                if (current_time != NULL && code != NULL && 
                    cJSON_IsNumber(current_time) && strcmp(cJSON_GetStringValue(code), "1") == 0) {
                    
                    long long timestamp = (long long)cJSON_GetNumberValue(current_time);
                    ESP_LOGI(TAG, "获取到网络时间戳: %lld", timestamp);
                    
                    /* 转换网络时间戳 */
                    ds3231_time_t network_time;
                    timestamp_to_ds3231_time(timestamp, &network_time);
                    
                    bool should_sync = false;
                    
                    if (!ds3231_valid) {
                        ESP_LOGI(TAG, "DS3231时间无效，执行同步");
                        should_sync = true;
                    } else {
                        /* 计算时间差异（秒） */
                        time_t ds3231_timestamp = mktime(&(struct tm){
                            .tm_year = ds3231_time.year - 1900,
                            .tm_mon = ds3231_time.month - 1,
                            .tm_mday = ds3231_time.date,
                            .tm_hour = ds3231_time.hour,
                            .tm_min = ds3231_time.minute,
                            .tm_sec = ds3231_time.second
                        });
                        
                        time_t network_timestamp = timestamp / 1000;
                        long time_diff = abs((long)(network_timestamp - ds3231_timestamp));
                        
                        ESP_LOGI(TAG, "DS3231时间: %04d-%02d-%02d %02d:%02d:%02d", 
                                ds3231_time.year, ds3231_time.month, ds3231_time.date,
                                ds3231_time.hour, ds3231_time.minute, ds3231_time.second);
                        ESP_LOGI(TAG, "网络时间: %04d-%02d-%02d %02d:%02d:%02d", 
                                network_time.year, network_time.month, network_time.date,
                                network_time.hour, network_time.minute, network_time.second);
                        ESP_LOGI(TAG, "时间差异: %ld 秒", time_diff);
                        
                        /* 只有时间差异超过5分钟（300秒）才同步 */
                        if (time_diff > 300) {
                            ESP_LOGI(TAG, "时间差异过大，执行同步");
                            should_sync = true;
                        } else {
                            ESP_LOGI(TAG, "时间差异较小，跳过同步，保持DS3231时间");
                            should_sync = false;
                        }
                    }
                    
                    if (should_sync) {
                        esp_err_t set_result = ds3231_set_time(&network_time);
                        if (set_result == ESP_OK) {
                            ESP_LOGI(TAG, "时间同步成功: %04d-%02d-%02d %02d:%02d:%02d", 
                                    network_time.year, network_time.month, network_time.date,
                                    network_time.hour, network_time.minute, network_time.second);
                            time_synced = true;
                            last_time_sync = xTaskGetTickCount();
                            
                            /* 更新同步状态显示 */
                            if (sync_status_label) {
                                lv_label_set_text(sync_status_label, "✓");
                                lv_obj_set_style_text_color(sync_status_label, lv_color_hex(0x00AA00), 0); // 绿色
                            }
                            
                            err = ESP_OK;
                        } else {
                            ESP_LOGE(TAG, "设置DS3231时间失败");
                            
                            /* 更新同步状态显示为失败 */
                            if (sync_status_label) {
                                lv_label_set_text(sync_status_label, "✗");
                                lv_obj_set_style_text_color(sync_status_label, lv_color_hex(0xAA0000), 0); // 红色
                            }
                            
                            err = ESP_FAIL;
                        }
                    } else {
                        /* 跳过同步，但标记为已检查 */
                        time_synced = true;
                        last_time_sync = xTaskGetTickCount();
                        
                        /* 更新同步状态显示为已检查但未同步 */
                        if (sync_status_label) {
                            lv_label_set_text(sync_status_label, "◐");
                            lv_obj_set_style_text_color(sync_status_label, lv_color_hex(0x0000AA), 0); // 蓝色
                        }
                        
                        err = ESP_OK;
                    }
                } else {
                    ESP_LOGE(TAG, "JSON数据格式错误");
                    
                    /* 更新同步状态显示为失败 */
                    if (sync_status_label) {
                        lv_label_set_text(sync_status_label, "✗");
                        lv_obj_set_style_text_color(sync_status_label, lv_color_hex(0xAA0000), 0); // 红色
                    }
                    
                    err = ESP_FAIL;
                }
                cJSON_Delete(json);
            } else {
                ESP_LOGE(TAG, "JSON解析失败");
                
                /* 更新同步状态显示为失败 */
                if (sync_status_label) {
                    lv_label_set_text(sync_status_label, "✗");
                    lv_obj_set_style_text_color(sync_status_label, lv_color_hex(0xAA0000), 0); // 红色
                }
                
                err = ESP_FAIL;
            }
        } else {
            ESP_LOGE(TAG, "HTTP请求失败，状态码: %d", status_code);
            
            /* 更新同步状态显示为失败 */
            if (sync_status_label) {
                lv_label_set_text(sync_status_label, "✗");
                lv_obj_set_style_text_color(sync_status_label, lv_color_hex(0xAA0000), 0); // 红色
            }
            
            err = ESP_FAIL;
        }
    } else {
        ESP_LOGE(TAG, "HTTP请求执行失败: %s", esp_err_to_name(err));
        
        /* 更新同步状态显示为失败 */
        if (sync_status_label) {
            lv_label_set_text(sync_status_label, "✗");
            lv_obj_set_style_text_color(sync_status_label, lv_color_hex(0xAA0000), 0); // 红色
        }
    }
    
    /* 清理资源 */
    if (response.data) {
        free(response.data);
    }
    esp_http_client_cleanup(client);
    
    return err;
}

/* 提取农历月份和日期 */
static void extract_lunar_month_day(const char* full_lunar_date, char* result, size_t result_size)
{
    if (full_lunar_date == NULL || result == NULL || result_size == 0) {
        if (result && result_size > 0) {
            result[0] = '\0';
        }
        return;
    }
    
    ESP_LOGI(TAG, "开始提取农历月日: %s", full_lunar_date);
    
    /* 查找年份后的空格位置，格式通常为 "农历二零二三年 五月(小) 二十" */
    const char *year_end = strstr(full_lunar_date, "年 ");
    if (year_end == NULL) {
        /* 如果没找到"年 "，尝试其他格式 */
        year_end = strstr(full_lunar_date, "年");
        if (year_end != NULL) {
            year_end += strlen("年");
            /* 跳过可能的空格 */
            while (*year_end == ' ') year_end++;
        }
    } else {
        year_end += strlen("年 ");
    }
    
    if (year_end != NULL && *year_end != '\0') {
        /* 找到年份后的内容 */
        const char *month_start = year_end;
        
        /* 复制月份和日期部分，但要处理可能的括号信息 */
        char temp_buffer[64];
        strncpy(temp_buffer, month_start, sizeof(temp_buffer) - 1);
        temp_buffer[sizeof(temp_buffer) - 1] = '\0';
        
        /* 移除括号内容，如 "(小)" 或 "(大)" */
        char *bracket_start = strchr(temp_buffer, '(');
        if (bracket_start != NULL) {
            char *bracket_end = strchr(bracket_start, ')');
            if (bracket_end != NULL) {
                /* 移除括号及其内容 */
                memmove(bracket_start, bracket_end + 1, strlen(bracket_end + 1) + 1);
            }
        }
        
        /* 复制结果到输出缓冲区 */
        strncpy(result, temp_buffer, result_size - 1);
        result[result_size - 1] = '\0';
        
        /* 移除尾部空格 */
        int len = strlen(result);
        while (len > 0 && result[len - 1] == ' ') {
            result[--len] = '\0';
        }
        
        ESP_LOGI(TAG, "提取的农历月日: %s", result);
    } else {
        /* 如果提取失败，使用原字符串 */
        strncpy(result, full_lunar_date, result_size - 1);
        result[result_size - 1] = '\0';
        ESP_LOGW(TAG, "农历提取失败，使用原字符串: %s", result);
    }
}

/* 初始化农历缓存 */
static void init_lunar_cache(void)
{
    for (int i = 0; i < LUNAR_CACHE_DAYS; i++) {
        lunar_cache[i].valid = false;
        lunar_cache[i].lunar_date[0] = '\0';
    }
    lunar_cache_timestamp = 0;
}

/* 检查农历缓存是否有效 */
static bool is_lunar_cache_valid(void)
{
    if (lunar_cache_timestamp == 0) {
        return false;
    }
    
    time_t current_time;
    time(&current_time);
    
    return (current_time - lunar_cache_timestamp) < LUNAR_CACHE_VALID_SECONDS;
}

/* 从缓存获取农历日期 */
static bool get_lunar_from_cache(int year, int month, int day, char* result, size_t result_size)
{
    if (!is_lunar_cache_valid()) {
        return false;
    }
    
    for (int i = 0; i < LUNAR_CACHE_DAYS; i++) {
        if (lunar_cache[i].valid && 
            lunar_cache[i].year == year && 
            lunar_cache[i].month == month && 
            lunar_cache[i].day == day) {
            strncpy(result, lunar_cache[i].lunar_date, result_size - 1);
            result[result_size - 1] = '\0';
            return true;
        }
    }
    
    return false;
}

/* 保存农历日期到缓存 */
static void save_lunar_to_cache(int year, int month, int day, const char* lunar_date)
{
    for (int i = 0; i < LUNAR_CACHE_DAYS; i++) {
        if (!lunar_cache[i].valid) {
            lunar_cache[i].year = year;
            lunar_cache[i].month = month;
            lunar_cache[i].day = day;
            strncpy(lunar_cache[i].lunar_date, lunar_date, sizeof(lunar_cache[i].lunar_date) - 1);
            lunar_cache[i].lunar_date[sizeof(lunar_cache[i].lunar_date) - 1] = '\0';
            lunar_cache[i].valid = true;
            break;
        }
    }
}

/* 计算指定日期的下一天 */
static void get_next_day(int* year, int* month, int* day)
{
    int days_in_month[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    
    // 检查闰年
    if (*year % 4 == 0 && (*year % 100 != 0 || *year % 400 == 0)) {
        days_in_month[1] = 29;
    }
    
    (*day)++;
    if (*day > days_in_month[*month - 1]) {
        *day = 1;
        (*month)++;
        if (*month > 12) {
            *month = 1;
            (*year)++;
        }
    }
}

/* 天气缓存管理函数 */
static void update_weather_cache(const char *weather_info)
{
    if (weather_info && strlen(weather_info) > 0) {
        snprintf(cached_weather_str, sizeof(cached_weather_str), "%s", weather_info);
        weather_cache_valid = true;
        weather_cache_time = time(NULL);
        ESP_LOGI(TAG, "天气信息已缓存: %s", cached_weather_str);
    }
}

static bool is_weather_cache_valid(void)
{
    if (!weather_cache_valid) {
        return false;
    }
    
    time_t current_time = time(NULL);
    time_t cache_age = current_time - weather_cache_time;
    
    if (cache_age > WEATHER_CACHE_TIMEOUT) {
        ESP_LOGI(TAG, "天气缓存已过期，缓存时间: %lld 秒前", (long long)cache_age);
        weather_cache_valid = false;
        return false;
    }
    
    ESP_LOGI(TAG, "使用缓存的天气信息，缓存时间: %lld 秒前", (long long)cache_age);
    return true;
}

static const char* get_cached_weather(void)
{
    return cached_weather_str;
}

/* 获取单个日期的农历信息 */
static esp_err_t get_single_lunar_date(int year, int month, int day, char* lunar_result, size_t result_size)
{
    /* 构建API URL */
    char url[256];
    snprintf(url, sizeof(url), "%s?year=%d&month=%d&day=%d", 
             LUNAR_API_URL, year, month, day);
    
    ESP_LOGI(TAG, "农历API URL: %s", url);
    
    http_response_t response = {0};
    
    esp_http_client_config_t config = {
        .url = url,
        .event_handler = http_event_handler,
        .user_data = &response,
        .timeout_ms = 10000,
    };
    
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG, "农历API HTTP客户端初始化失败");
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "开始发送农历API HTTP请求...");
    esp_err_t err = esp_http_client_perform(client);
    ESP_LOGI(TAG, "农历API HTTP请求完成，结果: %s", esp_err_to_name(err));
    
    if (err == ESP_OK) {
        int status_code = esp_http_client_get_status_code(client);
        ESP_LOGI(TAG, "农历API HTTP状态码: %d", status_code);
        
        if (status_code == 200 && response.data != NULL && response.len > 0) {
            ESP_LOGI(TAG, "农历API响应: %s", response.data);
            
            /* 解析JSON响应 */
            cJSON *json = cJSON_Parse(response.data);
            if (json != NULL) {
                cJSON *lunar_date_obj = cJSON_GetObjectItem(json, "农历日期");
                
                if (lunar_date_obj != NULL && cJSON_IsString(lunar_date_obj)) {
                    const char *full_lunar_date = cJSON_GetStringValue(lunar_date_obj);
                    ESP_LOGI(TAG, "完整农历日期: %s", full_lunar_date);
                    
                    /* 提取月份和日期部分 */
                    extract_lunar_month_day(full_lunar_date, lunar_result, result_size);
                    
                    ESP_LOGI(TAG, "农历日期获取成功: %s", lunar_result);
                    err = ESP_OK;
                } else {
                    ESP_LOGE(TAG, "农历日期字段不存在或格式错误");
                    err = ESP_FAIL;
                }
                cJSON_Delete(json);
            } else {
                ESP_LOGE(TAG, "农历API JSON解析失败");
                err = ESP_FAIL;
            }
        } else {
            ESP_LOGE(TAG, "农历API HTTP请求失败，状态码: %d", status_code);
            err = ESP_FAIL;
        }
    } else {
        ESP_LOGE(TAG, "农历API HTTP请求执行失败: %s", esp_err_to_name(err));
    }
    
    /* 清理资源 */
    if (response.data) {
        free(response.data);
    }
    esp_http_client_cleanup(client);
    
    return err;
}

/* 农历批量获取任务 */
static void lunar_batch_task(void *arg)
{
    ESP_LOGI(TAG, "农历批量获取任务启动");
    
    /* 获取当前日期 */
    ds3231_time_t current_time;
    if (ds3231_get_time(&current_time) != ESP_OK) {
        ESP_LOGE(TAG, "无法获取当前时间");
        vTaskDelete(NULL);
        return;
    }
    
    ESP_LOGI(TAG, "当前时间: %04d-%02d-%02d", current_time.year, current_time.month, current_time.date);
    
    /* 检查WiFi连接状态 */
    wifi_status_t wifi_status = wifi_get_status();
    if (wifi_status != WIFI_STATUS_CONNECTED) {
        ESP_LOGW(TAG, "WiFi未连接，无法获取农历信息");
        vTaskDelete(NULL);
        return;
    }
    
    /* 清空现有缓存 */
    init_lunar_cache();
    
    int success_count = 0;
    int target_year = current_time.year;
    int target_month = current_time.month;
    int target_day = current_time.date;
    
    /* 获取未来7天的农历信息 */
    for (int i = 0; i < LUNAR_CACHE_DAYS; i++) {
        char lunar_result[64];
        
        ESP_LOGI(TAG, "正在获取第%d天的农历: %04d-%02d-%02d", i+1, target_year, target_month, target_day);
        
        if (get_single_lunar_date(target_year, target_month, target_day, lunar_result, sizeof(lunar_result)) == ESP_OK) {
            save_lunar_to_cache(target_year, target_month, target_day, lunar_result);
            success_count++;
            ESP_LOGI(TAG, "成功获取第%d天农历: %s", i+1, lunar_result);
        } else {
            ESP_LOGW(TAG, "获取第%d天农历失败", i+1);
        }
        
        /* 计算下一天 */
        get_next_day(&target_year, &target_month, &target_day);
        
        /* 增加短暂延时，避免请求过于频繁 */
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    
    if (success_count > 0) {
        time(&lunar_cache_timestamp);
        ESP_LOGI(TAG, "农历批量获取完成，成功获取%d天数据", success_count);
    } else {
        ESP_LOGE(TAG, "农历批量获取失败，没有成功获取任何数据");
    }
    
    /* 任务完成，删除自身 */
    vTaskDelete(NULL);
}

/* 获取农历日期函数 */
static esp_err_t get_lunar_date(void)
{
    ESP_LOGI(TAG, "开始获取农历日期...");
    
    /* 获取当前日期 */
    ds3231_time_t current_time;
    if (ds3231_get_time(&current_time) != ESP_OK) {
        ESP_LOGE(TAG, "无法获取当前时间");
        if (lunar_date_label) {
            lv_label_set_text(lunar_date_label, "农历时间获取失败");
            lv_obj_set_style_text_font(lunar_date_label, &my_font_1, 0);
        }
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "当前时间: %04d-%02d-%02d", current_time.year, current_time.month, current_time.date);
    
    /* 首先尝试从缓存获取 */
    char lunar_display[64];
    if (get_lunar_from_cache(current_time.year, current_time.month, current_time.date, 
                           lunar_display, sizeof(lunar_display))) {
        /* 从缓存获取成功 */
        if (lunar_date_label) {
            lv_label_set_text(lunar_date_label, lunar_display);
            lv_obj_set_style_text_font(lunar_date_label, &my_font_1, 0);
        }
        ESP_LOGI(TAG, "从缓存获取农历日期成功: %s", lunar_display);
        return ESP_OK;
    }
    
    /* 缓存中没有，尝试在线获取 */
    wifi_status_t wifi_status = wifi_get_status();
    if (wifi_status != WIFI_STATUS_CONNECTED) {
        ESP_LOGW(TAG, "WiFi未连接且缓存无效，无法获取农历信息");
        if (lunar_date_label) {
            lv_label_set_text(lunar_date_label, "农历获取失败");
            lv_obj_set_style_text_font(lunar_date_label, &my_font_1, 0);
        }
        return ESP_ERR_WIFI_NOT_CONNECT;
    }
    
    /* 在线获取当前日期的农历 */
    if (get_single_lunar_date(current_time.year, current_time.month, current_time.date, 
                            lunar_display, sizeof(lunar_display)) == ESP_OK) {
        /* 保存到缓存 */
        save_lunar_to_cache(current_time.year, current_time.month, current_time.date, lunar_display);
        
        /* 更新显示 */
        if (lunar_date_label) {
            lv_label_set_text(lunar_date_label, lunar_display);
            lv_obj_set_style_text_font(lunar_date_label, &my_font_1, 0);
        }
        
        ESP_LOGI(TAG, "在线获取农历日期成功: %s", lunar_display);
        last_lunar_update = xTaskGetTickCount();
        
        /* 如果缓存无效，启动批量获取任务 */
        if (!is_lunar_cache_valid()) {
            ESP_LOGI(TAG, "启动农历批量获取任务...");
            xTaskCreate(lunar_batch_task, "lunar_batch_task", 12288, NULL, 3, NULL);
        }
        
        return ESP_OK;
    } else {
        ESP_LOGE(TAG, "在线获取农历日期失败");
        if (lunar_date_label) {
            lv_label_set_text(lunar_date_label, "农历获取失败");
            lv_obj_set_style_text_font(lunar_date_label, &my_font_1, 0);
        }
        return ESP_FAIL;
    }
}

/* 获取认证模式字符串 */
static const char* get_auth_mode_name(wifi_auth_mode_t authmode)
{
    switch (authmode) {
        case WIFI_AUTH_OPEN: return "Open";
        case WIFI_AUTH_WEP: return "WEP";
        case WIFI_AUTH_WPA_PSK: return "WPA";
        case WIFI_AUTH_WPA2_PSK: return "WPA2";
        case WIFI_AUTH_WPA_WPA2_PSK: return "WPA/WPA2";
        case WIFI_AUTH_WPA3_PSK: return "WPA3";
        default: return "Unknown";
    }
}

/* LVGL tick任务 */
static void lv_tick_task(void *arg)
{
    while (1) {
        lv_tick_inc(10);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

/* WiFi状态更新任务已移至wifi_status_task.c */

/* 时间更新任务 */
static void time_update_task(void *arg)
{
    ds3231_time_t time;
    char time_str[32];
    char date_str[128];  // 增加缓冲区大小以容纳字符
    char reminder_alert_str[128];
    
    while (1) {
        if (ds3231_get_time(&time) == ESP_OK) {
            /* 格式化时间字符串 - 支持12/24小时制 */
            if (use_24hour_format) {
                snprintf(time_str, sizeof(time_str), "%02d:%02d:%02d", 
                        time.hour, time.minute, time.second);
            } else {
                // 12小时制格式
                int display_hour = time.hour;
                const char* am_pm = "AM";
                
                if (display_hour == 0) {
                    display_hour = 12;  // 午夜12点
                } else if (display_hour > 12) {
                    display_hour -= 12;
                    am_pm = "PM";
                } else if (display_hour == 12) {
                    am_pm = "PM";  // 中午12点
                }
                
                snprintf(time_str, sizeof(time_str), "%02d:%02d:%02d %s", 
                        display_hour, time.minute, time.second, am_pm);
            }
            
            /* 格式化日期字符串 - 使用2024-12-27格式和中文星期 */
            snprintf(date_str, sizeof(date_str), "%04d-%02d-%02d %s", 
                    time.year, time.month, time.date, weekdays[time.day_of_week]);
            
            /* 更新显示 */
            if (time_label) {
                lv_label_set_text(time_label, time_str);
            }
            if (date_label) {
                lv_label_set_text(date_label, date_str);
                /* 使用my_font_1显示中文星期 */
                lv_obj_set_style_text_font(date_label, &my_font_1, 0);
            }
            
            /* 检查是否有临近事件并更新桌面1提醒 */
            if (reminder_valid && reminder_datetime[0] != '\0' && strlen(reminder_datetime) >= 19) {
                struct tm event_time = {0};
                struct tm current_time = {0};
                
                // 解析提醒事件的时间
                sscanf(reminder_datetime, "%d-%d-%dT%d:%d:%d",
                       &event_time.tm_year, &event_time.tm_mon, &event_time.tm_mday,
                       &event_time.tm_hour, &event_time.tm_min, &event_time.tm_sec);
                
                // 转换为标准格式
                event_time.tm_year -= 1900;   // 年份从1900年开始
                event_time.tm_mon -= 1;       // 月份从0开始
                
                // 设置当前时间
                current_time.tm_year = time.year - 1900;
                current_time.tm_mon = time.month - 1;
                current_time.tm_mday = time.date;
                current_time.tm_hour = time.hour;
                current_time.tm_min = time.minute;
                current_time.tm_sec = time.second;
                
                // 计算时间差（秒）
                time_t event_timestamp = mktime(&event_time);
                time_t current_timestamp = mktime(&current_time);
                double diff_seconds = difftime(event_timestamp, current_timestamp);
                
                // 如果事件在24小时内，在桌面1显示提醒
                if (diff_seconds > 0 && diff_seconds <= 24 * 3600) {
                    // 计算剩余小时和分钟
                    int hours_left = (int)(diff_seconds / 3600);
                    int minutes_left = (int)((diff_seconds - hours_left * 3600) / 60);
                    
                    if (hours_left > 0) {
                        snprintf(reminder_alert_str, sizeof(reminder_alert_str), 
                                "提醒: %s (%dh%dm)", 
                                reminder_title, hours_left, minutes_left);
                    } else {
                        snprintf(reminder_alert_str, sizeof(reminder_alert_str), 
                                "提醒: %s (%dm)", 
                                reminder_title, minutes_left);
                    }
                    
                    if (reminder_alert_label) {
                        lv_label_set_text(reminder_alert_label, reminder_alert_str);
                    }
                } else {
                    // 事件不在24小时内，清空提醒
                    if (reminder_alert_label) {
                        lv_label_set_text(reminder_alert_label, "");
                    }
                }
            } else {
                // 没有有效事件，清空提醒
                if (reminder_alert_label) {
                    lv_label_set_text(reminder_alert_label, "");
                }
            }
            
            ESP_LOGI(TAG, "Time: %s, Date: %s", time_str, date_str);
        } else {
            ESP_LOGE(TAG, "Failed to get time from DS3231");
        }
        
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

/* LVGL处理任务 */
static void lvgl_task(void *arg)
{
    while (1) {
        lv_timer_handler();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

/* 定时器功能相关函数 */
static void update_timer_display(void)
{
    char display_str[64];
    char status_str[64];
    char hint_str[128];
    
    switch (timer_state) {
        case TIMER_STATE_MAIN:
            snprintf(display_str, sizeof(display_str), "定时器");
            snprintf(status_str, sizeof(status_str), "00:00:00");
            snprintf(hint_str, sizeof(hint_str), "按下按键进入菜单");
            break;
            
        case TIMER_STATE_MENU:
            snprintf(display_str, sizeof(display_str), "定时器");
            snprintf(status_str, sizeof(status_str), "设置定时器");
            snprintf(hint_str, sizeof(hint_str), "按下按键开始设置");
            break;
            
        case TIMER_STATE_SET_HOUR:
            snprintf(display_str, sizeof(display_str), "设置小时");
            snprintf(status_str, sizeof(status_str), "[%02d]:%02d:%02d", timer_hours, timer_minutes, timer_seconds);
            snprintf(hint_str, sizeof(hint_str), "旋转设置，按键确认");
            break;
            
        case TIMER_STATE_SET_MINUTE:
            snprintf(display_str, sizeof(display_str), "设置分钟");
            snprintf(status_str, sizeof(status_str), "%02d:[%02d]:%02d", timer_hours, timer_minutes, timer_seconds);
            snprintf(hint_str, sizeof(hint_str), "旋转设置，按键确认");
            break;
            
        case TIMER_STATE_SET_SECOND:
            snprintf(display_str, sizeof(display_str), "设置秒");
            snprintf(status_str, sizeof(status_str), "%02d:%02d:[%02d]", timer_hours, timer_minutes, timer_seconds);
            snprintf(hint_str, sizeof(hint_str), "旋转设置，按键开始");
            break;
            
        case TIMER_STATE_COUNTDOWN:
            snprintf(display_str, sizeof(display_str), "倒计时中");
            snprintf(status_str, sizeof(status_str), "%02d:%02d:%02d", countdown_hours, countdown_minutes, countdown_seconds);
            snprintf(hint_str, sizeof(hint_str), "倒计时进行中...");
            break;
            
        case TIMER_STATE_TIME_UP:
            snprintf(display_str, sizeof(display_str), "TIME UP!");
            snprintf(status_str, sizeof(status_str), "时间到！");
            snprintf(hint_str, sizeof(hint_str), "按键回到主界面");
            break;
    }
    
    if (timer_display_label) {
        lv_label_set_text(timer_display_label, display_str);
        // 为TIME UP状态设置红色
        if (timer_state == TIMER_STATE_TIME_UP) {
            lv_obj_set_style_text_color(timer_display_label, lv_color_hex(0xFF0000), 0);
        } else {
            lv_obj_set_style_text_color(timer_display_label, lv_color_black(), 0);
        }
    }
    if (timer_status_label) {
        lv_label_set_text(timer_status_label, status_str);
        // 为TIME UP状态设置红色
        if (timer_state == TIMER_STATE_TIME_UP) {
            lv_obj_set_style_text_color(timer_status_label, lv_color_hex(0xFF0000), 0);
        } else {
            lv_obj_set_style_text_color(timer_status_label, lv_color_black(), 0);
        }
    }
    if (timer_hint_label) {
        lv_label_set_text(timer_hint_label, hint_str);
    }
}

static void timer_countdown_task(void *arg)
{
    while (1) {
        if (timer_running && timer_state == TIMER_STATE_COUNTDOWN) {
            TickType_t current_tick = xTaskGetTickCount();
            TickType_t elapsed_ticks = current_tick - timer_start_tick;
            int elapsed_seconds = elapsed_ticks / portTICK_PERIOD_MS / 1000;
            
            int total_seconds = timer_hours * 3600 + timer_minutes * 60 + timer_seconds;
            int remaining_seconds = total_seconds - elapsed_seconds;
            
            if (remaining_seconds <= 0) {
                // 时间到
                timer_running = false;
                timer_state = TIMER_STATE_TIME_UP;
                countdown_hours = 0;
                countdown_minutes = 0; 
                countdown_seconds = 0;
                ESP_LOGI(TAG, "Timer finished!");
                
                // 立即更新显示为TIME UP状态
                if (current_desktop == 1) {
                    update_timer_display();
                }
                
                // 启动震动
                start_vibration();
                
                // 播放定时器结束铃声（异步）
                ESP_LOGI(TAG, "播放定时器结束铃声");
                // 创建任务异步播放音频，避免阻塞定时器任务
                xTaskCreate(timer_audio_task, "timer_audio", 4096, NULL, 3, NULL);
            } else {
                countdown_hours = remaining_seconds / 3600;
                countdown_minutes = (remaining_seconds % 3600) / 60;
                countdown_seconds = remaining_seconds % 60;
                
                // 只有在桌面2时才更新显示
                if (current_desktop == 1) {
                    update_timer_display();
                }
            }
        }
        
        vTaskDelay(pdMS_TO_TICKS(1000)); // 每秒更新一次
    }
}

static void handle_timer_button_press(void)
{
    ESP_LOGI(TAG, "定时器按键: 状态 %d->", timer_state);
    
    /* 播放按键音效 */
    audio_player_play_pcm(beep_sound_data, beep_sound_size);
    
    switch (timer_state) {
        case TIMER_STATE_MAIN:
            timer_state = TIMER_STATE_MENU;
            break;
            
        case TIMER_STATE_MENU:
            timer_state = TIMER_STATE_SET_HOUR;
            timer_hours = 0;
            timer_minutes = 0;
            timer_seconds = 0;
            break;
            
        case TIMER_STATE_SET_HOUR:
            timer_state = TIMER_STATE_SET_MINUTE;
            break;
            
        case TIMER_STATE_SET_MINUTE:
            timer_state = TIMER_STATE_SET_SECOND;
            break;
            
        case TIMER_STATE_SET_SECOND:
            // 开始倒计时
            if (timer_hours > 0 || timer_minutes > 0 || timer_seconds > 0) {
                ESP_LOGI(TAG, "倒计时开始: %02d:%02d:%02d", timer_hours, timer_minutes, timer_seconds);
                timer_state = TIMER_STATE_COUNTDOWN;
                countdown_hours = timer_hours;
                countdown_minutes = timer_minutes;
                countdown_seconds = timer_seconds;
                timer_running = true;
                timer_start_tick = xTaskGetTickCount();
                
                // 更新Web服务器定时器状态
                web_server_update_timer_status(countdown_hours, countdown_minutes, countdown_seconds, timer_running);
            } else {
                // 如果时间为0，回到主界面
                timer_state = TIMER_STATE_MAIN;
            }
            break;
            
        case TIMER_STATE_COUNTDOWN:
            // 停止倒计时，回到主界面
            timer_running = false;
            timer_state = TIMER_STATE_MAIN;
            
            // 更新Web服务器定时器状态
            web_server_update_timer_status(countdown_hours, countdown_minutes, countdown_seconds, timer_running);
            break;
            
        case TIMER_STATE_TIME_UP:
            timer_state = TIMER_STATE_MAIN;
            break;
    }
    
    ESP_LOGI(TAG, "状态更新为: %d", timer_state);
    update_timer_display();
}

static void handle_timer_rotation(ec11_rotate_t rotate)
{
    int *value_ptr = NULL;
    int max_value = 0;
    
    switch (timer_state) {
        case TIMER_STATE_SET_HOUR:
            value_ptr = &timer_hours;
            max_value = 23;
            break;
            
        case TIMER_STATE_SET_MINUTE:
        case TIMER_STATE_SET_SECOND:
            value_ptr = (timer_state == TIMER_STATE_SET_MINUTE) ? &timer_minutes : &timer_seconds;
            max_value = 59;
            break;
            
        default:
            return; // 其他状态不处理旋转
    }
    
    if (value_ptr) {
        if (rotate == EC11_ROTATE_RIGHT) {
            *value_ptr = (*value_ptr + 1) % (max_value + 1);
        } else if (rotate == EC11_ROTATE_LEFT) {
            *value_ptr = (*value_ptr - 1 + max_value + 1) % (max_value + 1);
        }
        update_timer_display();
    }
}

/* 闹钟功能相关函数 */
static void update_alarm_display(void)
{
    char display_str[64];
    char status_str[64];
    char hint_str[128];
    char reminder_time_str[64] = {0};
    char reminder_content_str[128] = {0};
    
    // 处理闹钟显示内容
    switch (alarm_state) {
        case ALARM_STATE_MAIN:
            snprintf(display_str, sizeof(display_str), "闹钟");
            if (alarm_enabled) {
                snprintf(status_str, sizeof(status_str), "%02d:%02d ✓", alarm_hours, alarm_minutes);
                snprintf(hint_str, sizeof(hint_str), "闹钟已设置，按键修改");
            } else {
                snprintf(status_str, sizeof(status_str), "%02d:%02d", alarm_hours, alarm_minutes);
                snprintf(hint_str, sizeof(hint_str), "按下按键进入菜单");
            }
            break;
            
        case ALARM_STATE_MENU:
            snprintf(display_str, sizeof(display_str), "闹钟");
            snprintf(status_str, sizeof(status_str), "设置闹钟");
            snprintf(hint_str, sizeof(hint_str), "按下按键开始设置");
            break;
            
        case ALARM_STATE_SET_HOUR:
            snprintf(display_str, sizeof(display_str), "设置小时");
            snprintf(status_str, sizeof(status_str), "[%02d]:%02d", alarm_hours, alarm_minutes);
            snprintf(hint_str, sizeof(hint_str), "旋转设置，按键确认");
            break;
            
        case ALARM_STATE_SET_MINUTE:
            snprintf(display_str, sizeof(display_str), "设置分钟");
            snprintf(status_str, sizeof(status_str), "%02d:[%02d]", alarm_hours, alarm_minutes);
            snprintf(hint_str, sizeof(hint_str), "旋转设置，按键确认");
            break;
            
        case ALARM_STATE_ALARM_SET:
            snprintf(display_str, sizeof(display_str), "闹钟已设置");
            snprintf(status_str, sizeof(status_str), "%02d:%02d ✓", alarm_hours, alarm_minutes);
            snprintf(hint_str, sizeof(hint_str), "等待闹钟时间...");
            break;
            
        case ALARM_STATE_RINGING:
            snprintf(display_str, sizeof(display_str), "ALARM!");
            snprintf(status_str, sizeof(status_str), "闹钟响铃！");
            snprintf(hint_str, sizeof(hint_str), "按键关闭闹钟");
            break;
    }
    
    // 处理事件提醒内容
    if (reminder_valid) {
        // 解析ISO格式的日期时间 (YYYY-MM-DDThh:mm:ss)
        char date_part[16] = {0};
        char time_part[16] = {0};
        
        // 尝试提取日期和时间部分
        if (strlen(reminder_datetime) >= 19) {
            strncpy(date_part, reminder_datetime, 10);
            date_part[10] = '\0';
            strncpy(time_part, reminder_datetime + 11, 5);
            time_part[5] = '\0';
            
            snprintf(reminder_time_str, sizeof(reminder_time_str), "%s %s", date_part, time_part);
        } else {
            strcpy(reminder_time_str, "时间未知");
        }
        
        // 准备事件提醒内容
        if (reminder_description[0] == '\0') {
            snprintf(reminder_content_str, sizeof(reminder_content_str), "%s", reminder_title);
        } else {
            // 如果标题和描述都有，则显示标题和描述的前几个字符
            int max_chars = sizeof(reminder_content_str) - 10; // 预留一些空间给省略号
            if (strlen(reminder_title) + strlen(reminder_description) + 3 > max_chars) {
                // 需要截断
                int title_len = strlen(reminder_title);
                int desc_len = max_chars - title_len - 3; // 减去标题长度和": "及省略号
                
                if (desc_len > 3) {
                    // 有足够空间显示部分描述
                    snprintf(reminder_content_str, sizeof(reminder_content_str), "%s: %.*s...", 
                            reminder_title, desc_len, reminder_description);
                } else {
                    // 只能显示标题
                    snprintf(reminder_content_str, sizeof(reminder_content_str), "%s...", reminder_title);
                }
            } else {
                // 可以完整显示，但需要确保不会超出缓冲区大小
                int max_desc_len = sizeof(reminder_content_str) - strlen(reminder_title) - 3; // 为": "和'\0'预留空间
                if (max_desc_len > 0) {
                    snprintf(reminder_content_str, sizeof(reminder_content_str), "%s: %.*s", 
                            reminder_title, max_desc_len, reminder_description);
                } else {
                    // 如果标题已经占用了大部分空间，只显示标题
                    snprintf(reminder_content_str, sizeof(reminder_content_str), "%.*s", 
                            (int)sizeof(reminder_content_str) - 1, reminder_title);
                }
            }
        }
    } else {
        strcpy(reminder_time_str, "无事件");
        reminder_content_str[0] = '\0';
    }
    
    // 更新闹钟显示
    if (alarm_display_label) {
        lv_label_set_text(alarm_display_label, display_str);
        lv_obj_set_style_text_color(alarm_display_label, lv_color_black(), 0);
    }
    if (alarm_status_label) {
        lv_label_set_text(alarm_status_label, status_str);
    }
    if (alarm_hint_label) {
        lv_label_set_text(alarm_hint_label, hint_str);
    }
    
    // 更新事件提醒显示
    if (reminder_display_label) {
        lv_label_set_text(reminder_display_label, "事件提醒");
    }
    if (reminder_time_label) {
        lv_label_set_text(reminder_time_label, reminder_time_str);
    }
    if (reminder_content_label) {
        lv_label_set_text(reminder_content_label, reminder_content_str);
    }
}

static void alarm_check_task(void *arg)
{
    while (1) {
        if (alarm_enabled && !alarm_ringing) {
            ds3231_time_t current_time;
            if (ds3231_get_time(&current_time) == ESP_OK) {
                /* 检查是否到达闹钟时间 */
                if (current_time.hour == alarm_hours && current_time.minute == alarm_minutes) {
                    alarm_ringing = true;
                    alarm_state = ALARM_STATE_RINGING;
                    ESP_LOGI(TAG, "闹钟响铃！时间: %02d:%02d", alarm_hours, alarm_minutes);
                    
                    // 启动震动
                    start_vibration();
                    
                    // 播放闹钟铃声（异步）
                    ESP_LOGI(TAG, "播放闹钟铃声");
                    // 创建任务异步播放音频，避免阻塞闹钟任务
                    xTaskCreate(alarm_audio_task, "alarm_audio", 4096, NULL, 3, NULL);
                    
                    /* 只有在桌面3时才更新显示 */
                    if (current_desktop == 2) {
                        update_alarm_display();
                    }
                }
            }
        }
        
        vTaskDelay(pdMS_TO_TICKS(30000)); // 每30秒检查一次闹钟
    }
}

static void handle_alarm_button_press(void)
{
    ESP_LOGI(TAG, "闹钟按键: 状态 %d->", alarm_state);
    
    /* 播放按键音效 */
    audio_player_play_pcm(beep_sound_data, beep_sound_size);
    
    // 如果当前在主界面且正在显示事件提醒，按键将清除事件提醒
    if (alarm_state == ALARM_STATE_MAIN && reminder_valid) {
        // 清除事件提醒
        reminder_valid = false;
        ESP_LOGI(TAG, "事件提醒已清除");
        update_alarm_display();
        return;
    }
    
    switch (alarm_state) {
        case ALARM_STATE_MAIN:
            alarm_state = ALARM_STATE_MENU;
            break;
            
        case ALARM_STATE_MENU:
            alarm_state = ALARM_STATE_SET_HOUR;
            break;
            
        case ALARM_STATE_SET_HOUR:
            alarm_state = ALARM_STATE_SET_MINUTE;
            break;
            
        case ALARM_STATE_SET_MINUTE:
            /* 设置完成，启用闹钟 */
            alarm_enabled = true;
            alarm_ringing = false;
            alarm_state = ALARM_STATE_ALARM_SET;
            ESP_LOGI(TAG, "闹钟设置完成: %02d:%02d", alarm_hours, alarm_minutes);
            
            /* 更新Web服务器闹钟状态 */
            web_server_update_alarm_status(alarm_hours, alarm_minutes, alarm_enabled);
            break;
            
        case ALARM_STATE_ALARM_SET:
            /* 关闭闹钟 */
            alarm_enabled = false;
            alarm_state = ALARM_STATE_MAIN;
            ESP_LOGI(TAG, "闹钟已关闭");
            
            /* 更新Web服务器闹钟状态 */
            web_server_update_alarm_status(alarm_hours, alarm_minutes, alarm_enabled);
            break;
            
        case ALARM_STATE_RINGING:
            /* 关闭响铃 */
            alarm_ringing = false;
            alarm_enabled = false;  // 闹钟响过后自动关闭
            alarm_state = ALARM_STATE_MAIN;
            ESP_LOGI(TAG, "闹钟已关闭");
            
            /* 更新Web服务器闹钟状态 */
            web_server_update_alarm_status(alarm_hours, alarm_minutes, alarm_enabled);
            break;
    }
    
    ESP_LOGI(TAG, "状态更新为: %d", alarm_state);
    update_alarm_display();
}

static void handle_alarm_rotation(ec11_rotate_t rotate)
{
    if (alarm_state == ALARM_STATE_SET_HOUR) {
        // 设置小时
        if (rotate == EC11_ROTATE_LEFT) {
            alarm_hours--;
            if (alarm_hours < 0) alarm_hours = 23;
        } else if (rotate == EC11_ROTATE_RIGHT) {
            alarm_hours++;
            if (alarm_hours > 23) alarm_hours = 0;
        }
        update_alarm_display();
    } else if (alarm_state == ALARM_STATE_SET_MINUTE) {  
        // 设置分钟
        if (rotate == EC11_ROTATE_LEFT) {
            alarm_minutes--;
            if (alarm_minutes < 0) alarm_minutes = 59;
        } else if (rotate == EC11_ROTATE_RIGHT) {
            alarm_minutes++;
            if (alarm_minutes > 59) alarm_minutes = 0;
        }
        update_alarm_display();
    }
}

/* 创建设置页面 */
static void create_setting_page(void)
{
    if (setting_screen != NULL || setting_page_active) {
        ESP_LOGW(TAG, "设置页面已存在，跳过创建");
        return; // 页面已存在
    }
    
    ESP_LOGI(TAG, "开始创建设置页面...");
    
    /* 创建设置页面屏幕 */
    setting_screen = lv_obj_create(NULL);
    if (setting_screen == NULL) {
        ESP_LOGE(TAG, "设置页面屏幕创建失败");
        return;
    }
    lv_obj_set_style_bg_color(setting_screen, lv_color_white(), 0);
    
    /* 移除返回按钮提示文字 - 保持界面简洁 */
    
    /* 创建设置页面标题 */
    setting_title_label = lv_label_create(setting_screen);
    lv_obj_set_width(setting_title_label, LV_SIZE_CONTENT);
    lv_obj_set_height(setting_title_label, LV_SIZE_CONTENT);
    lv_obj_align(setting_title_label, LV_ALIGN_TOP_MID, 0, 20);
    lv_obj_set_style_text_color(setting_title_label, lv_color_black(), 0);
    lv_obj_set_style_text_font(setting_title_label, &my_font_1, 0);
    lv_label_set_text(setting_title_label, "设置");
    
    /* 创建设置页面显示标签 */
    setting_display_label = lv_label_create(setting_screen);
    lv_obj_set_width(setting_display_label, 220);  // 适应240px屏幕宽度，留出边距
    lv_obj_set_height(setting_display_label, LV_SIZE_CONTENT);
    lv_obj_align(setting_display_label, LV_ALIGN_TOP_MID, 0, 50);
    lv_obj_set_style_text_color(setting_display_label, lv_color_black(), 0);
    lv_obj_set_style_text_font(setting_display_label, &my_font_1, 0);
    lv_label_set_long_mode(setting_display_label, LV_LABEL_LONG_WRAP);  // 启用自动换行
    lv_obj_set_style_text_align(setting_display_label, LV_TEXT_ALIGN_LEFT, 0);  // 左对齐，适合对话显示
    lv_label_set_text(setting_display_label, "");
    
    /* 创建用户消息标签（蓝色） */
    user_message_label = lv_label_create(setting_screen);
    lv_obj_set_width(user_message_label, 220);
    lv_obj_set_height(user_message_label, LV_SIZE_CONTENT);
    lv_obj_align(user_message_label, LV_ALIGN_TOP_MID, 0, 50);
    lv_obj_set_style_text_color(user_message_label, lv_color_hex(0x007BFF), 0);  // 蓝色
    lv_obj_set_style_text_font(user_message_label, &my_font_1, 0);
    lv_label_set_long_mode(user_message_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(user_message_label, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_bg_color(user_message_label, lv_color_hex(0xF0F8FF), 0);  // 浅蓝背景
    lv_obj_set_style_radius(user_message_label, 5, 0);  // 圆角
    lv_obj_set_style_pad_all(user_message_label, 8, 0);  // 内边距
    lv_obj_add_flag(user_message_label, LV_OBJ_FLAG_HIDDEN);  // 默认隐藏
    
    /* 创建AI消息标签（绿色） */
    ai_message_label = lv_label_create(setting_screen);
    lv_obj_set_width(ai_message_label, 220);
    lv_obj_set_height(ai_message_label, LV_SIZE_CONTENT);
    lv_obj_align(ai_message_label, LV_ALIGN_TOP_MID, 0, 120);
    lv_obj_set_style_text_color(ai_message_label, lv_color_hex(0x28A745), 0);  // 绿色
    lv_obj_set_style_text_font(ai_message_label, &my_font_1, 0);
    lv_label_set_long_mode(ai_message_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(ai_message_label, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_bg_color(ai_message_label, lv_color_hex(0xF0FFF0), 0);  // 浅绿背景
    lv_obj_set_style_radius(ai_message_label, 5, 0);  // 圆角
    lv_obj_set_style_pad_all(ai_message_label, 8, 0);  // 内边距
    lv_obj_add_flag(ai_message_label, LV_OBJ_FLAG_HIDDEN);  // 默认隐藏
    
    /* 创建设置页面提示标签 */
    setting_hint_label = lv_label_create(setting_screen);
    lv_obj_set_width(setting_hint_label, LV_SIZE_CONTENT);
    lv_obj_set_height(setting_hint_label, LV_SIZE_CONTENT);
    lv_obj_align(setting_hint_label, LV_ALIGN_BOTTOM_MID, 0, -20);
    lv_obj_set_style_text_color(setting_hint_label, lv_color_black(), 0);
    lv_obj_set_style_text_font(setting_hint_label, &my_font_1, 0);
    lv_label_set_text(setting_hint_label, "");
    
    /* 切换到设置页面 */
    lv_scr_load(setting_screen);
    setting_page_active = true;
    
    /* 打印内存使用情况 */
    ESP_LOGI(TAG, "设置页面已创建并激活，当前可用堆内存: %ld 字节", 
             esp_get_free_heap_size());
}

/* 销毁设置页面并返回桌面1 */
static void destroy_setting_page(void)
{
    /* 使用静态变量防止多重调用 */
    static bool destroying = false;
    
    if (destroying) {
        ESP_LOGW(TAG, "设置页面正在销毁中，跳过重复调用");
        return;
    }
    
    if (setting_screen == NULL || !setting_page_active || setting_page_delete_requested) {
        ESP_LOGW(TAG, "设置页面不存在或已删除或删除请求已提交，跳过删除");
        return;
    }
    
    /* 验证设置屏幕对象的有效性 */
    if (!lv_obj_is_valid(setting_screen)) {
        ESP_LOGE(TAG, "设置屏幕对象已无效，清理状态变量");
        setting_screen = NULL;
        setting_display_label = NULL;
        setting_hint_label = NULL;
        setting_title_label = NULL;
        user_message_label = NULL;
        ai_message_label = NULL;
        setting_page_active = false;
        return;
    }
    
    destroying = true;
    ESP_LOGI(TAG, "开始销毁设置页面...");
    
    /* 先切换回桌面1 */
    if (lv_scr_act() == setting_screen) {
        /* 确保桌面1存在且有效 */
        if (desktop_screens[0] != NULL && lv_obj_is_valid(desktop_screens[0])) {
            lv_scr_load(desktop_screens[0]);
            ESP_LOGI(TAG, "已切换回桌面1");
        } else {
            ESP_LOGE(TAG, "桌面1无效，无法切换");
            destroying = false;
            return;
        }
    }
    
    /* 标记删除请求，使用延迟删除避免在事件回调中直接删除对象 */
    setting_page_delete_requested = true;
    
    /* 创建延迟删除定时器 */
    if (setting_delete_timer == NULL) {
        setting_delete_timer = xTimerCreate("SettingDelete", 
                                           pdMS_TO_TICKS(SETTING_DELETE_DELAY_MS),
                                           pdFALSE, NULL, setting_delete_timer_callback);
    }
    
    if (setting_delete_timer != NULL) {
        /* 停止之前的定时器，重新启动 */
        xTimerStop(setting_delete_timer, 0);
        xTimerStart(setting_delete_timer, 0);
        ESP_LOGI(TAG, "设置页面延迟删除定时器已启动");
    } else {
        ESP_LOGE(TAG, "创建延迟删除定时器失败，直接删除");
        setting_page_delete_requested = false;
        
        /* 备用直接删除 */
        setting_page_active = false;
        setting_display_label = NULL;
        setting_hint_label = NULL;
        setting_title_label = NULL;
        user_message_label = NULL;
        ai_message_label = NULL;
        
        if (setting_screen != NULL && lv_obj_is_valid(setting_screen)) {
            lv_obj_t *temp_screen = setting_screen;
            setting_screen = NULL;
            lv_obj_del(temp_screen);
        }
    }
    
    destroying = false;
}

/* 更新桌面1设置显示 */
static void update_setting_display(void)
{
    if (!setting_display_label || !setting_hint_label) return;

    if (setting_title_label) {
        switch (setting_state) {
            case SETTING_STATE_TIME_MENU:
            case SETTING_STATE_SET_YEAR:
            case SETTING_STATE_SET_MONTH:
            case SETTING_STATE_SET_DAY:
            case SETTING_STATE_SET_HOUR:
            case SETTING_STATE_SET_MINUTE:
            case SETTING_STATE_SET_SECOND:
            case SETTING_STATE_TIME_CONFIRM:
                lv_label_set_text(setting_title_label, "时间设置");
                break;
            case SETTING_STATE_PREF_MENU:
            case SETTING_STATE_TIME_FORMAT:
            case SETTING_STATE_NETWORK_TIME:
                lv_label_set_text(setting_title_label, "偏好设置");
                break;
            case SETTING_STATE_SPEECH_REC:
                lv_label_set_text(setting_title_label, "AI助手");
                break;
            default:
                lv_label_set_text(setting_title_label, "设置");
                break;
        }
    }
    
    char display_str[128];
    
    switch (setting_state) {
        case SETTING_STATE_MAIN:
            lv_label_set_text(setting_display_label, "设置");
            lv_label_set_text(setting_hint_label, "按键进入");
            break;
            
        case SETTING_STATE_MENU:
            if (main_menu_selection == 0) {
                snprintf(display_str, sizeof(display_str), "菜单: [时间] 偏好 AI助手");
            } else if (main_menu_selection == 1) {
                snprintf(display_str, sizeof(display_str), "菜单: 时间 [偏好] AI助手");
            } else {
                snprintf(display_str, sizeof(display_str), "菜单: 时间 偏好 [AI助手]");
            }
            lv_label_set_text(setting_display_label, display_str);
            lv_label_set_text(setting_hint_label, "旋转选择");
            break;
            
        case SETTING_STATE_TIME_MENU:
            lv_label_set_text(setting_display_label, "Time Settings");
            lv_label_set_text(setting_hint_label, "Press to start");
            break;
            
        case SETTING_STATE_SET_YEAR:
            snprintf(display_str, sizeof(display_str), "Year: [%04d]", setting_year);
            lv_label_set_text(setting_display_label, display_str);
            lv_label_set_text(setting_hint_label, "Rotate to adjust");
            break;
            
        case SETTING_STATE_SET_MONTH:
            snprintf(display_str, sizeof(display_str), "Month: [%02d]", setting_month);
            lv_label_set_text(setting_display_label, display_str);
            lv_label_set_text(setting_hint_label, "Rotate to adjust");
            break;
            
        case SETTING_STATE_SET_DAY:
            snprintf(display_str, sizeof(display_str), "Day: [%02d]", setting_day);
            lv_label_set_text(setting_display_label, display_str);
            lv_label_set_text(setting_hint_label, "Rotate to adjust");
            break;
            
        case SETTING_STATE_SET_HOUR:
            snprintf(display_str, sizeof(display_str), "Hour: [%02d]", setting_hour);
            lv_label_set_text(setting_display_label, display_str);
            lv_label_set_text(setting_hint_label, "Rotate to adjust");
            break;
            
        case SETTING_STATE_SET_MINUTE:
            snprintf(display_str, sizeof(display_str), "Minute: [%02d]", setting_minute);
            lv_label_set_text(setting_display_label, display_str);
            lv_label_set_text(setting_hint_label, "Rotate to adjust");
            break;
            
        case SETTING_STATE_SET_SECOND:
            snprintf(display_str, sizeof(display_str), "Second: [%02d]", setting_second);
            lv_label_set_text(setting_display_label, display_str);
            lv_label_set_text(setting_hint_label, "Rotate to adjust");
            break;
            
        case SETTING_STATE_TIME_CONFIRM:
            snprintf(display_str, sizeof(display_str), "Confirm: %04d-%02d-%02d %02d:%02d:%02d", 
                    setting_year, setting_month, setting_day, 
                    setting_hour, setting_minute, setting_second);
            lv_label_set_text(setting_display_label, display_str);
            lv_label_set_text(setting_hint_label, "Press to confirm");
            break;
            
        case SETTING_STATE_TIME_COMPLETE:
            // 在handle_setting_button_press_delayed中已经设置了显示文本
            // 这里不需要再次设置，避免覆盖成功/失败消息
            break;
            
        case SETTING_STATE_PREF_MENU:
            if (pref_menu_selection == 0) {
                snprintf(display_str, sizeof(display_str), "Pref: [格式] 网络 音量 铃声");
            } else if (pref_menu_selection == 1) {
                snprintf(display_str, sizeof(display_str), "Pref: 格式 [网络] 音量 铃声");
            } else if (pref_menu_selection == 2) {
                snprintf(display_str, sizeof(display_str), "Pref: 格式 网络 [音量] 铃声");
            } else {
                snprintf(display_str, sizeof(display_str), "Pref: 格式 网络 音量 [铃声]");
            }
            lv_label_set_text(setting_display_label, display_str);
            lv_label_set_text(setting_hint_label, "旋转选择");
            break;
            
        case SETTING_STATE_TIME_FORMAT:
            snprintf(display_str, sizeof(display_str), "Format: %s", 
                    use_24hour_format ? "[24H] 12H" : "24H [12H]");
            lv_label_set_text(setting_display_label, display_str);
            lv_label_set_text(setting_hint_label, "Rotate to toggle");
            break;
            
        case SETTING_STATE_NETWORK_TIME:
            snprintf(display_str, sizeof(display_str), "Net Time: %s", 
                    use_network_time ? "[ON] OFF" : "ON [OFF]");
            lv_label_set_text(setting_display_label, display_str);
            lv_label_set_text(setting_hint_label, "Rotate to toggle");
            break;
            
        case SETTING_STATE_VOLUME:
            snprintf(display_str, sizeof(display_str), "音量: [%d]", system_volume);
            lv_label_set_text(setting_display_label, display_str);
            lv_label_set_text(setting_hint_label, "旋转调节音量");
            break;
            
        case SETTING_STATE_RINGTONE:
            snprintf(display_str, sizeof(display_str), "铃声: %s", 
                    selected_ringtone == RINGTONE_WAV_FILE ? "[轻松] 紧急" : "轻松 [紧急]");
            lv_label_set_text(setting_display_label, display_str);
            lv_label_set_text(setting_hint_label, "旋转选择铃声");
            break;
            
        case SETTING_STATE_SPEECH_REC:
            // 显示会由speech_display_update_task更新，这里提供默认显示
            lv_label_set_text(setting_display_label, "正在初始化AI助手...");
            lv_label_set_text(setting_hint_label, "请稍等...");
            break;
            

    }
}

/* 单击定时器回调函数 */
static void single_click_timer_callback(TimerHandle_t xTimer)
{
    waiting_for_double_click = false;
    
    // 执行延迟的单击操作
    if (setting_page_active) {
        handle_setting_button_press_delayed();
    }
}

/* 时间设置完成定时器回调函数 */
static void time_setting_complete_callback(TimerHandle_t xTimer)
{
    ESP_LOGI(TAG, "时间设置完成，返回主界面");
    setting_state = SETTING_STATE_MAIN;
    destroy_setting_page();  // 销毁设置页面，返回桌面1
}

/* 延迟删除设置页面定时器回调 */
/* 安全删除LVGL对象的任务 */
static void safe_delete_task(void *arg)
{
    lv_obj_t *obj_to_delete = (lv_obj_t *)arg;
    
    /* 添加小延迟确保其他操作完成 */
    vTaskDelay(pdMS_TO_TICKS(50));
    
    /* 验证对象是否仍然有效 */
    if (obj_to_delete != NULL) {
        ESP_LOGI(TAG, "开始安全删除LVGL对象: %p", obj_to_delete);
        
        /* 双重验证对象有效性 */
        if (lv_obj_is_valid(obj_to_delete)) {
            /* 确保对象不是当前活动屏幕 */
            if (lv_scr_act() != obj_to_delete) {
                ESP_LOGI(TAG, "执行安全删除LVGL对象: %p", obj_to_delete);
                lv_obj_del(obj_to_delete);
                ESP_LOGI(TAG, "LVGL对象删除成功");
            } else {
                ESP_LOGE(TAG, "无法删除活动屏幕对象: %p", obj_to_delete);
            }
        } else {
            ESP_LOGW(TAG, "对象已无效，跳过删除: %p", obj_to_delete);
        }
    } else {
        ESP_LOGW(TAG, "删除对象为NULL，跳过删除");
    }
    
    ESP_LOGI(TAG, "设置页面删除完成，当前可用堆内存: %ld 字节", 
             esp_get_free_heap_size());
    
    /* 任务完成后删除自己 */
    vTaskDelete(NULL);
}

static void setting_delete_timer_callback(TimerHandle_t xTimer)
{
    if (setting_page_delete_requested) {
        setting_page_delete_requested = false;
        ESP_LOGI(TAG, "执行延迟删除设置页面");
        
        /* 在定时器回调中安全删除 */
        if (setting_screen != NULL) {
            lv_obj_t *temp_screen = setting_screen;
            
            /* 清理所有引用 */
            setting_screen = NULL;
            setting_display_label = NULL;
            setting_hint_label = NULL;
            setting_title_label = NULL;
            user_message_label = NULL;
            ai_message_label = NULL;
            setting_page_active = false;
            
            /* 创建任务在LVGL线程中安全删除对象 */
            BaseType_t ret = xTaskCreate(safe_delete_task, "SafeDelete", 
                                       2048, temp_screen, 5, NULL);
            if (ret != pdPASS) {
                ESP_LOGE(TAG, "创建安全删除任务失败，对象可能泄漏");
            }
        }
    }
}

/* 延迟执行的单击处理 */
static void handle_setting_button_press_delayed(void)
{
    switch (setting_state) {
        case SETTING_STATE_MAIN:
            // 进入设置菜单
            create_setting_page();  // 创建设置页面
            setting_state = SETTING_STATE_MENU;
            // 从当前时间初始化设置值
            ds3231_time_t current_time;
            if (ds3231_get_time(&current_time) == ESP_OK) {
                setting_year = current_time.year;
                setting_month = current_time.month;
                setting_day = current_time.date;
                setting_hour = current_time.hour;
                setting_minute = current_time.minute;
                setting_second = current_time.second;
            }
            update_setting_display();
            ESP_LOGI(TAG, "进入设置菜单");
            break;
            
        case SETTING_STATE_MENU:
            // 根据选择进入相应菜单
            if (main_menu_selection == 0) {
                setting_state = SETTING_STATE_TIME_MENU;
                ESP_LOGI(TAG, "进入时间设置菜单");
            } else if (main_menu_selection == 1) {
                setting_state = SETTING_STATE_PREF_MENU;
                ESP_LOGI(TAG, "进入偏好设置菜单");
            } else if (main_menu_selection == 2) {
                setting_state = SETTING_STATE_SPEECH_REC;
                start_speech_recognition();
                ESP_LOGI(TAG, "进入AI助手");
            }
            update_setting_display();
            break;
            
        case SETTING_STATE_TIME_MENU:
            // 开始设置年份
            setting_state = SETTING_STATE_SET_YEAR;
            update_setting_display();
            ESP_LOGI(TAG, "开始设置年份");
            break;
            
        case SETTING_STATE_SET_YEAR:
            // 确认年份，设置月份
            setting_state = SETTING_STATE_SET_MONTH;
            update_setting_display();
            ESP_LOGI(TAG, "确认年份: %d", setting_year);
            break;
            
        case SETTING_STATE_SET_MONTH:
            // 确认月份，设置日期
            setting_state = SETTING_STATE_SET_DAY;
            update_setting_display();
            ESP_LOGI(TAG, "确认月份: %02d", setting_month);
            break;
            
        case SETTING_STATE_SET_DAY:
            // 确认日期，设置小时
            setting_state = SETTING_STATE_SET_HOUR;
            update_setting_display();
            ESP_LOGI(TAG, "确认日期: %02d", setting_day);
            break;
            
        case SETTING_STATE_SET_HOUR:
            // 确认小时，设置分钟
            setting_state = SETTING_STATE_SET_MINUTE;
            update_setting_display();
            ESP_LOGI(TAG, "确认小时: %02d", setting_hour);
            break;
            
        case SETTING_STATE_SET_MINUTE:
            // 确认分钟，设置秒钟
            setting_state = SETTING_STATE_SET_SECOND;
            update_setting_display();
            ESP_LOGI(TAG, "确认分钟: %02d", setting_minute);
            break;
            
        case SETTING_STATE_SET_SECOND:
            // 确认秒钟，进入确认状态
            setting_state = SETTING_STATE_TIME_CONFIRM;
            update_setting_display();
            ESP_LOGI(TAG, "确认秒钟: %02d", setting_second);
            break;
            
        case SETTING_STATE_TIME_CONFIRM:
            // 确认设置，应用到DS3231
            {
                ds3231_time_t new_time;
                new_time.year = setting_year;
                new_time.month = setting_month;
                new_time.date = setting_day;
                new_time.hour = setting_hour;
                new_time.minute = setting_minute;
                new_time.second = setting_second;
                
                // 计算星期几 (简化算法)
                int day_of_week = (setting_day + ((13 * (setting_month + 1)) / 5) + 
                                 setting_year + (setting_year / 4) - (setting_year / 100) + 
                                 (setting_year / 400)) % 7;
                new_time.day_of_week = (day_of_week == 0) ? 7 : day_of_week;
                
                if (ds3231_set_time(&new_time) == ESP_OK) {
                    ESP_LOGI(TAG, "时间设置成功: %04d-%02d-%02d %02d:%02d:%02d", 
                            setting_year, setting_month, setting_day,
                            setting_hour, setting_minute, setting_second);
                    if (setting_display_label) {
                        lv_label_set_text(setting_display_label, "设置成功!");
                    }
                    if (setting_hint_label) {
                        lv_label_set_text(setting_hint_label, "时间已更新");
                    }
                    
                    /* 立即改变状态，防止重复执行 */
                    setting_state = SETTING_STATE_TIME_COMPLETE;
                    
                } else {
                    ESP_LOGE(TAG, "时间设置失败");
                    if (setting_display_label) {
                        lv_label_set_text(setting_display_label, "设置失败!");
                    }
                    if (setting_hint_label) {
                        lv_label_set_text(setting_hint_label, "请重试");
                    }
                    
                    /* 设置失败时也改变状态，防止重复执行 */
                    setting_state = SETTING_STATE_TIME_COMPLETE;
                }
                
                // 创建时间设置完成定时器，延迟返回主界面
                if (time_complete_timer == NULL) {
                    time_complete_timer = xTimerCreate("TimeComplete", 
                                                     pdMS_TO_TICKS(TIME_COMPLETE_DELAY_MS),
                                                     pdFALSE, NULL, 
                                                     time_setting_complete_callback);
                }
                
                if (time_complete_timer != NULL) {
                    /* 停止之前的定时器，重新启动 */
                    xTimerStop(time_complete_timer, 0);
                    xTimerStart(time_complete_timer, 0);
                    ESP_LOGI(TAG, "已启动返回主界面定时器");
                } else {
                    ESP_LOGE(TAG, "创建返回定时器失败，直接返回");
                    setting_state = SETTING_STATE_MAIN;
                    destroy_setting_page();
                }
            }
            break;
            
        case SETTING_STATE_TIME_COMPLETE:
            // 时间设置完成状态，等待定时器触发返回
            ESP_LOGW(TAG, "时间设置已完成，等待返回主界面");
            break;
            
        case SETTING_STATE_PREF_MENU:
            // 根据选择进入相应设置
            if (pref_menu_selection == 0) {
                setting_state = SETTING_STATE_TIME_FORMAT;
                ESP_LOGI(TAG, "进入时间格式设置");
            } else if (pref_menu_selection == 1) {
                setting_state = SETTING_STATE_NETWORK_TIME;
                ESP_LOGI(TAG, "进入网络时间设置");
            } else if (pref_menu_selection == 2) {
                setting_state = SETTING_STATE_VOLUME;
                ESP_LOGI(TAG, "进入音量设置");
            } else {
                setting_state = SETTING_STATE_RINGTONE;
                ESP_LOGI(TAG, "进入铃声设置");
            }
            update_setting_display();
            break;
            
        case SETTING_STATE_TIME_FORMAT:
            // 返回偏好设置菜单
            setting_state = SETTING_STATE_PREF_MENU;
            update_setting_display();
            ESP_LOGI(TAG, "返回偏好设置菜单");
            break;
            
        case SETTING_STATE_NETWORK_TIME:
            // 返回偏好设置菜单
            setting_state = SETTING_STATE_PREF_MENU;
            update_setting_display();
            ESP_LOGI(TAG, "返回偏好设置菜单");
            break;
            
        case SETTING_STATE_VOLUME:
            // 返回偏好设置菜单
            setting_state = SETTING_STATE_PREF_MENU;
            update_setting_display();
            ESP_LOGI(TAG, "返回偏好设置菜单");
            break;
            
        case SETTING_STATE_RINGTONE:
            // 返回偏好设置菜单
            setting_state = SETTING_STATE_PREF_MENU;
            update_setting_display();
            ESP_LOGI(TAG, "返回偏好设置菜单");
            break;
            
        case SETTING_STATE_SPEECH_REC:
            // AI助手状态下的按键处理 - 开始录音
            if (!speech_recognition_is_active()) {
                esp_err_t ret = speech_recognition_start();
                if (ret != ESP_OK) {
                    ESP_LOGE(TAG, "Failed to start AI assistant: %s", esp_err_to_name(ret));
                } else {
                    ESP_LOGI(TAG, "AI助手录音已开始");
                }
            } else {
                ESP_LOGI(TAG, "AI助手正在进行中，请等待完成");
            }
            break;
            

    }
}

/* 返回设置上一步 */
static void setting_go_back(void)
{
    switch (setting_state) {
        case SETTING_STATE_TIME_MENU:
            setting_state = SETTING_STATE_MENU;
            update_setting_display();
            ESP_LOGI(TAG, "返回主菜单");
            break;
        case SETTING_STATE_SET_YEAR:
            setting_state = SETTING_STATE_TIME_MENU;
            update_setting_display();
            ESP_LOGI(TAG, "返回时间设置菜单");
            break;
        case SETTING_STATE_SET_MONTH:
            setting_state = SETTING_STATE_SET_YEAR;
            update_setting_display();
            ESP_LOGI(TAG, "返回设置年份");
            break;
        case SETTING_STATE_SET_DAY:
            setting_state = SETTING_STATE_SET_MONTH;
            update_setting_display();
            ESP_LOGI(TAG, "返回设置月份");
            break;
        case SETTING_STATE_SET_HOUR:
            setting_state = SETTING_STATE_SET_DAY;
            update_setting_display();
            ESP_LOGI(TAG, "返回设置日期");
            break;
        case SETTING_STATE_SET_MINUTE:
            setting_state = SETTING_STATE_SET_HOUR;
            update_setting_display();
            ESP_LOGI(TAG, "返回设置小时");
            break;
        case SETTING_STATE_SET_SECOND:
            setting_state = SETTING_STATE_SET_MINUTE;
            update_setting_display();
            ESP_LOGI(TAG, "返回设置分钟");
            break;
        case SETTING_STATE_TIME_CONFIRM:
            setting_state = SETTING_STATE_SET_SECOND;
            update_setting_display();
            ESP_LOGI(TAG, "返回设置秒钟");
            break;
        case SETTING_STATE_PREF_MENU:
            setting_state = SETTING_STATE_MENU;
            update_setting_display();
            ESP_LOGI(TAG, "返回主菜单");
            break;
        case SETTING_STATE_TIME_FORMAT:
            setting_state = SETTING_STATE_PREF_MENU;
            update_setting_display();
            ESP_LOGI(TAG, "返回偏好设置菜单");
            break;
        case SETTING_STATE_NETWORK_TIME:
            setting_state = SETTING_STATE_PREF_MENU;
            update_setting_display();
            ESP_LOGI(TAG, "返回偏好设置菜单");
            break;
        case SETTING_STATE_VOLUME:
            setting_state = SETTING_STATE_PREF_MENU;
            update_setting_display();
            ESP_LOGI(TAG, "返回偏好设置菜单");
            break;
        case SETTING_STATE_SPEECH_REC:
            // 停止AI助手，返回主菜单
            stop_speech_recognition();
            setting_state = SETTING_STATE_MENU;
            update_setting_display();
            ESP_LOGI(TAG, "AI助手已停止，返回主菜单");
            break;
            

        default:
            // 其他状态下双击退出设置页面
            destroy_setting_page();
            setting_state = SETTING_STATE_MAIN;
            ESP_LOGI(TAG, "双击退出设置页面");
            break;
    }
}

/* 处理桌面1设置按钮事件 */
static void handle_setting_button_press(void)
{
    uint32_t current_time = xTaskGetTickCount() * portTICK_PERIOD_MS;
    
    // 在设置页面时处理双击逻辑
    if (setting_page_active) {
        /* 在时间设置完成状态下，阻止按键处理，避免重复执行 */
        if (setting_state == SETTING_STATE_TIME_COMPLETE) {
            ESP_LOGW(TAG, "时间设置已完成，忽略按键事件");
            return;
        }
        
        if (waiting_for_double_click && (current_time - last_key_press_time <= DOUBLE_CLICK_INTERVAL_MS)) {
            // 检测到双击
            waiting_for_double_click = false;
            if (single_click_timer != NULL) {
                xTimerStop(single_click_timer, 0);  // 停止单击定时器
            }
            
            // 执行双击逻辑（返回上一步）
            setting_go_back();
            ESP_LOGI(TAG, "检测到双击，返回上一步");
            return;
        } else {
            // 第一次按键，启动延迟定时器
            waiting_for_double_click = true;
            last_key_press_time = current_time;
            
            // 创建或重启单击定时器
            if (single_click_timer == NULL) {
                single_click_timer = xTimerCreate("SingleClick", 
                                                 pdMS_TO_TICKS(DOUBLE_CLICK_INTERVAL_MS),
                                                 pdFALSE, NULL, single_click_timer_callback);
            }
            
            if (single_click_timer != NULL) {
                xTimerStart(single_click_timer, 0);
            }
            
            ESP_LOGI(TAG, "等待双击检测...");
            return;
        }
    }
    
         // 非设置页面的按钮处理（桌面1进入设置）
    if (setting_state == SETTING_STATE_MAIN && !setting_page_active) {
        /* 播放按键音效 */
        audio_player_play_pcm(beep_sound_data, beep_sound_size);
        
        // 直接进入设置页面（无需延迟）
        create_setting_page();  // 创建设置页面
        setting_state = SETTING_STATE_MENU;
        // 从当前时间初始化设置值
        ds3231_time_t current_time_val;
        if (ds3231_get_time(&current_time_val) == ESP_OK) {
            setting_year = current_time_val.year;
            setting_month = current_time_val.month;
            setting_day = current_time_val.date;
            setting_hour = current_time_val.hour;
            setting_minute = current_time_val.minute;
            setting_second = current_time_val.second;
        }
        update_setting_display();
        ESP_LOGI(TAG, "进入设置菜单");
    }
}

/* 处理桌面1设置旋转事件 */
static void handle_setting_rotation(ec11_rotate_t rotate)
{
    switch (setting_state) {
                case SETTING_STATE_MENU:
            if (rotate == EC11_ROTATE_LEFT) {
                main_menu_selection--;
                if (main_menu_selection < 0) main_menu_selection = 2;
            } else if (rotate == EC11_ROTATE_RIGHT) {
                main_menu_selection++;
                if (main_menu_selection > 2) main_menu_selection = 0;
            }
            update_setting_display();
            break;
            
        case SETTING_STATE_PREF_MENU:
            if (rotate == EC11_ROTATE_LEFT) {
                pref_menu_selection--;
                if (pref_menu_selection < 0) pref_menu_selection = 3;
            } else if (rotate == EC11_ROTATE_RIGHT) {
                pref_menu_selection++;
                if (pref_menu_selection > 3) pref_menu_selection = 0;
            }
            update_setting_display();
            break;
            
        case SETTING_STATE_TIME_FORMAT:
            // 旋转切换时间格式
            use_24hour_format = !use_24hour_format;
            update_setting_display();
            ESP_LOGI(TAG, "时间格式切换为: %s", use_24hour_format ? "24小时制" : "12小时制");
            
            // 更新Web服务器时间格式状态
            web_server_update_clock_status(use_24hour_format);
            break;
            
        case SETTING_STATE_NETWORK_TIME:
            // 旋转切换网络时间设置
            use_network_time = !use_network_time;
            update_setting_display();
            ESP_LOGI(TAG, "网络时间设置切换为: %s", use_network_time ? "开启" : "关闭");
            break;
            
        case SETTING_STATE_VOLUME:
            // 旋转调节音量
            if (rotate == EC11_ROTATE_LEFT) {
                system_volume -= 5;
                if (system_volume < 0) system_volume = 0;
            } else if (rotate == EC11_ROTATE_RIGHT) {
                system_volume += 5;
                if (system_volume > 100) system_volume = 100;
            }
            // 应用音量设置并播放测试音效
            audio_player_set_volume(system_volume);
            audio_player_play_pcm(beep_sound_data, beep_sound_size);
            update_setting_display();
            ESP_LOGI(TAG, "音量调节为: %d%%", system_volume);
            break;
            
        case SETTING_STATE_RINGTONE:
            // 旋转切换铃声类型
            selected_ringtone = (selected_ringtone == RINGTONE_WAV_FILE) ? RINGTONE_BUILTIN_TONE : RINGTONE_WAV_FILE;
            update_setting_display();
            
            // 播放选中的铃声预览（短暂播放）
            if (selected_ringtone == RINGTONE_WAV_FILE) {
                ESP_LOGI(TAG, "切换到WAV文件铃声，播放预览");
                audio_play_wav_file("ring.wav", system_volume);
            } else {
                ESP_LOGI(TAG, "切换到内置音调铃声，播放预览");
                audio_player_play_pcm(alarm_tone_data, alarm_tone_size);
            }
            break;
        case SETTING_STATE_SET_YEAR:
            if (rotate == EC11_ROTATE_LEFT) {
                setting_year--;
                if (setting_year < 2000) setting_year = 2000;
            } else if (rotate == EC11_ROTATE_RIGHT) {
                setting_year++;
                if (setting_year > 2100) setting_year = 2100;
            }
            update_setting_display();
            break;
            
        case SETTING_STATE_SET_MONTH:
            if (rotate == EC11_ROTATE_LEFT) {
                setting_month--;
                if (setting_month < 1) setting_month = 12;
            } else if (rotate == EC11_ROTATE_RIGHT) {
                setting_month++;
                if (setting_month > 12) setting_month = 1;
            }
            update_setting_display();
            break;
            
        case SETTING_STATE_SET_DAY:
            if (rotate == EC11_ROTATE_LEFT) {
                setting_day--;
                if (setting_day < 1) setting_day = 31;  // 简化处理，不考虑每月天数差异
            } else if (rotate == EC11_ROTATE_RIGHT) {
                setting_day++;
                if (setting_day > 31) setting_day = 1;
            }
            update_setting_display();
            break;
            
        case SETTING_STATE_SET_HOUR:
            if (rotate == EC11_ROTATE_LEFT) {
                setting_hour--;
                if (setting_hour < 0) setting_hour = 23;
            } else if (rotate == EC11_ROTATE_RIGHT) {
                setting_hour++;
                if (setting_hour > 23) setting_hour = 0;
            }
            update_setting_display();
            break;
            
        case SETTING_STATE_SET_MINUTE:
            if (rotate == EC11_ROTATE_LEFT) {
                setting_minute--;
                if (setting_minute < 0) setting_minute = 59;
            } else if (rotate == EC11_ROTATE_RIGHT) {
                setting_minute++;
                if (setting_minute > 59) setting_minute = 0;
            }
            update_setting_display();
            break;
            
        case SETTING_STATE_SET_SECOND:
            if (rotate == EC11_ROTATE_LEFT) {
                setting_second--;
                if (setting_second < 0) setting_second = 59;
            } else if (rotate == EC11_ROTATE_RIGHT) {
                setting_second++;
                if (setting_second > 59) setting_second = 0;
            }
            update_setting_display();
            break;
            
        default:
            // 其他状态不处理旋转
            break;
    }
}

/* 天气预报功能相关函数 */
/* 创建桌面点状指示器 */
static void create_desktop_dots(lv_obj_t *parent_screen, int desktop_index)
{
    /* 创建点容器 */
    dot_containers[desktop_index] = lv_obj_create(parent_screen);
    lv_obj_set_size(dot_containers[desktop_index], 80, 20);  // 容器大小
    lv_obj_align(dot_containers[desktop_index], LV_ALIGN_BOTTOM_MID, 0, -10);  // 底部居中
    lv_obj_set_style_bg_opa(dot_containers[desktop_index], LV_OPA_TRANSP, 0);  // 透明背景
    lv_obj_set_style_border_opa(dot_containers[desktop_index], LV_OPA_TRANSP, 0);  // 透明边框
    lv_obj_set_style_pad_all(dot_containers[desktop_index], 0, 0);  // 无内边距
    
    /* 创建4个点 */
    for (int i = 0; i < DESKTOP_COUNT; i++) {
        desktop_dots[desktop_index][i] = lv_obj_create(dot_containers[desktop_index]);
        lv_obj_set_size(desktop_dots[desktop_index][i], 8, 8);  // 点的大小
        lv_obj_align(desktop_dots[desktop_index][i], LV_ALIGN_LEFT_MID, i * 15 + 10, 0);  // 水平排列，间距15像素
        lv_obj_set_style_radius(desktop_dots[desktop_index][i], LV_RADIUS_CIRCLE, 0);  // 圆形
        lv_obj_set_style_border_width(desktop_dots[desktop_index][i], 1, 0);  // 边框宽度
        lv_obj_set_style_border_color(desktop_dots[desktop_index][i], lv_color_black(), 0);  // 黑色边框
        
        /* 根据当前桌面设置点的状态 */
        if (i == desktop_index) {
            /* 当前桌面：实心点 */
            lv_obj_set_style_bg_opa(desktop_dots[desktop_index][i], LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(desktop_dots[desktop_index][i], lv_color_black(), 0);
        } else {
            /* 其他桌面：空心点 */
            lv_obj_set_style_bg_opa(desktop_dots[desktop_index][i], LV_OPA_TRANSP, 0);
        }
    }
}

/* 更新桌面点状指示器 */
static void update_desktop_dots(int current_desktop_index)
{
    /* 更新所有桌面的点状态 */
    for (int desktop = 0; desktop < DESKTOP_COUNT; desktop++) {
        for (int dot = 0; dot < DESKTOP_COUNT; dot++) {
            if (dot == current_desktop_index) {
                /* 当前桌面：实心点 */
                lv_obj_set_style_bg_opa(desktop_dots[desktop][dot], LV_OPA_COVER, 0);
                lv_obj_set_style_bg_color(desktop_dots[desktop][dot], lv_color_black(), 0);
            } else {
                /* 其他桌面：空心点 */
                lv_obj_set_style_bg_opa(desktop_dots[desktop][dot], LV_OPA_TRANSP, 0);
            }
        }
    }
}

/* 更新气温图表数据 */


/* 更新桌面4显示状态 */
static void update_forecast_view(void)
{
    if (!forecast_display_label) {
        return;
    }
    
    // 只显示文字预报
    lv_obj_clear_flag(forecast_display_label, LV_OBJ_FLAG_HIDDEN);
}

/* 桌面4按键处理 */
static void handle_forecast_button_press(void)
{
    /* 播放按键音效 */
    audio_player_play_pcm(beep_sound_data, beep_sound_size);
    
    // 桌面4只有文字显示，无需控制模式
    ESP_LOGI(TAG, "桌面4: 天气预报显示");
}



static void update_forecast_display(void)
{
    if (!forecast_updated || !forecast_display_label) {
        return;
    }
    
    char display_text[1024];
    int pos = 0;
    
    // 添加城市信息
    pos += snprintf(display_text + pos, sizeof(display_text) - pos, 
                   "%s天气预报\n\n", forecast_city);
    
    // 显示预报数据（改用纵向卡片式布局，避免对齐问题）
    for (int i = 0; i < 3 && i < 4; i++) {
        if (strlen(forecast_data[i].date) > 0) {
            // 提取月日信息
            char month_day[8] = "";
            if (strlen(forecast_data[i].date) >= 10) {
                snprintf(month_day, sizeof(month_day), "%.2s/%.2s", 
                        forecast_data[i].date + 5, forecast_data[i].date + 8);
            }
            
            // 卡片式显示格式
            pos += snprintf(display_text + pos, sizeof(display_text) - pos,
                          "%s %s\n%s %s~%s° %s%s级\n",
                          month_day, forecast_data[i].week,
                          forecast_data[i].dayweather,
                          forecast_data[i].nighttemp, forecast_data[i].daytemp,
                          forecast_data[i].daywind, forecast_data[i].daypower);
            
            // 添加分隔线（除了最后一天）
            if (i < 2) {
                pos += snprintf(display_text + pos, sizeof(display_text) - pos, "\n");
            }
        }
    }
    
    lv_label_set_text(forecast_display_label, display_text);
}

/* 桌面切换函数 */
static void switch_desktop(int target_desktop)
{
    if (target_desktop < 0 || target_desktop >= DESKTOP_COUNT) {
        ESP_LOGE(TAG, "无效桌面: %d", target_desktop);
        return;
    }
    
    if (target_desktop == current_desktop) {
        return; // 已经在目标桌面
    }
    
    ESP_LOGI(TAG, "切换桌面: %d->%d", current_desktop, target_desktop);
    
    /* 切换到目标桌面屏幕 */
    lv_scr_load(desktop_screens[target_desktop]);
    
    /* 更新当前桌面编号 */
    current_desktop = target_desktop;
    
    /* 如果切换到桌面2，重置定时器状态到主界面 */
    if (target_desktop == 1) {
        timer_state = TIMER_STATE_MAIN;
        update_timer_display();
    }
    
    /* 如果切换到桌面3，重置闹钟状态到主界面 */
    if (target_desktop == 2) {
        alarm_state = ALARM_STATE_MAIN;
        update_alarm_display();
    }
    
    /* 如果切换到桌面4，更新天气预报显示 */
    if (target_desktop == 3) {
        forecast_state = FORECAST_STATE_TEXT;
        update_forecast_display();
        update_forecast_view();
    }
    
    /* 更新点状指示器 */
    update_desktop_dots(target_desktop);
}

/* EC11事件处理回调函数 */
static void ec11_event_callback(ec11_event_t *event)
{
    // 简化调试日志，减少栈使用
    if (event->rotate != EC11_ROTATE_NONE) {
        ESP_LOGI(TAG, "旋转事件: %d, 桌面: %d", event->rotate, current_desktop);
    }
    if (event->key != EC11_KEY_NONE) {
        ESP_LOGI(TAG, "按键事件: %d, 桌面: %d", event->key, current_desktop);
    }
    
    /* 优先处理按键事件，如果有按键事件则忽略旋转事件 */
    if (event->key == EC11_KEY_PRESSED) {
        if (setting_page_active) {
            // 在设置页面时处理设置按键
            ESP_LOGI(TAG, "处理设置按键");
            handle_setting_button_press();
        } else if (current_desktop == 0) {
            // 在桌面1时处理设置按键
            ESP_LOGI(TAG, "处理设置按键");
            handle_setting_button_press();
        } else if (current_desktop == 1) {
            // 在桌面2时处理定时器按键
            ESP_LOGI(TAG, "处理定时器按键");
            handle_timer_button_press();
        } else if (current_desktop == 2) {
            // 在桌面3时处理闹钟按键
            ESP_LOGI(TAG, "处理闹钟按键");
            handle_alarm_button_press();
        } else if (current_desktop == 3) {
            // 在桌面4时处理天气预报按键
            ESP_LOGI(TAG, "处理天气预报按键");
            handle_forecast_button_press();
        }
        return; // 有按键事件时，忽略旋转事件
    }
    
    /* 处理旋转事件（只有在没有按键事件时） */
    if (event->rotate != EC11_ROTATE_NONE) {
        if (setting_page_active && setting_state == SETTING_STATE_MAIN) {
            // 在设置页面主状态时，旋转退出设置页面
            destroy_setting_page();
            ESP_LOGI(TAG, "通过旋转退出设置页面");
        } else if (setting_page_active && setting_state != SETTING_STATE_MAIN) {
            // 在设置页面且不在主状态时，处理设置旋转
            handle_setting_rotation(event->rotate);
        } else if (current_desktop == 1 && timer_state != TIMER_STATE_MAIN) {
            // 在桌面2且不在主状态时，处理定时器旋转
            handle_timer_rotation(event->rotate);
        } else if (current_desktop == 2 && alarm_state != ALARM_STATE_MAIN && alarm_state != ALARM_STATE_ALARM_SET && alarm_state != ALARM_STATE_RINGING) {
            // 在桌面3且在设置状态时，处理闹钟旋转
            handle_alarm_rotation(event->rotate);
        } else {
            // 桌面切换
            if (event->rotate == EC11_ROTATE_LEFT) {
                /* 左旋，切换到前一个桌面 */
                int target = current_desktop - 1;
                if (target < 0) {
                    target = DESKTOP_COUNT - 1; // 循环到最后一个桌面
                }
                switch_desktop(target);
            } else if (event->rotate == EC11_ROTATE_RIGHT) {
                /* 右旋，切换到下一个桌面 */
                int target = current_desktop + 1;
                if (target >= DESKTOP_COUNT) {
                    target = 0; // 循环到第一个桌面
                }
                switch_desktop(target);
            }
        }
    }
}

/* 创建桌面1 - 时间天气桌面 */
static void create_desktop1(void)
{
    lv_obj_t *screen1 = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(screen1, lv_color_white(), 0);
    desktop_screens[0] = screen1;
    
    /* 创建标题标签 - 使用全局变量以便修改颜色 */
    title_label = lv_label_create(screen1);
    lv_obj_set_width(title_label, LV_SIZE_CONTENT);
    lv_obj_set_height(title_label, LV_SIZE_CONTENT);
    lv_obj_align(title_label, LV_ALIGN_TOP_MID, 0, 10);
    lv_obj_set_style_text_color(title_label, lv_color_black(), 0);
    lv_obj_set_style_text_font(title_label, &my_font_1, 0);
    lv_label_set_text(title_label, "智能桌面助手");
    
    /* 创建WiFi状态标签 */
    wifi_status_label = lv_label_create(screen1);
    lv_obj_set_width(wifi_status_label, LV_SIZE_CONTENT);
    lv_obj_set_height(wifi_status_label, LV_SIZE_CONTENT);
    lv_obj_align(wifi_status_label, LV_ALIGN_TOP_MID, 0, 35);
    lv_obj_set_style_text_color(wifi_status_label, lv_color_black(), 0);
    lv_obj_set_style_text_font(wifi_status_label, &lv_font_montserrat_14, 0);
    lv_label_set_text(wifi_status_label, "WiFi: Initializing...");
    
    /* 创建时间标签 */
    time_label = lv_label_create(screen1);
    lv_obj_set_width(time_label, LV_SIZE_CONTENT);
    lv_obj_set_height(time_label, LV_SIZE_CONTENT);
    lv_obj_align(time_label, LV_ALIGN_CENTER, 0, -40);
    lv_obj_set_style_text_color(time_label, lv_color_black(), 0);
    lv_obj_set_style_text_font(time_label, &lv_font_montserrat_20, 0);
    lv_label_set_text(time_label, "00:00:00");
    
    /* 创建时间同步状态标签 */
    sync_status_label = lv_label_create(screen1);
    lv_obj_set_width(sync_status_label, LV_SIZE_CONTENT);
    lv_obj_set_height(sync_status_label, LV_SIZE_CONTENT);
    lv_obj_align(sync_status_label, LV_ALIGN_CENTER, 80, -40);
    lv_obj_set_style_text_color(sync_status_label, lv_color_black(), 0);
    lv_obj_set_style_text_font(sync_status_label, &lv_font_montserrat_14, 0);
    lv_label_set_text(sync_status_label, "");
    
    /* 创建日期标签 */
    date_label = lv_label_create(screen1);
    lv_obj_set_width(date_label, LV_SIZE_CONTENT);
    lv_obj_set_height(date_label, LV_SIZE_CONTENT);
    lv_obj_align(date_label, LV_ALIGN_CENTER, 0, -10);
    lv_obj_set_style_text_color(date_label, lv_color_black(), 0);
    lv_obj_set_style_text_font(date_label, &my_font_1, 0);
    lv_label_set_text(date_label, "2024-12-27 星期五");
    
    /* 创建农历日期标签 */
    lunar_date_label = lv_label_create(screen1);
    lv_obj_set_width(lunar_date_label, LV_SIZE_CONTENT);
    lv_obj_set_height(lunar_date_label, LV_SIZE_CONTENT);
    lv_obj_align(lunar_date_label, LV_ALIGN_CENTER, 0, 10);
    lv_obj_set_style_text_color(lunar_date_label, lv_color_black(), 0);
    lv_obj_set_style_text_font(lunar_date_label, &my_font_1, 0);
    lv_label_set_text(lunar_date_label, "农历等待获取...");
    
    /* 创建临近提醒标签 - 位置调整到原来的设置提示位置 */
    reminder_alert_label = lv_label_create(screen1);
    lv_obj_set_width(reminder_alert_label, 220);  // 设置宽度限制
    lv_obj_set_height(reminder_alert_label, LV_SIZE_CONTENT);
    lv_obj_align(reminder_alert_label, LV_ALIGN_CENTER, 0, 110);  // 调整到原"按下按键进入设置"位置
    lv_obj_set_style_text_color(reminder_alert_label, lv_color_hex(0x0000FF), 0);  // 蓝色
    lv_obj_set_style_text_font(reminder_alert_label, &my_font_1, 0);
    lv_obj_set_style_text_align(reminder_alert_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(reminder_alert_label, "");  // 初始为空
    
    /* 创建天气标签 */
    weather_label = lv_label_create(screen1);
    lv_obj_set_width(weather_label, LV_SIZE_CONTENT);
    lv_obj_set_height(weather_label, LV_SIZE_CONTENT);
    lv_obj_align(weather_label, LV_ALIGN_CENTER, 0, 40);  // 上移一行
    lv_obj_set_style_text_color(weather_label, lv_color_black(), 0);
    lv_obj_set_style_text_font(weather_label, &my_font_1, 0);
    lv_label_set_text(weather_label, "天气: 等待连接...");
    
    /* 创建WiFi扫描结果标签（隐藏） */
    wifi_scan_label = lv_label_create(screen1);
    lv_obj_set_width(wifi_scan_label, 300);
    lv_obj_set_height(wifi_scan_label, 100);  // 设置固定高度
    lv_obj_align(wifi_scan_label, LV_ALIGN_BOTTOM_MID, 0, -20);
    lv_obj_set_style_text_color(wifi_scan_label, lv_color_black(), 0);
    lv_obj_set_style_text_font(wifi_scan_label, &lv_font_montserrat_14, 0);  // 使用14号字体
    lv_label_set_long_mode(wifi_scan_label, LV_LABEL_LONG_SCROLL_CIRCULAR);  // 设置文字滚动模式
    lv_obj_set_style_text_align(wifi_scan_label, LV_TEXT_ALIGN_CENTER, 0);   // 居中对齐
    lv_obj_add_flag(wifi_scan_label, LV_OBJ_FLAG_HIDDEN);  // 隐藏WiFi扫描结果
    
    /* 创建MQ2烟雾传感器显示标签 */
    mq2_label = lv_label_create(screen1);
    lv_obj_set_width(mq2_label, LV_SIZE_CONTENT);
    lv_obj_set_height(mq2_label, LV_SIZE_CONTENT);
    lv_obj_align(mq2_label, LV_ALIGN_CENTER, 0, 65);  // 上移一行
    lv_obj_set_style_text_color(mq2_label, lv_color_make(0, 160, 0), 0); // 初始为绿色
    lv_obj_set_style_text_font(mq2_label, &my_font_1, 0);
    lv_label_set_text(mq2_label, "空气质量: 正常");
    
    /* 已移除设置按钮提示文本 */
    
    /* 创建桌面1的点状指示器 */
    create_desktop_dots(screen1, 0);
}

/* 创建桌面2 - 定时器桌面 */
static void create_desktop2(void)
{
    lv_obj_t *screen2 = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(screen2, lv_color_white(), 0); // 白色背景
    desktop_screens[1] = screen2;
    
    /* 创建定时器标题 */
    timer_display_label = lv_label_create(screen2);
    lv_obj_set_width(timer_display_label, LV_SIZE_CONTENT);
    lv_obj_set_height(timer_display_label, LV_SIZE_CONTENT);
    lv_obj_align(timer_display_label, LV_ALIGN_CENTER, 0, -60);
    lv_obj_set_style_text_color(timer_display_label, lv_color_black(), 0);
    lv_obj_set_style_text_font(timer_display_label, &my_font_1, 0);
    lv_label_set_text(timer_display_label, "定时器");
    
    /* 创建时间显示 */
    timer_status_label = lv_label_create(screen2);
    lv_obj_set_width(timer_status_label, LV_SIZE_CONTENT);
    lv_obj_set_height(timer_status_label, LV_SIZE_CONTENT);
    lv_obj_align(timer_status_label, LV_ALIGN_CENTER, 0, -20);
    lv_obj_set_style_text_color(timer_status_label, lv_color_black(), 0);
    lv_obj_set_style_text_font(timer_status_label, &lv_font_montserrat_20, 0);
    lv_label_set_text(timer_status_label, "00:00:00");
    
    /* 创建操作提示 */
    timer_hint_label = lv_label_create(screen2);
    lv_obj_set_width(timer_hint_label, LV_SIZE_CONTENT);
    lv_obj_set_height(timer_hint_label, LV_SIZE_CONTENT);
    lv_obj_align(timer_hint_label, LV_ALIGN_CENTER, 0, 20);
    lv_obj_set_style_text_color(timer_hint_label, lv_color_black(), 0);
    lv_obj_set_style_text_font(timer_hint_label, &my_font_1, 0);
    lv_label_set_text(timer_hint_label, "按下按键进入菜单");
    
    /* 创建桌面2的点状指示器 */
    create_desktop_dots(screen2, 1);
    
    /* 初始化定时器显示 */
    update_timer_display();
}

/* 创建桌面3 - 闹钟桌面 */
static void create_desktop3(void)
{
    lv_obj_t *screen3 = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(screen3, lv_color_white(), 0); // 白色背景
    desktop_screens[2] = screen3;
    
    /* 创建闹钟标题 */
    alarm_display_label = lv_label_create(screen3);
    lv_obj_set_width(alarm_display_label, LV_SIZE_CONTENT);
    lv_obj_set_height(alarm_display_label, LV_SIZE_CONTENT);
    lv_obj_align(alarm_display_label, LV_ALIGN_TOP_MID, 0, 20);  // 移动到上方
    lv_obj_set_style_text_color(alarm_display_label, lv_color_black(), 0);
    lv_obj_set_style_text_font(alarm_display_label, &my_font_1, 0);
    lv_label_set_text(alarm_display_label, "闹钟");
    
    /* 创建闹钟时间显示 */
    alarm_status_label = lv_label_create(screen3);
    lv_obj_set_width(alarm_status_label, LV_SIZE_CONTENT);
    lv_obj_set_height(alarm_status_label, LV_SIZE_CONTENT);
    lv_obj_align(alarm_status_label, LV_ALIGN_TOP_MID, 0, 50);  // 调整位置
    lv_obj_set_style_text_color(alarm_status_label, lv_color_black(), 0);
    lv_obj_set_style_text_font(alarm_status_label, &lv_font_montserrat_20, 0);
    lv_label_set_text(alarm_status_label, "07:00");
    
    /* 创建闹钟操作提示 */
    alarm_hint_label = lv_label_create(screen3);
    lv_obj_set_width(alarm_hint_label, LV_SIZE_CONTENT);
    lv_obj_set_height(alarm_hint_label, LV_SIZE_CONTENT);
    lv_obj_align(alarm_hint_label, LV_ALIGN_TOP_MID, 0, 80);  // 调整位置
    lv_obj_set_style_text_color(alarm_hint_label, lv_color_black(), 0);
    lv_obj_set_style_text_font(alarm_hint_label, &my_font_1, 0);
    lv_label_set_text(alarm_hint_label, "按下按键进入菜单");
    
    /* 创建事件提醒标题 */
    reminder_display_label = lv_label_create(screen3);
    lv_obj_set_width(reminder_display_label, LV_SIZE_CONTENT);
    lv_obj_set_height(reminder_display_label, LV_SIZE_CONTENT);
    lv_obj_align(reminder_display_label, LV_ALIGN_TOP_MID, 0, 120);  // 在闹钟下方
    lv_obj_set_style_text_color(reminder_display_label, lv_color_hex(0x0000FF), 0);  // 蓝色
    lv_obj_set_style_text_font(reminder_display_label, &my_font_1, 0);
    lv_label_set_text(reminder_display_label, "事件提醒");
    
    /* 创建事件提醒时间显示 */
    reminder_time_label = lv_label_create(screen3);
    lv_obj_set_width(reminder_time_label, LV_SIZE_CONTENT);
    lv_obj_set_height(reminder_time_label, LV_SIZE_CONTENT);
    lv_obj_align(reminder_time_label, LV_ALIGN_TOP_MID, 0, 150);
    lv_obj_set_style_text_color(reminder_display_label, lv_color_hex(0x0000FF), 0);
    lv_obj_set_style_text_font(reminder_time_label, &lv_font_montserrat_16, 0);
    lv_label_set_text(reminder_time_label, "无事件");
    
    /* 创建事件提醒内容显示 */
    reminder_content_label = lv_label_create(screen3);
    lv_obj_set_width(reminder_content_label, 220);  // 设置宽度限制
    lv_obj_set_height(reminder_content_label, LV_SIZE_CONTENT);
    lv_obj_align(reminder_content_label, LV_ALIGN_TOP_MID, 0, 180);
    lv_obj_set_style_text_color(reminder_content_label, lv_color_black(), 0);
    lv_obj_set_style_text_font(reminder_content_label, &my_font_1, 0);
    lv_obj_set_style_text_align(reminder_content_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(reminder_content_label, "");
    
    /* 创建桌面3的点状指示器 */
    create_desktop_dots(screen3, 2);
    
    /* 初始化闹钟显示 */
    update_alarm_display();
}

/* 创建桌面4 - 天气预报桌面 */
static void create_desktop4(void)
{
    lv_obj_t *screen4 = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(screen4, lv_color_white(), 0);
    desktop_screens[3] = screen4;
    
    /* 创建天气标题 - 针对240*320屏幕优化位置 */
    lv_obj_t *title_label = lv_label_create(screen4);
    lv_obj_set_width(title_label, LV_SIZE_CONTENT);
    lv_obj_set_height(title_label, LV_SIZE_CONTENT);
    lv_obj_align(title_label, LV_ALIGN_TOP_MID, 0, 10);  // 调整位置
    lv_obj_set_style_text_color(title_label, lv_color_black(), 0);
    lv_obj_set_style_text_font(title_label, &my_font_1, 0);
    lv_label_set_text(title_label, "天气预报");
    
    /* 创建天气预报显示 - 居中布局 */
    forecast_display_label = lv_label_create(screen4);
    lv_obj_set_width(forecast_display_label, 220);  // 限制宽度以适配240像素屏幕
    lv_obj_set_height(forecast_display_label, LV_SIZE_CONTENT);
    lv_obj_align(forecast_display_label, LV_ALIGN_CENTER, 0, -30);  // 上移，为室内温湿度留出空间
    lv_obj_set_style_text_color(forecast_display_label, lv_color_black(), 0);
    lv_obj_set_style_text_font(forecast_display_label, &my_font_1, 0);
    lv_obj_set_style_text_align(forecast_display_label, LV_TEXT_ALIGN_CENTER, 0);  // 居中对齐
    lv_label_set_text(forecast_display_label, "正在获取天气预报...");
    
    /* 创建室内温湿度显示标签（合并为一行） */
    indoor_temp_label = lv_label_create(screen4);
    lv_obj_set_width(indoor_temp_label, 220);  // 设置足够宽度以容纳合并后的文本
    lv_obj_set_height(indoor_temp_label, LV_SIZE_CONTENT);
    lv_obj_align(indoor_temp_label, LV_ALIGN_BOTTOM_MID, 0, -30);  // 底部居中位置
    lv_obj_set_style_text_color(indoor_temp_label, lv_color_make(0, 100, 100), 0);  // 青绿色
    lv_obj_set_style_text_font(indoor_temp_label, &my_font_1, 0);
    lv_label_set_text(indoor_temp_label, "温度/湿度：加载中...");
    
    /* 创建室内湿度显示标签（将被隐藏，但保留以避免代码其他部分报错） */
    indoor_humid_label = lv_label_create(screen4);
    lv_obj_set_width(indoor_humid_label, LV_SIZE_CONTENT);
    lv_obj_set_height(indoor_humid_label, LV_SIZE_CONTENT);
    lv_obj_add_flag(indoor_humid_label, LV_OBJ_FLAG_HIDDEN);  // 默认隐藏
    lv_obj_set_style_text_font(indoor_humid_label, &my_font_1, 0);
    
    /* 创建桌面4的点状指示器 */
    create_desktop_dots(screen4, 3);
}

/* 创建UI界面 */
static void create_ui(void)
{
    ESP_LOGI(TAG, "创建多桌面UI界面...");
    
    /* 创建桌面1 */
    create_desktop1();
    
    /* 创建桌面2 */
    create_desktop2();
    
    /* 创建桌面3 */
    create_desktop3();
    
    /* 创建桌面4 */
    create_desktop4();
    
    /* 设置桌面1为活动屏幕 */
    lv_scr_load(desktop_screens[0]);
    current_desktop = 0;
    
    ESP_LOGI(TAG, "多桌面UI界面创建成功");
}

/* 初始化时间（如果需要设置初始时间） */
static void init_time_if_needed(void)
{
    ds3231_time_t time;
    
    /* 尝试读取时间，只有在读取失败或时间明显不合理时才设置初始时间 */
    if (ds3231_get_time(&time) != ESP_OK) {
        ESP_LOGW(TAG, "DS3231读取失败，可能电池没电，设置初始时间...");
        
        /* 设置初始时间为 2024年12月25日 12:00:00 周三 */
        time.year = 2024;
        time.month = 12;
        time.date = 25;
        time.day_of_week = 3; /* 周三 */
        time.hour = 12;
        time.minute = 0;
        time.second = 0;
        
        if (ds3231_set_time(&time) == ESP_OK) {
            ESP_LOGI(TAG, "Initial time set successfully");
        } else {
            ESP_LOGE(TAG, "Failed to set initial time");
        }
    } else {
        /* DS3231读取成功，检查时间是否在合理范围内 */
        if (time.year < 2020 || time.year > 2100 || 
            time.month < 1 || time.month > 12 ||
            time.date < 1 || time.date > 31 ||
            time.hour > 23 || time.minute > 59 || time.second > 59) {
            
            ESP_LOGW(TAG, "DS3231时间数据异常: %04d-%02d-%02d %02d:%02d:%02d，重置时间...", 
                    time.year, time.month, time.date, time.hour, time.minute, time.second);
            
            /* 设置初始时间 */
            time.year = 2024;
            time.month = 12;
            time.date = 25;
            time.day_of_week = 3;
            time.hour = 12;
            time.minute = 0;
            time.second = 0;
            
            if (ds3231_set_time(&time) == ESP_OK) {
                ESP_LOGI(TAG, "时间重置成功");
            } else {
                ESP_LOGE(TAG, "时间重置失败");
            }
        } else {
            ESP_LOGI(TAG, "DS3231时间正常: %04d-%02d-%02d %02d:%02d:%02d", 
                    time.year, time.month, time.date, time.hour, time.minute, time.second);
        }
    }
}

/* WiFi连接任务 */
static void wifi_connect_task(void *arg)
{
    ESP_LOGI(TAG, "开始连接WiFi...");
    esp_err_t ret = wifi_connect();
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "WiFi连接成功！");
    } else {
        ESP_LOGE(TAG, "WiFi连接失败！");
    }
    vTaskDelete(NULL);  // 删除当前任务
}

/* 天气信息更新任务 */
static void weather_update_task(void *arg)
{
    weather_info_t weather_info;
    char weather_str[256];
    
    /* 初始化天气API */
    esp_err_t ret = weather_api_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize weather API: %s", esp_err_to_name(ret));
        vTaskDelete(NULL);
        return;
    }
    
    ESP_LOGI(TAG, "Weather API initialized successfully");
    
    while (1) {
        /* 检查WiFi连接状态 */
        wifi_status_t wifi_status = wifi_get_status();
        if (wifi_status == WIFI_STATUS_CONNECTED) {
            
            /* 首次连接或定时同步网络时间（仅在启用网络时间时执行） */
            if (use_network_time && (!time_synced || 
                (xTaskGetTickCount() - last_time_sync) >= pdMS_TO_TICKS(TIME_SYNC_INTERVAL_MS))) {
                ESP_LOGI(TAG, "执行网络时间同步...");
                esp_err_t sync_result = sync_time_from_network();
                if (sync_result == ESP_OK) {
                    ESP_LOGI(TAG, "网络时间同步成功");
                } else {
                    ESP_LOGE(TAG, "网络时间同步失败");
                }
            } else if (!use_network_time) {
                ESP_LOGI(TAG, "网络时间同步已禁用，跳过同步");
            }
            
            /* 检查是否到了天气更新时间 */
            if ((xTaskGetTickCount() - last_weather_update) >= pdMS_TO_TICKS(WEATHER_UPDATE_INTERVAL_MS)) {
                ESP_LOGI(TAG, "Updating weather information...");
                
                /* 获取即墨天气信息 (城市编码: 370215) */
                ret = weather_api_get_weather("370215", &weather_info);
                if (ret == ESP_OK) {
                    /* 格式化天气信息显示 - 只使用字库中有的字 */
                    snprintf(weather_str, sizeof(weather_str), 
                            "即墨 %s %s°C", 
                            weather_info.weather, 
                            weather_info.temperature);
                    
                    /* 更新天气缓存 */
                    update_weather_cache(weather_str);
                    
                    /* 更新天气显示 */
                    if (weather_label) {
                        lv_label_set_text(weather_label, weather_str);
                        /* 使用新字体 */
                        lv_obj_set_style_text_font(weather_label, &my_font_1, 0);
                    }
                    
                    ESP_LOGI(TAG, "Weather updated: %s", weather_str);
                    last_weather_update = xTaskGetTickCount();
                } else {
                    ESP_LOGE(TAG, "Failed to get weather info: %s", esp_err_to_name(ret));
                    
                    /* 显示具体的错误信息 */
                    if (weather_label) {
                        if (ret == ESP_ERR_WIFI_NOT_CONNECT) {
                            lv_label_set_text(weather_label, "等待连接");
                        } else {
                            lv_label_set_text(weather_label, "连接失败");
                        }
                        lv_obj_set_style_text_font(weather_label, &my_font_1, 0);
                    }
                }
            }
            
            /* 检查是否到了农历更新时间（首次连接立即更新，之后按间隔更新） */
            if (!lunar_first_update || 
                (xTaskGetTickCount() - last_lunar_update) >= pdMS_TO_TICKS(LUNAR_UPDATE_INTERVAL_MS)) {
                ESP_LOGI(TAG, "Updating lunar date information...");
                
                /* 获取农历日期信息 */
                esp_err_t lunar_ret = get_lunar_date();
                if (lunar_ret == ESP_OK) {
                    ESP_LOGI(TAG, "Lunar date updated successfully");
                    lunar_first_update = true;  // 标记已完成首次更新
                } else {
                    ESP_LOGE(TAG, "Failed to update lunar date: %s", esp_err_to_name(lunar_ret));
                }
            }
        } else {
            ESP_LOGW(TAG, "WiFi not connected, skipping weather and time sync");
            
            /* 显示WiFi未连接状态或缓存的天气信息 */
            if (weather_label) {
                if (is_weather_cache_valid()) {
                    /* 使用缓存的天气信息 */
                    lv_label_set_text(weather_label, get_cached_weather());
                    ESP_LOGI(TAG, "显示缓存天气信息: %s", get_cached_weather());
                } else {
                    /* 无缓存或缓存过期，显示等待连接 */
                    lv_label_set_text(weather_label, "等待连接");
                }
                lv_obj_set_style_text_font(weather_label, &my_font_1, 0);
            }
            /* 检查农历缓存，如果有有效缓存就使用，否则显示等待连接 */
            ds3231_time_t current_time;
            if (ds3231_get_time(&current_time) == ESP_OK) {
                char cached_lunar[64];
                if (get_lunar_from_cache(current_time.year, current_time.month, current_time.date, 
                                       cached_lunar, sizeof(cached_lunar))) {
                    /* 使用缓存的农历信息 */
                    if (lunar_date_label) {
                        lv_label_set_text(lunar_date_label, cached_lunar);
                        lv_obj_set_style_text_font(lunar_date_label, &my_font_1, 0);
                    }
                    ESP_LOGI(TAG, "显示缓存农历信息: %s", cached_lunar);
                } else {
                    /* 无缓存或缓存过期 */
                    if (lunar_date_label) {
                        lv_label_set_text(lunar_date_label, "农历等待连接");
                        lv_obj_set_style_text_font(lunar_date_label, &my_font_1, 0);
                    }
                }
            } else {
                if (lunar_date_label) {
                    lv_label_set_text(lunar_date_label, "农历等待连接");
                    lv_obj_set_style_text_font(lunar_date_label, &my_font_1, 0);
                }
            }
            
            /* 重置时间同步标志，WiFi重连后重新同步 */
            if (time_synced) {
                time_synced = false;
                ESP_LOGI(TAG, "WiFi断连，重置时间同步标志");
            }
            
            /* 即使WiFi断开，也要定期检查农历缓存 */
            if ((xTaskGetTickCount() - last_lunar_update) >= pdMS_TO_TICKS(60000)) { // 每分钟检查一次
                ESP_LOGI(TAG, "WiFi断开状态下检查农历缓存...");
                esp_err_t lunar_ret = get_lunar_date();
                if (lunar_ret == ESP_OK) {
                    ESP_LOGI(TAG, "成功从缓存获取农历信息");
                } else {
                    ESP_LOGI(TAG, "缓存中无有效农历信息");
                }
                last_lunar_update = xTaskGetTickCount();
            }
        }
        
        vTaskDelay(pdMS_TO_TICKS(5000));  // 每5秒检查一次
    }
}

static esp_err_t get_weather_forecast(void)
{
    /* 检查WiFi连接状态 */
    wifi_status_t wifi_status = wifi_get_status();
    if (wifi_status != WIFI_STATUS_CONNECTED) {
        ESP_LOGW(TAG, "WiFi未连接，跳过天气预报获取");
        return ESP_FAIL;
    }

    char url[512];
    snprintf(url, sizeof(url), 
        "http://restapi.amap.com/v3/weather/weatherInfo?key=%s&city=370215&extensions=all&output=json", WEATHER_API_KEY);

    ESP_LOGI(TAG, "天气预报请求URL: %s", url);

    esp_http_client_config_t config = {
        .url = url,
        .event_handler = http_event_handler,
        .buffer_size = 8192,
        .timeout_ms = 10000,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG, "Failed to initialize HTTP client for forecast");
        return ESP_FAIL;
    }

    http_response_t response = {0};
    esp_http_client_set_user_data(client, &response);

    ESP_LOGI(TAG, "开始执行天气预报HTTP请求...");
    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        int status_code = esp_http_client_get_status_code(client);
        ESP_LOGI(TAG, "天气预报HTTP GET Status = %d, content_length = %lld",
                status_code, esp_http_client_get_content_length(client));
        
        if (status_code == 200 && response.data) {
            ESP_LOGI(TAG, "天气预报响应数据长度: %d", response.len);
            ESP_LOGI(TAG, "天气预报响应: %s", response.data);
            
            cJSON *json = cJSON_Parse(response.data);
            if (json) {
                cJSON *status = cJSON_GetObjectItem(json, "status");
                if (status && cJSON_IsString(status) && strcmp(status->valuestring, "1") == 0) {
                    cJSON *forecasts = cJSON_GetObjectItem(json, "forecasts");
                    if (forecasts && cJSON_IsArray(forecasts)) {
                        cJSON *forecast = cJSON_GetArrayItem(forecasts, 0);
                        if (forecast) {
                            // 获取城市和更新时间
                            cJSON *city = cJSON_GetObjectItem(forecast, "city");
                            cJSON *reporttime = cJSON_GetObjectItem(forecast, "reporttime");
                            
                            if (city && cJSON_IsString(city)) {
                                strncpy(forecast_city, city->valuestring, sizeof(forecast_city) - 1);
                            }
                            if (reporttime && cJSON_IsString(reporttime)) {
                                strncpy(forecast_update_time, reporttime->valuestring, sizeof(forecast_update_time) - 1);
                            }
                            
                            // 获取预报数据
                            cJSON *casts = cJSON_GetObjectItem(forecast, "casts");
                            if (casts && cJSON_IsArray(casts)) {
                                int count = cJSON_GetArraySize(casts);
                                if (count > 4) count = 4; // 最多4天数据
                                
                                for (int i = 0; i < count; i++) {
                                    cJSON *cast = cJSON_GetArrayItem(casts, i);
                                    if (cast) {
                                        cJSON *date = cJSON_GetObjectItem(cast, "date");
                                        cJSON *week = cJSON_GetObjectItem(cast, "week");
                                        cJSON *dayweather = cJSON_GetObjectItem(cast, "dayweather");
                                        cJSON *nightweather = cJSON_GetObjectItem(cast, "nightweather");
                                        cJSON *daytemp = cJSON_GetObjectItem(cast, "daytemp");
                                        cJSON *nighttemp = cJSON_GetObjectItem(cast, "nighttemp");
                                        cJSON *daywind = cJSON_GetObjectItem(cast, "daywind");
                                        cJSON *nightwind = cJSON_GetObjectItem(cast, "nightwind");
                                        cJSON *daypower = cJSON_GetObjectItem(cast, "daypower");
                                        cJSON *nightpower = cJSON_GetObjectItem(cast, "nightpower");
                                        
                                        // 清空数据
                                        memset(&forecast_data[i], 0, sizeof(weather_forecast_t));
                                        
                                        // 填充数据
                                        if (date && cJSON_IsString(date)) {
                                            strncpy(forecast_data[i].date, date->valuestring, sizeof(forecast_data[i].date) - 1);
                                        }
                                        if (week && cJSON_IsString(week)) {
                                            strncpy(forecast_data[i].week, week->valuestring, sizeof(forecast_data[i].week) - 1);
                                        }
                                        if (dayweather && cJSON_IsString(dayweather)) {
                                            strncpy(forecast_data[i].dayweather, dayweather->valuestring, sizeof(forecast_data[i].dayweather) - 1);
                                        }
                                        if (nightweather && cJSON_IsString(nightweather)) {
                                            strncpy(forecast_data[i].nightweather, nightweather->valuestring, sizeof(forecast_data[i].nightweather) - 1);
                                        }
                                        if (daytemp && cJSON_IsString(daytemp)) {
                                            strncpy(forecast_data[i].daytemp, daytemp->valuestring, sizeof(forecast_data[i].daytemp) - 1);
                                        }
                                        if (nighttemp && cJSON_IsString(nighttemp)) {
                                            strncpy(forecast_data[i].nighttemp, nighttemp->valuestring, sizeof(forecast_data[i].nighttemp) - 1);
                                        }
                                        if (daywind && cJSON_IsString(daywind)) {
                                            strncpy(forecast_data[i].daywind, daywind->valuestring, sizeof(forecast_data[i].daywind) - 1);
                                        }
                                        if (nightwind && cJSON_IsString(nightwind)) {
                                            strncpy(forecast_data[i].nightwind, nightwind->valuestring, sizeof(forecast_data[i].nightwind) - 1);
                                        }
                                        if (daypower && cJSON_IsString(daypower)) {
                                            strncpy(forecast_data[i].daypower, daypower->valuestring, sizeof(forecast_data[i].daypower) - 1);
                                        }
                                        if (nightpower && cJSON_IsString(nightpower)) {
                                            strncpy(forecast_data[i].nightpower, nightpower->valuestring, sizeof(forecast_data[i].nightpower) - 1);
                                        }
                                    }
                                }
                                
                                forecast_updated = true;
                                ESP_LOGI(TAG, "天气预报更新成功");
                            }
                        }
                    }
                }
                cJSON_Delete(json);
            } else {
                ESP_LOGE(TAG, "天气预报JSON解析失败");
            }
        } else {
            ESP_LOGE(TAG, "天气预报HTTP响应错误: status_code=%d, data=%s", status_code, response.data ? "有数据" : "无数据");
        }
    } else {
        ESP_LOGE(TAG, "天气预报HTTP GET request failed: %s", esp_err_to_name(err));
    }

    if (response.data) {
        free(response.data);
    }
    esp_http_client_cleanup(client);
    return err;
}

static void weather_forecast_update_task(void *arg)
{
    ESP_LOGI(TAG, "天气预报更新任务启动");
    
    /* 等待30秒让系统完全初始化 */
    vTaskDelay(pdMS_TO_TICKS(30000));
    ESP_LOGI(TAG, "天气预报任务等待结束，开始工作");
    
    while (1) {
        /* 检查WiFi连接状态 */
        wifi_status_t wifi_status = wifi_get_status();
        if (wifi_status == WIFI_STATUS_CONNECTED) {
            ESP_LOGI(TAG, "开始获取天气预报...");
            esp_err_t ret = get_weather_forecast();
            if (ret == ESP_OK) {
                ESP_LOGI(TAG, "天气预报获取成功");
                /* 如果当前在桌面4，更新显示 */
                if (current_desktop == 3) {
                    update_forecast_display();
                }
                    } else {
            ESP_LOGE(TAG, "天气预报获取失败: %s", esp_err_to_name(ret));
        }
        } else {
            ESP_LOGW(TAG, "WiFi未连接，跳过天气预报获取");
        }
        
        /* 每30分钟获取一次天气预报 */
        vTaskDelay(pdMS_TO_TICKS(30 * 60 * 1000));
    }
}

/* AI助手显示更新任务 */
static void speech_display_update_task(void *arg)
{
    char display_buffer[512];
    TickType_t last_update = xTaskGetTickCount();
    
    ESP_LOGI(TAG, "AI assistant display update task started");
    
    while (speech_rec_active && setting_state == SETTING_STATE_SPEECH_REC) {
        // 获取AI助手结果
        speech_recognition_result_t* result = speech_recognition_get_result();
        
        // 格式化显示信息（聊天框形式）
        switch (result->state) {
            case SPEECH_STATE_IDLE:
                snprintf(display_buffer, sizeof(display_buffer),
                    "◆ AI智能助手 ◆\n\n"
                    "系统已就绪，按下按键\n"
                    "开始语音对话"
                );
                break;
                
            case SPEECH_STATE_RECORDING:
                snprintf(display_buffer, sizeof(display_buffer),
                    "◆ 录音中 ◆\n\n"
                    "正在聆听您的声音...\n"
                );
                break;
                
            case SPEECH_STATE_PROCESSING:
                snprintf(display_buffer, sizeof(display_buffer),
                    "◆ 处理中 ◆\n\n"
                    "AI正在思考，请稍候...\n"
                );
                break;
                
            case SPEECH_STATE_COMPLETED:
                if (result->valid && strlen(result->result_text) > 0) {
                    // 智能聊天框显示
                    char user_display[100];
                    
                    // 截断用户输入，保持简洁
                    if (strlen(result->result_text) > 80) {
                        strncpy(user_display, result->result_text, 77);
                        user_display[77] = '\0';
                        strcat(user_display, "...");
                    } else {
                        strcpy(user_display, result->result_text);
                    }
                    
                    // 使用分离的标签显示用户和AI消息，实现不同颜色
                    if (setting_page_active && user_message_label && ai_message_label) {
                        // 显示用户消息（蓝色）
                        char user_buffer[150];
                        snprintf(user_buffer, sizeof(user_buffer), "用户:\n%s", user_display);
                        lv_label_set_text(user_message_label, user_buffer);
                        lv_obj_clear_flag(user_message_label, LV_OBJ_FLAG_HIDDEN);
                        
                        // 显示AI消息（绿色）
                        char ai_buffer[350];
                        if (result->has_ai_reply && strlen(result->ai_reply) > 0) {
                            char truncated_ai_text[280];
                            
                            // 复制AI回复内容，确保标点符号正确显示
                            size_t copy_len = sizeof(truncated_ai_text) - 1 < strlen(result->ai_reply) ? 
                                             sizeof(truncated_ai_text) - 1 : strlen(result->ai_reply);
                            memcpy(truncated_ai_text, result->ai_reply, copy_len);
                            truncated_ai_text[copy_len] = '\0';
                            
                            // 使用memcpy和固定长度而不是strncpy，避免标点符号问题
                            snprintf(ai_buffer, sizeof(ai_buffer), "AI助手:\n%s", truncated_ai_text);
                        } else {
                            snprintf(ai_buffer, sizeof(ai_buffer), "AI助手:\n正在生成回复...");
                        }
                        lv_label_set_text(ai_message_label, ai_buffer);
                        lv_obj_clear_flag(ai_message_label, LV_OBJ_FLAG_HIDDEN);
                        
                        // 隐藏原来的单一显示标签
                        lv_obj_add_flag(setting_display_label, LV_OBJ_FLAG_HIDDEN);
                    } else {
                        // 备用显示方式
                        if (result->has_ai_reply && strlen(result->ai_reply) > 0) {
                            char truncated_ai_text[280];
                            
                            // 复制AI回复内容，确保标点符号正确显示
                            size_t copy_len = sizeof(truncated_ai_text) - 1 < strlen(result->ai_reply) ? 
                                             sizeof(truncated_ai_text) - 1 : strlen(result->ai_reply);
                            memcpy(truncated_ai_text, result->ai_reply, copy_len);
                            truncated_ai_text[copy_len] = '\0';
                            
                            snprintf(display_buffer, sizeof(display_buffer),
                                "用户: %s\n\n"
                                "AI: %s",
                                user_display,
                                truncated_ai_text
                            );
                        } else {
                            snprintf(display_buffer, sizeof(display_buffer),
                                "用户: %s\n\n"
                                "AI: 正在生成回复...",
                                user_display
                            );
                        }
                    }
                } else {
                    snprintf(display_buffer, sizeof(display_buffer),
                        "◆ 录音提示 ◆\n\n"
                        "没有检测到有效语音\n"
                        "请靠近麦克风并大声一些\n\n"
                        "按键重新尝试录音"
                    );
                }
                break;
                
            case SPEECH_STATE_ERROR:
                // 简化错误信息显示
                char error_display[100];
                if (strlen(result->error_message) > 60) {
                    strncpy(error_display, result->error_message, 57);
                    error_display[57] = '\0';
                    strcat(error_display, "...");
                } else {
                    strcpy(error_display, result->error_message);
                }
                
                snprintf(display_buffer, sizeof(display_buffer),
                    "◆ 系统错误 ◆\n\n"
                    "操作遇到问题：\n"
                    "%s\n\n"
                    "请重新尝试或检查网络连接",
                    error_display
                );
                break;
        }
        
        // 更新显示（仅在设置页面激活时）
        if (setting_page_active && setting_display_label != NULL) {
            // 对于非对话状态，隐藏分离的标签，显示单一标签
            if (result->state != SPEECH_STATE_COMPLETED || 
                !result->valid || strlen(result->result_text) == 0) {
                
                // 隐藏分离的用户/AI标签
                if (user_message_label) lv_obj_add_flag(user_message_label, LV_OBJ_FLAG_HIDDEN);
                if (ai_message_label) lv_obj_add_flag(ai_message_label, LV_OBJ_FLAG_HIDDEN);
                
                // 显示单一标签
                lv_obj_clear_flag(setting_display_label, LV_OBJ_FLAG_HIDDEN);
                lv_label_set_text(setting_display_label, display_buffer);
                
                // 根据状态设置不同的颜色和背景效果
                switch (result->state) {
                    case SPEECH_STATE_IDLE:
                        lv_obj_set_style_text_color(setting_display_label, lv_color_hex(0x007BFF), 0); // 蓝色
                        lv_obj_set_style_bg_color(setting_display_label, lv_color_hex(0xF0F8FF), 0); // 浅蓝背景
                        break;
                    case SPEECH_STATE_RECORDING:
                        lv_obj_set_style_text_color(setting_display_label, lv_color_hex(0xFF6600), 0); // 橙色
                        lv_obj_set_style_bg_color(setting_display_label, lv_color_hex(0xFFF8F0), 0); // 浅橙背景
                        break;
                    case SPEECH_STATE_PROCESSING:
                        lv_obj_set_style_text_color(setting_display_label, lv_color_hex(0x6F42C1), 0); // 紫色
                        lv_obj_set_style_bg_color(setting_display_label, lv_color_hex(0xF8F0FF), 0); // 浅紫背景
                        break;
                    case SPEECH_STATE_ERROR:
                        lv_obj_set_style_text_color(setting_display_label, lv_color_hex(0xDC3545), 0); // 红色
                        lv_obj_set_style_bg_color(setting_display_label, lv_color_hex(0xFFF0F0), 0); // 浅红背景
                        break;
                    default:
                        lv_obj_set_style_text_color(setting_display_label, lv_color_hex(0x333333), 0); // 深灰色
                        lv_obj_set_style_bg_color(setting_display_label, lv_color_hex(0xF8FFF8), 0); // 浅绿背景
                        break;
                }
                
                // 为显示区域添加边框和圆角美化
                lv_obj_set_style_border_width(setting_display_label, 2, 0);
                lv_obj_set_style_border_color(setting_display_label, lv_obj_get_style_text_color(setting_display_label, 0), 0);
                lv_obj_set_style_radius(setting_display_label, 5, 0);
                lv_obj_set_style_pad_all(setting_display_label, 8, 0);
            }
            
            // 在AI助手模式下隐藏底部提示文字，保持界面简洁
            if (setting_state == SETTING_STATE_SPEECH_REC) {
                lv_label_set_text(setting_hint_label, "");  // 清空提示文字
            }
        }
        
        // 延迟更新，避免过于频繁
        vTaskDelayUntil(&last_update, pdMS_TO_TICKS(500));
    }
    
    ESP_LOGI(TAG, "AI assistant display update task ended");
    speech_display_task_handle = NULL;
    vTaskDelete(NULL);
}

/* MQ2烟雾传感器相关函数 */
static esp_err_t mq2_sensor_init(void)
{
    // 配置ADC
    adc1_config_width(MQ2_ADC_WIDTH);
    adc1_config_channel_atten(MQ2_ADC_CHANNEL, MQ2_ADC_ATTEN);
    
    // 表征ADC
    esp_adc_cal_characterize(ADC_UNIT_1, MQ2_ADC_ATTEN, MQ2_ADC_WIDTH, MQ2_DEFAULT_VREF, &mq2_adc_chars);
    
    ESP_LOGI(TAG, "MQ2烟雾传感器初始化完成");
    return ESP_OK;
}

// 前向声明
static void update_mq2_display(void);
static void play_mq2_alarm_sound(void);

static uint32_t mq2_read_voltage(void)
{
    // 多次采样取平均值，提高精度
    uint32_t adc_reading = 0;
    for (int i = 0; i < MQ2_SAMPLE_COUNT; i++) {
        adc_reading += adc1_get_raw(MQ2_ADC_CHANNEL);
    }
    adc_reading /= MQ2_SAMPLE_COUNT;
    
    // 将ADC读数转换为电压（mV）
    uint32_t voltage = esp_adc_cal_raw_to_voltage(adc_reading, &mq2_adc_chars);
    
    ESP_LOGD(TAG, "MQ2传感器读数: 原始值=%lu, 电压=%lumV", (unsigned long)adc_reading, (unsigned long)voltage);
    return voltage;
}

static void mq2_sensor_update_task(void *arg)
{
    ESP_LOGI(TAG, "MQ2烟雾传感器更新任务启动");
    
    TickType_t last_wake_time = xTaskGetTickCount();
    
    while (true) {
        // 读取MQ2传感器数据
        uint32_t voltage = mq2_read_voltage();
        
        // 保存当前读数
        mq2_value = voltage;
        
        // 判断是否超过阈值
        bool is_alarm = (voltage > MQ2_ALARM_THRESHOLD);
        
        // 检查报警状态
        if (is_alarm) {
            // 超出阈值，增加计数器
            mq2_alarm_counter++;
            
            // 如果是状态变化，打印日志
            if (!mq2_alarm_state) {
                ESP_LOGW(TAG, "MQ2烟雾传感器警报: 当前值=%lumV, 阈值=%lumV", (unsigned long)voltage, (unsigned long)MQ2_ALARM_THRESHOLD);
                mq2_alarm_state = true;
                // 触发震动提醒
                start_vibration();
            }
            
            // 连续超出阈值次数达到要求，触发声音报警
            if (mq2_alarm_counter >= MQ2_ALARM_COUNT && !mq2_audio_alarm_triggered) {
                ESP_LOGW(TAG, "MQ2烟雾传感器连续%d次超出阈值，触发声音报警", MQ2_ALARM_COUNT);
                play_mq2_alarm_sound();
            }
        } else {
            // 正常状态，重置计数器
            mq2_alarm_counter = 0;
            
            // 如果是状态变化，打印日志
            if (mq2_alarm_state) {
                ESP_LOGI(TAG, "MQ2烟雾传感器恢复正常: 当前值=%lumV", (unsigned long)voltage);
                mq2_alarm_state = false;
            }
        }
        
        // 更新显示
        update_mq2_display();
        
        // 延时
        vTaskDelayUntil(&last_wake_time, pdMS_TO_TICKS(MQ2_UPDATE_INTERVAL));
    }
    
    vTaskDelete(NULL);
}

/* MQ2传感器报警声音播放任务 */
static void mq2_audio_task(void *arg)
{
    ESP_LOGI(TAG, "MQ2烟雾传感器报警：播放紧急警报声");
    
    // 保存原来的音量设置
    uint8_t original_volume = system_volume;
    
    // 交替使用高低音量播放，增强警报效果
    uint8_t volumes[] = {100, 85, 95, 85, 100}; // 音量变化模式
    
    // 循环播放5次报警音，增加次数使警报更明显
    for (int i = 0; i < 5; i++) {
        // 使用不同音量播放报警提示音
        audio_player_set_volume(volumes[i]); 
        
        // 播放警报声
        audio_player_play_pcm((const uint8_t*)mq2_alarm_tone_data, mq2_alarm_tone_size);
        
        // 闪烁UI上的文字效果 - 通过任务通知给主任务
        if (current_desktop == 0) { // 如果在桌面1
            lv_obj_set_style_text_color(mq2_label, 
                (i % 2 == 0) ? lv_color_make(255, 0, 0) : lv_color_make(255, 255, 0), 0); 
        }
        
        // 播放警报声的同时触发震动
        start_vibration();
        
        // 每次播放之间有短暂间隔
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    
    // 恢复原来的系统音量和文字颜色
    audio_player_set_volume(original_volume);
    if (current_desktop == 0 && mq2_label != NULL) {
        lv_obj_set_style_text_color(mq2_label, lv_color_make(220, 0, 0), 0);
    }
    
    // 音频报警完成后重置标志
    mq2_audio_alarm_triggered = false;
    vTaskDelete(NULL);
}

/* 播放MQ2报警声音 */
static void play_mq2_alarm_sound(void)
{
    // 如果已经触发过报警声，则不重复触发
    if (mq2_audio_alarm_triggered) {
        return;
    }
    
    mq2_audio_alarm_triggered = true;
    
    // 创建播放报警音的任务
    xTaskCreate(mq2_audio_task, "mq2_audio_task", 4096, NULL, 5, NULL);
}

static void update_mq2_display(void)
{
    if (mq2_label == NULL) {
        return;
    }
    
    char buffer[64];
    
    if (mq2_alarm_state) {
        // 异常状态，显示红色警告
        snprintf(buffer, sizeof(buffer), "空气质量: 异常 (%lumV)", (unsigned long)mq2_value);
        lv_obj_set_style_text_color(mq2_label, lv_color_make(220, 0, 0), 0); // 红色
    } else {
        // 正常状态，显示绿色文字
        snprintf(buffer, sizeof(buffer), "空气质量: 正常 (%lumV)", (unsigned long)mq2_value);
        lv_obj_set_style_text_color(mq2_label, lv_color_make(0, 160, 0), 0); // 绿色
    }
    
    lv_label_set_text(mq2_label, buffer);
}

/* 定时器音频播放任务 */
static void timer_audio_task(void *arg)
{
    if (selected_ringtone == RINGTONE_WAV_FILE) {
        ESP_LOGI(TAG, "定时器结束：播放WAV文件铃声");
        audio_play_wav_file("ring.wav", system_volume);
    } else {
        ESP_LOGI(TAG, "定时器结束：播放内置双音调铃声");
        audio_player_play_pcm(alarm_tone_data, alarm_tone_size);
    }
    vTaskDelete(NULL);
}

/* 闹钟音频播放任务 */
static void alarm_audio_task(void *arg)
{
    if (selected_ringtone == RINGTONE_WAV_FILE) {
        ESP_LOGI(TAG, "闹钟响铃：播放WAV文件铃声");
        audio_play_wav_file("ring.wav", system_volume);
    } else {
        ESP_LOGI(TAG, "闹钟响铃：播放内置双音调铃声");
        audio_player_play_pcm(alarm_tone_data, alarm_tone_size);
    }
    vTaskDelete(NULL);
}

/* 内存监控任务 */
static void memory_monitor_task(void *arg)
{
    uint32_t min_free_heap = esp_get_minimum_free_heap_size();
    uint32_t last_free_heap = esp_get_free_heap_size();
    uint32_t warning_threshold = 50 * 1024;  // 50KB警告阈值
    uint32_t critical_threshold = 20 * 1024; // 20KB临界阈值
    
    ESP_LOGI(TAG, "内存监控任务启动，警告阈值: %ld KB", warning_threshold / 1024);
    
    while (1) {
        uint32_t current_free = esp_get_free_heap_size();
        uint32_t current_min = esp_get_minimum_free_heap_size();
        
        /* 检查内存变化 */
        if (current_min < min_free_heap) {
            min_free_heap = current_min;
            ESP_LOGW(TAG, "最小可用堆内存更新: %ld 字节", min_free_heap);
        }
        
        /* 内存警告检查 */
        if (current_free < critical_threshold) {
            ESP_LOGE(TAG, "内存严重不足! 当前: %ld 字节, 最小历史: %ld 字节", 
                     current_free, current_min);
            /* 可以在这里添加紧急内存清理操作 */
        } else if (current_free < warning_threshold) {
            ESP_LOGW(TAG, "内存偏低警告! 当前: %ld 字节, 最小历史: %ld 字节", 
                     current_free, current_min);
        }
        
        /* 内存大幅下降检测 */
        if (last_free_heap > current_free && 
            (last_free_heap - current_free) > 10 * 1024) {  // 超过10KB变化
            ESP_LOGI(TAG, "内存变化: %ld -> %ld 字节 (减少 %ld 字节)", 
                     last_free_heap, current_free, last_free_heap - current_free);
        }
        
        last_free_heap = current_free;
        
        /* 每30秒检查一次 */
        vTaskDelay(pdMS_TO_TICKS(30000));
    }
}

/* 启动AI助手 */
static void start_speech_recognition(void)
{
    if (speech_rec_active) {
        ESP_LOGW(TAG, "Speech recognition already active");
        return;
    }
    
    ESP_LOGI(TAG, "Starting speech recognition...");
    
    // 初始化AI助手
    esp_err_t ret = speech_recognition_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize speech recognition: %s", esp_err_to_name(ret));
        
        // 显示错误信息
        if (setting_display_label != NULL) {
            lv_label_set_text(setting_display_label, 
                "Speech Recognition\n\n"
                "ERROR:\n"
                "Failed to initialize\n"
                "speech recognition\n\n"
                "Please check:\n"
                "- Memory availability\n"
                "- System resources\n\n"
                "Double press to exit"
            );
            lv_label_set_text(setting_hint_label, "Initialization failed!");
        }
        return;
    }
    
    speech_rec_active = true;
    
    // 创建显示更新任务
    BaseType_t task_created = xTaskCreate(
        speech_display_update_task,
        "speech_display",
        8192,  // 增加栈大小防止溢出
        NULL,
        4,     // 降低优先级避免冲突
        &speech_display_task_handle
    );
    
    if (task_created != pdPASS) {
        ESP_LOGE(TAG, "Failed to create speech recognition display task");
        stop_speech_recognition();
        return;
    }
    
    ESP_LOGI(TAG, "Speech recognition started successfully");
}

/* 停止AI助手 */
static void stop_speech_recognition(void)
{
    if (!speech_rec_active) {
        return;
    }
    
    ESP_LOGI(TAG, "Stopping speech recognition...");
    
    speech_rec_active = false;
    
    // 等待显示更新任务结束
    if (speech_display_task_handle != NULL) {
        // 任务会自动删除自己
        speech_display_task_handle = NULL;
    }
    
    // 停止AI助手
    speech_recognition_stop();
    speech_recognition_deinit();
    
    ESP_LOGI(TAG, "Speech recognition stopped");
}



/* 震动模块相关配置 */
#define VIBRATION_PWM_CHANNEL    LEDC_CHANNEL_0
#define VIBRATION_PWM_TIMER      LEDC_TIMER_0
#define VIBRATION_PWM_MODE       LEDC_LOW_SPEED_MODE
#define VIBRATION_PWM_FREQUENCY  5000
#define VIBRATION_PWM_RESOLUTION LEDC_TIMER_8_BIT
#define VIBRATION_PWM_PIN        38
#define VIBRATION_DUTY_CYCLE     200  // 占空比 (0-255)
#define VIBRATION_DURATION_MS    3000 // 震动持续时间3秒

/* 震动相关变量 */
static bool vibration_initialized = false;
static TaskHandle_t vibration_task_handle = NULL;

/* 震动模块功能函数 */
static esp_err_t vibration_init(void)
{
    if (vibration_initialized) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "初始化震动模块 PWM 配置");

    // 配置 PWM 定时器
    ledc_timer_config_t ledc_timer = {
        .duty_resolution = VIBRATION_PWM_RESOLUTION,
        .freq_hz = VIBRATION_PWM_FREQUENCY,
        .speed_mode = VIBRATION_PWM_MODE,
        .timer_num = VIBRATION_PWM_TIMER,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    esp_err_t ret = ledc_timer_config(&ledc_timer);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "PWM 定时器配置失败: %s", esp_err_to_name(ret));
        return ret;
    }

    // 配置 PWM 通道
    ledc_channel_config_t ledc_channel = {
        .channel = VIBRATION_PWM_CHANNEL,
        .duty = 0,
        .gpio_num = VIBRATION_PWM_PIN,
        .speed_mode = VIBRATION_PWM_MODE,
        .hpoint = 0,
        .timer_sel = VIBRATION_PWM_TIMER
    };
    ret = ledc_channel_config(&ledc_channel);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "PWM 通道配置失败: %s", esp_err_to_name(ret));
        return ret;
    }

    vibration_initialized = true;
    ESP_LOGI(TAG, "震动模块初始化完成，引脚: %d", VIBRATION_PWM_PIN);
    return ESP_OK;
}

static void vibration_set_duty(uint32_t duty)
{
    if (!vibration_initialized) {
        ESP_LOGW(TAG, "震动模块未初始化");
        return;
    }

    ledc_set_duty(VIBRATION_PWM_MODE, VIBRATION_PWM_CHANNEL, duty);
    ledc_update_duty(VIBRATION_PWM_MODE, VIBRATION_PWM_CHANNEL);
}

static void vibration_task(void *arg)
{
    ESP_LOGI(TAG, "震动开始，持续时间: %d ms", VIBRATION_DURATION_MS);
    
    // 启动震动
    vibration_set_duty(VIBRATION_DUTY_CYCLE);
    
    // 等待震动时间
    vTaskDelay(pdMS_TO_TICKS(VIBRATION_DURATION_MS));
    
    // 停止震动
    vibration_set_duty(0);
    
    ESP_LOGI(TAG, "震动结束");
    
    // 清理任务句柄
    vibration_task_handle = NULL;
    vTaskDelete(NULL);
}

/* Web服务器启动任务 */
static void web_server_start_task(void *arg)
{
    // 等待WiFi连接成功
    ESP_LOGI(TAG, "等待WiFi连接以启动Web服务器...");
    int retry_count = 0;
    const int max_retries = 10;
    
    while (wifi_get_status() != WIFI_STATUS_CONNECTED && retry_count < max_retries) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        retry_count++;
        ESP_LOGI(TAG, "等待WiFi连接 %d/%d", retry_count, max_retries);
    }
    
    if (wifi_get_status() != WIFI_STATUS_CONNECTED) {
        ESP_LOGE(TAG, "WiFi未连接，无法启动Web服务器");
        vTaskDelete(NULL);
        return;
    }
    
    // 获取IP地址
    char ip_str[16] = {0};
    if (wifi_get_ip_string(ip_str, sizeof(ip_str)) == ESP_OK) {
        ESP_LOGI(TAG, "Web服务器IP地址: %s", ip_str);
    }
    
    // 启动Web服务器
    esp_err_t ret = web_server_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "启动Web服务器失败: %s", esp_err_to_name(ret));
        vTaskDelete(NULL);
        return;
    }
    
    ESP_LOGI(TAG, "Web服务器已启动，可通过 http://%s/ 访问", ip_str);
    
    // 同步初始状态到Web服务器
    web_server_update_clock_status(use_24hour_format);
    web_server_update_alarm_status(alarm_hours, alarm_minutes, alarm_enabled);
    web_server_update_timer_status(timer_hours, timer_minutes, timer_seconds, timer_running);
    web_server_push_system_status();
    
    vTaskDelete(NULL);
}

/* DHT11传感器初始化函数 */
static esp_err_t dht11_init(void)
{
    ESP_LOGI(TAG, "初始化DHT11传感器...");
    
    // 首先将GPIO配置为输出模式
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << DHT11_PIN),
        .mode = GPIO_MODE_OUTPUT,  // 普通输出模式
        .pull_up_en = GPIO_PULLUP_DISABLE,  // 禁用内部上拉电阻，因为已有外部上拉
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    esp_err_t ret = gpio_config(&io_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "DHT11 GPIO配置失败");
        return ret;
    }
    
    // 设置初始状态为高电平
    gpio_set_level(DHT11_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(2000));  // 等待传感器稳定，DHT11上电后需要1-2秒稳定
    
    ESP_LOGI(TAG, "DHT11初始化成功");
    return ESP_OK;
}

/* DHT11读取温湿度数据函数 */
static esp_err_t dht11_read_data(float *temperature, float *humidity)
{
    uint8_t data[5] = {0};
    uint8_t attempts = 0;
    const uint8_t MAX_ATTEMPTS = 5;  // 增加尝试次数
    
    while (attempts < MAX_ATTEMPTS) {
        attempts++;
        ESP_LOGI(TAG, "尝试读取DHT11数据，第%d次...", attempts);
        
        // 使用无中断方式进行精确时序控制
        portDISABLE_INTERRUPTS();
        
        // 1. 发送起始信号
        gpio_set_direction(DHT11_PIN, GPIO_MODE_OUTPUT);
        
        // 确保起始状态稳定
        gpio_set_level(DHT11_PIN, 1);
        esp_rom_delay_us(50);
        
        // 发送起始信号：拉低至少18ms
        gpio_set_level(DHT11_PIN, 0);
        esp_rom_delay_us(22000);  // 延长到22ms确保DHT11能检测到
        
        // 释放总线（输出高电平）
        gpio_set_level(DHT11_PIN, 1);
        esp_rom_delay_us(30);  // 等待20-40us
        
        // 2. 切换为输入模式，准备接收数据
        gpio_set_direction(DHT11_PIN, GPIO_MODE_INPUT);
        
        // 由于有外部上拉电阻，释放总线后应该是高电平
        // 给DHT11一些时间来控制总线
        esp_rom_delay_us(5);
        
        // 3. 等待DHT11响应
        // DHT11应该先拉低80us作为响应的第一部分
        int timeout = 500;  // 大幅增加超时时间
        while (gpio_get_level(DHT11_PIN) == 1) {
            if (--timeout <= 0) {
                ESP_LOGW(TAG, "DHT11无响应 - 等待低电平超时");
                break;
            }
            esp_rom_delay_us(1);
        }
        
        if (timeout <= 0) {
            portENABLE_INTERRUPTS();
            vTaskDelay(pdMS_TO_TICKS(1000)); // 等待一段时间再尝试
            continue;
        }
        
        // 等待DHT11拉高（响应信号的第二部分，约80us）
        timeout = 500;
        while (gpio_get_level(DHT11_PIN) == 0) {
            if (--timeout <= 0) {
                ESP_LOGW(TAG, "DHT11响应异常 - 等待高电平超时");
                break;
            }
            esp_rom_delay_us(1);
        }
        
        if (timeout <= 0) {
            portENABLE_INTERRUPTS();
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        
        // 等待DHT11再次拉低，准备发送第一个数据位
        timeout = 500;
        while (gpio_get_level(DHT11_PIN) == 1) {
            if (--timeout <= 0) {
                ESP_LOGW(TAG, "DHT11响应异常 - 等待数据开始信号超时");
                break;
            }
            esp_rom_delay_us(1);
        }
        
        if (timeout <= 0) {
            portENABLE_INTERRUPTS();
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        
        // 4. 开始接收40位数据（5字节：湿度整数、湿度小数、温度整数、温度小数、校验和）
        uint8_t i, j;
        bool success = true;
        
        for (i = 0; i < 5; i++) {
            data[i] = 0;
            for (j = 0; j < 8; j++) {
                // 等待高电平（数据位开始）
                timeout = 500;
                while (gpio_get_level(DHT11_PIN) == 0) {
                    if (--timeout <= 0) {
                        ESP_LOGW(TAG, "读取数据位时等待高电平超时 [%d,%d]", i, j);
                        success = false;
                        break;
                    }
                    esp_rom_delay_us(1);
                }
                
                if (!success) break;
                
                // 测量高电平持续时间以判断数据位
                int high_level_time = 0;
                timeout = 500;
                
                // 等待高电平结束
                while (gpio_get_level(DHT11_PIN) == 1) {
                    high_level_time++;
                    if (--timeout <= 0) {
                        ESP_LOGW(TAG, "等待高电平结束超时 [%d,%d]", i, j);
                        success = false;
                        break;
                    }
                    esp_rom_delay_us(1);
                }
                
                if (!success) break;
                
                // 判断数据位：0信号约26-28us，1信号约70us
                if (high_level_time > 35) {  // 调整判断阈值，增加容错性
                    data[i] |= (1 << (7-j));
                }
            }
            if (!success) break;
        }
        
        portENABLE_INTERRUPTS();  // 重新启用中断
        
        if (!success) {
            ESP_LOGW(TAG, "读取DHT11数据失败，将重试");
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        
        // 5. 验证校验和
        if (i == 5 && data[4] == ((data[0] + data[1] + data[2] + data[3]) & 0xFF)) {
            // DHT11通常只有整数部分有效，小数部分为0
            *humidity = (float)data[0];
            *temperature = (float)data[2];
            
            ESP_LOGI(TAG, "DHT11读取成功: 温度=%.1f°C, 湿度=%.1f%%, 原始数据: %02x %02x %02x %02x | %02x", 
                     *temperature, *humidity, data[0], data[1], data[2], data[3], data[4]);
            return ESP_OK;
        } else {
            if (i == 5) {
                ESP_LOGW(TAG, "DHT11校验和错误，数据: %02x %02x %02x %02x | %02x, 校验和计算: %02x", 
                         data[0], data[1], data[2], data[3], data[4], 
                         ((data[0] + data[1] + data[2] + data[3]) & 0xFF));
            } else {
                ESP_LOGW(TAG, "DHT11数据不完整，仅接收 %d 字节", i);
            }
            
            // 失败后等待一段时间再尝试
            vTaskDelay(pdMS_TO_TICKS(2000));
        }
    }
    
    ESP_LOGE(TAG, "DHT11读取失败，达到最大尝试次数");
    return ESP_FAIL;
}

/* 更新室内温湿度显示 */
static void update_indoor_temp_humid_display(void)
{
    char combined_str[64];
    
    // 如果DHT11已初始化且有有效数据，则显示温湿度
    if (dht11_initialized) {
        int temp = (int)indoor_temperature;  // 转换为整数显示
        int humid = (int)indoor_humidity;    // 转换为整数显示
        snprintf(combined_str, sizeof(combined_str), "温度：%dC   湿度：%d%%", temp, humid);
    } else {
        snprintf(combined_str, sizeof(combined_str), "温度/湿度：初始化中...");
    }
    
    // 更新UI显示
    if (indoor_temp_label != NULL) {
        lv_label_set_text(indoor_temp_label, combined_str);
    }
    
    // 隐藏湿度标签，因为我们已经将信息合并到温度标签中
    if (indoor_humid_label != NULL) {
        lv_obj_add_flag(indoor_humid_label, LV_OBJ_FLAG_HIDDEN);
    }
}

/* DHT11数据更新任务 */
static void dht11_update_task(void *arg)
{
    // 初始化DHT11传感器
    if (dht11_init() != ESP_OK) {
        ESP_LOGE(TAG, "DHT11初始化失败，温湿度功能不可用");
        vTaskDelete(NULL);
        return;
    }
    
    dht11_initialized = true;
    ESP_LOGI(TAG, "DHT11传感器就绪，开始定期更新温湿度数据");
    
    // 等待DHT11稳定
    vTaskDelay(pdMS_TO_TICKS(2000));
    
    while (1) {
        // 读取DHT11数据
        if (dht11_read_data(&indoor_temperature, &indoor_humidity) == ESP_OK) {
            // 更新显示
            update_indoor_temp_humid_display();
        } else {
            ESP_LOGW(TAG, "DHT11读取失败，等待下次尝试");
        }
        
        // 延时等待
        vTaskDelay(pdMS_TO_TICKS(DHT11_UPDATE_INTERVAL_MS));
    }
}

static void start_vibration(void)
{
    if (!vibration_initialized) {
        ESP_LOGW(TAG, "震动模块未初始化，无法启动震动");
        return;
    }

    // 如果已有震动任务在运行，不重复启动
    if (vibration_task_handle != NULL) {
        ESP_LOGI(TAG, "震动任务已在运行中");
        return;
    }

    // 创建震动任务
    BaseType_t ret = xTaskCreate(vibration_task, "vibration_task", 2048, NULL, 5, &vibration_task_handle);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "创建震动任务失败");
        vibration_task_handle = NULL;
    }
}

/* 时间格式更新函数 - 供Web服务器调用 */
void update_time_format(bool use_24h_format)
{
    // 更新全局时间格式变量
    use_24hour_format = use_24h_format;
    
    // 记录日志
    ESP_LOGI(TAG, "时间格式已更新为: %s", use_24hour_format ? "24小时制" : "12小时制");
    
    // 立即更新显示
    if (time_label) {
        ds3231_time_t current_time;
        if (ds3231_get_time(&current_time) == ESP_OK) {
            char time_str[32];
            if (use_24hour_format) {
                snprintf(time_str, sizeof(time_str), "%02d:%02d:%02d", 
                        current_time.hour, current_time.minute, current_time.second);
            } else {
                // 12小时制格式
                int display_hour = current_time.hour;
                const char* am_pm = "AM";
                
                if (display_hour == 0) {
                    display_hour = 12;  // 午夜12点
                } else if (display_hour > 12) {
                    display_hour -= 12;
                    am_pm = "PM";
                } else if (display_hour == 12) {
                    am_pm = "PM";  // 中午12点
                }
                
                snprintf(time_str, sizeof(time_str), "%02d:%02d:%02d %s", 
                        display_hour, current_time.minute, current_time.second, am_pm);
            }
            lv_label_set_text(time_label, time_str);
        }
    }
    
    // 如果当前在设置页面的时间格式设置界面，也更新设置页面显示
    if (setting_page_active && setting_state == SETTING_STATE_TIME_FORMAT) {
        update_setting_display();
    }
}

/* 闹钟设置更新函数 - 供Web服务器调用 */
void update_alarm_settings(uint8_t hour, uint8_t minute, bool enabled)
{
    // 更新全局闹钟变量
    alarm_hours = hour;
    alarm_minutes = minute;
    alarm_enabled = enabled;
    
    // 记录日志
    ESP_LOGI(TAG, "闹钟设置已更新: %02d:%02d, 状态: %s", 
             alarm_hours, alarm_minutes, alarm_enabled ? "启用" : "禁用");
    
    // 如果闹钟已启用，将闹钟状态设置为已设置状态
    if (alarm_enabled) {
        alarm_state = ALARM_STATE_ALARM_SET;
    }
    
    // 立即更新显示（如果在闹钟页面）
    if (current_desktop == 1) {
        update_alarm_display();
    }
}

/* 定时器设置更新函数 - 供Web服务器调用 */
void update_timer_settings(uint8_t hours, uint8_t minutes, uint8_t seconds, bool running, const char* action)
{
    // 更新全局定时器变量
    timer_hours = hours;
    timer_minutes = minutes;
    timer_seconds = seconds;
    
    // 记录日志
    ESP_LOGI(TAG, "定时器设置已更新: %02d:%02d:%02d", timer_hours, timer_minutes, timer_seconds);
    
    // 根据动作执行不同操作
    if (strcmp(action, "start") == 0) {
        // 开始定时器
        timer_running = true;
        timer_state = TIMER_STATE_COUNTDOWN;
        countdown_hours = hours;
        countdown_minutes = minutes;
        countdown_seconds = seconds;
        timer_start_tick = xTaskGetTickCount();
        ESP_LOGI(TAG, "定时器已启动");
    } else if (strcmp(action, "stop") == 0) {
        // 停止定时器
        timer_running = false;
        if (timer_state == TIMER_STATE_COUNTDOWN) {
            timer_state = TIMER_STATE_MAIN;
        }
        ESP_LOGI(TAG, "定时器已停止");
    } else if (strcmp(action, "reset") == 0) {
        // 重置定时器
        timer_running = false;
        timer_state = TIMER_STATE_MAIN;
        countdown_hours = 0;
        countdown_minutes = 0;
        countdown_seconds = 0;
        ESP_LOGI(TAG, "定时器已重置");
    }
    
    // 立即更新显示（如果在定时器页面）
    if (current_desktop == 1) {
        update_timer_display();
    }
}

/* 事件提醒设置更新函数 - 供Web服务器调用 */
void update_reminder_settings(const char* title, const char* description, const char* datetime)
{
    // 记录日志
    ESP_LOGI(TAG, "事件提醒已更新: 标题=\"%s\", 描述=\"%s\", 时间=\"%s\"", 
             title, description ? description : "", datetime);
    
    // 存储事件提醒信息到全局变量
    strncpy(reminder_title, title, sizeof(reminder_title) - 1);
    reminder_title[sizeof(reminder_title) - 1] = '\0';
    
    if (description && strlen(description) > 0) {
        strncpy(reminder_description, description, sizeof(reminder_description) - 1);
    } else {
        reminder_description[0] = '\0';
    }
    reminder_description[sizeof(reminder_description) - 1] = '\0';
    
    strncpy(reminder_datetime, datetime, sizeof(reminder_datetime) - 1);
    reminder_datetime[sizeof(reminder_datetime) - 1] = '\0';
    
    // 设置提醒有效标志
    reminder_valid = true;
    
    // 立即更新显示（如果在闹钟页面）
    if (current_desktop == 2) {
        update_alarm_display();
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "Starting ESP32 Smart Desktop Assistant");
    
    /* 输出初始内存状态 */
    ESP_LOGI(TAG, "系统启动时可用堆内存: %ld 字节", esp_get_free_heap_size());
    ESP_LOGI(TAG, "最小可用堆内存: %ld 字节", esp_get_minimum_free_heap_size());
    
    /* 初始化LVGL */
    lv_init();
    
    /* 初始化显示器 */
    lv_port_disp_init();
    
    /* 初始化输入设备（空实现） */
    lv_port_indev_init();
    
    /* 初始化DS3231 */
    ESP_ERROR_CHECK(ds3231_init());
    
    /* 初始化时间（如果需要） */
    init_time_if_needed();
    
    /* 初始化农历缓存 */
    ESP_LOGI(TAG, "Initializing lunar calendar cache...");
    init_lunar_cache();
    
    /* 创建UI界面 */
    create_ui();
    
    /* 初始化EC11旋转编码器 */
    ESP_LOGI(TAG, "Initializing EC11 rotary encoder...");
    esp_err_t ec11_ret = ec11_init(ec11_event_callback);
    if (ec11_ret != ESP_OK) {
        ESP_LOGE(TAG, "EC11初始化失败: %s", esp_err_to_name(ec11_ret));
        ESP_LOGE(TAG, "系统将在没有EC11的情况下继续运行");
    } else {
        ESP_LOGI(TAG, "EC11旋转编码器初始化成功");
    }
    
    /* 初始化WiFi */
    ESP_LOGI(TAG, "Initializing WiFi...");
    esp_err_t wifi_ret = wifi_init();
    if (wifi_ret != ESP_OK) {
        ESP_LOGE(TAG, "WiFi初始化失败: %s", esp_err_to_name(wifi_ret));
        ESP_LOGE(TAG, "系统将在没有WiFi的情况下继续运行");
    }
    
    /* 初始化震动模块 */
    ESP_LOGI(TAG, "Initializing vibration motor...");
    esp_err_t vibration_ret = vibration_init();
    if (vibration_ret != ESP_OK) {
        ESP_LOGE(TAG, "震动模块初始化失败: %s", esp_err_to_name(vibration_ret));
        ESP_LOGE(TAG, "系统将在没有震动功能的情况下继续运行");
    } else {
        ESP_LOGI(TAG, "震动模块初始化成功");
    }
    
    /* 初始化MQ2烟雾传感器 */
    ESP_LOGI(TAG, "Initializing MQ2 sensor...");
    esp_err_t mq2_ret = mq2_sensor_init();
    if (mq2_ret != ESP_OK) {
        ESP_LOGE(TAG, "MQ2传感器初始化失败: %s", esp_err_to_name(mq2_ret));
        ESP_LOGE(TAG, "系统将在没有MQ2传感器的情况下继续运行");
    } else {
        ESP_LOGI(TAG, "MQ2传感器初始化成功");
    }
    
    /* 初始化音频播放器 */
    ESP_LOGI(TAG, "Initializing audio player...");
    esp_err_t audio_ret = audio_player_init();
    if (audio_ret != ESP_OK) {
        ESP_LOGE(TAG, "音频播放器初始化失败: %s", esp_err_to_name(audio_ret));
        ESP_LOGE(TAG, "系统将在没有音频播放功能的情况下继续运行");
    } else {
        ESP_LOGI(TAG, "音频播放器初始化成功");
        
        /* 初始化音频数据 */
        audio_data_init();
        ESP_LOGI(TAG, "音频数据初始化完成");
        
        /* 设置系统音量 */
        audio_player_set_volume(system_volume);
        ESP_LOGI(TAG, "系统音量设置为: %d%%", system_volume);
        
        /* 初始化SPIFFS文件系统 */
        ESP_LOGI(TAG, "初始化SPIFFS文件系统...");
        esp_err_t spiffs_ret = audio_spiffs_init();
        if (spiffs_ret == ESP_OK) {
            ESP_LOGI(TAG, "SPIFFS初始化成功，可以播放WAV文件");
            
            // 列出所有WAV文件
            char wav_files[10][64];
            int wav_count = audio_list_wav_files(wav_files, 10);
            if (wav_count > 0) {
                ESP_LOGI(TAG, "发现 %d 个WAV文件:", wav_count);
                for (int i = 0; i < wav_count; i++) {
                    ESP_LOGI(TAG, "  %d: %s", i+1, wav_files[i]);
                }
                
                // 播放第一个WAV文件作为启动音效
                ESP_LOGI(TAG, "播放启动音效: %s", wav_files[0]);
                audio_play_wav_file(wav_files[0], system_volume);
                vTaskDelay(pdMS_TO_TICKS(2000)); // 等待播放完成
            } else {
                ESP_LOGI(TAG, "未发现WAV文件，使用内置音效");
                /* 播放启动提示音 */
                ESP_LOGI(TAG, "播放启动提示音...");
                audio_player_play_pcm(startup_sound_data, startup_sound_size);
            }
        } else {
            ESP_LOGE(TAG, "SPIFFS初始化失败，使用内置音效");
            /* 播放启动提示音 */
            ESP_LOGI(TAG, "播放启动提示音...");
            audio_player_play_pcm(startup_sound_data, startup_sound_size);
        }
        
        /* 音频系统已初始化完成 */
        ESP_LOGI(TAG, "音频系统已准备就绪");
    }
    
    /* 创建LVGL tick任务 */
    xTaskCreate(lv_tick_task, "lv_tick_task", 2048, NULL, 5, NULL);
    
    /* 创建LVGL处理任务 */
    xTaskCreate(lvgl_task, "lvgl_task", 4096, NULL, 5, NULL);
    
    /* 创建时间更新任务 */
    xTaskCreate(time_update_task, "time_update_task", 4096, NULL, 4, NULL);
    
    /* 创建WiFi状态更新任务 */
    xTaskCreate(wifi_status_update_task, "wifi_status_task", 4096, NULL, 4, NULL);
    
    /* 创建天气更新任务 */
    xTaskCreate(weather_update_task, "weather_update_task", 4096, NULL, 4, NULL);
    
    /* 创建定时器任务 */
    xTaskCreate(timer_countdown_task, "timer_countdown_task", 2048, NULL, 4, NULL);
    
    /* 创建闹钟任务 */
    xTaskCreate(alarm_check_task, "alarm_check_task", 2048, NULL, 4, NULL);
    
    /* 创建天气预报获取任务 */
    xTaskCreate(weather_forecast_update_task, "weather_forecast_task", 4096, NULL, 3, NULL);
    
    /* 创建MQ2传感器更新任务 */
    if (mq2_ret == ESP_OK) {
        xTaskCreate(mq2_sensor_update_task, "mq2_sensor_task", 4096, NULL, 4, NULL);
        ESP_LOGI(TAG, "MQ2传感器更新任务已创建");
    }
    
    /* 创建内存监控任务 */
    xTaskCreate(memory_monitor_task, "memory_monitor", 2048, NULL, 2, NULL);
    ESP_LOGI(TAG, "内存监控任务已创建");
    
    /* 创建DHT11温湿度传感器更新任务 */
    xTaskCreate(dht11_update_task, "dht11_task", 3072, NULL, 4, NULL);
    ESP_LOGI(TAG, "DHT11温湿度传感器更新任务已创建");
    
    /* 连接WiFi（只有在WiFi初始化成功时才尝试连接） */
    if (wifi_ret == ESP_OK) {
        xTaskCreate(wifi_connect_task, "wifi_connect_task", 4096, NULL, 3, NULL);
        
        /* 初始化和启动Web服务器 */
        ESP_LOGI(TAG, "Initializing Web Server...");
        esp_err_t web_server_ret = web_server_init();
        if (web_server_ret != ESP_OK) {
            ESP_LOGE(TAG, "Web服务器初始化失败: %s", esp_err_to_name(web_server_ret));
        } else {
            /* 创建Web服务器启动任务 */
            xTaskCreate(web_server_start_task, "web_server_task", 4096, NULL, 3, NULL);
            ESP_LOGI(TAG, "Web服务器任务已创建");
        }
    }
    
    ESP_LOGI(TAG, "All tasks created successfully");
    
    /* 主任务结束，但其他任务继续运行 */
} 