#include "web_server.h"
#include "ds3231.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_http_client.h"
#include "cJSON.h"
#include "wifi_manager.h"
#include <string.h>

static const char *TAG = "WEB_SERVER";

static httpd_handle_t server_handle = NULL;

// 客户端连接计数器
static uint32_t active_connections = 0;
static uint32_t total_connection_counter = 0;

// 系统状态存储
static struct {
    bool time_format_24h;
    struct {
        uint8_t hour;
        uint8_t minute;
        bool enabled;
    } alarm;
    struct {
        uint8_t hours;
        uint8_t minutes; 
        uint8_t seconds;
        bool running;
    } timer;
    struct {
        char title[64];
        char description[128];
        char datetime[32]; // ISO format: YYYY-MM-DDThh:mm:ss
    } reminder;
} system_status = {
    .time_format_24h = true,
    .alarm = {0, 0, false},
    .timer = {0, 0, 0, false},
    .reminder = {"", "", ""}
};

// HTTP处理函数 - 根路径
static esp_err_t root_handler(httpd_req_t *req)
{
    // 增加连接计数器
    active_connections++;
    total_connection_counter++;
    
    ESP_LOGI(TAG, "新连接已建立! 活动连接: %lu, 总连接数: %lu", 
            (unsigned long)active_connections, (unsigned long)total_connection_counter);
            
    const char* resp_str = "ESP32 Smart Desktop Assistant API";
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, resp_str, strlen(resp_str));
    
    // 延迟减少活动连接计数，因为连接可能会立即关闭
    active_connections--;
    
    return ESP_OK;
}

// HTTP处理函数 - 获取设备状态
static esp_err_t get_status_handler(httpd_req_t *req)
{
    // 增加连接计数器
    active_connections++;
    total_connection_counter++;
    
    ESP_LOGI(TAG, "状态请求已接收! 活动连接: %lu, 总连接数: %lu",
            (unsigned long)active_connections, (unsigned long)total_connection_counter);

    cJSON *response = cJSON_CreateObject();
    
    // 获取当前时间
    ds3231_time_t current_time;
    if (ds3231_get_time(&current_time) == ESP_OK) {
        cJSON *time_obj = cJSON_CreateObject();
        cJSON_AddNumberToObject(time_obj, "year", current_time.year);
        cJSON_AddNumberToObject(time_obj, "month", current_time.month);
        cJSON_AddNumberToObject(time_obj, "date", current_time.date);
        cJSON_AddNumberToObject(time_obj, "hour", current_time.hour);
        cJSON_AddNumberToObject(time_obj, "minute", current_time.minute);
        cJSON_AddNumberToObject(time_obj, "second", current_time.second);
        cJSON_AddItemToObject(response, "current_time", time_obj);
    }
    
    // 添加时间格式设置
    cJSON_AddBoolToObject(response, "time_format_24h", system_status.time_format_24h);
    
    // 添加闹钟信息
    cJSON *alarm_obj = cJSON_CreateObject();
    cJSON_AddNumberToObject(alarm_obj, "hour", system_status.alarm.hour);
    cJSON_AddNumberToObject(alarm_obj, "minute", system_status.alarm.minute);
    cJSON_AddBoolToObject(alarm_obj, "enabled", system_status.alarm.enabled);
    cJSON_AddItemToObject(response, "alarm", alarm_obj);
    
    // 添加定时器信息
    cJSON *timer_obj = cJSON_CreateObject();
    cJSON_AddNumberToObject(timer_obj, "hours", system_status.timer.hours);
    cJSON_AddNumberToObject(timer_obj, "minutes", system_status.timer.minutes);
    cJSON_AddNumberToObject(timer_obj, "seconds", system_status.timer.seconds);
    cJSON_AddBoolToObject(timer_obj, "running", system_status.timer.running);
    cJSON_AddItemToObject(response, "timer", timer_obj);
    
    // 添加提醒信息
    if (strlen(system_status.reminder.title) > 0) {
        cJSON *reminder_obj = cJSON_CreateObject();
        cJSON_AddStringToObject(reminder_obj, "title", system_status.reminder.title);
        cJSON_AddStringToObject(reminder_obj, "description", system_status.reminder.description);
        cJSON_AddStringToObject(reminder_obj, "datetime", system_status.reminder.datetime);
        cJSON_AddItemToObject(response, "reminder", reminder_obj);
    }
    
    // 转换为字符串
    char *json_str = cJSON_Print(response);
    
    // 发送响应
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, strlen(json_str));
    
    // 释放内存
    free(json_str);
    cJSON_Delete(response);
    
    // 减少活动连接计数
    active_connections--;
    
    return ESP_OK;
}

// 处理JSON请求体，提取内容到buffer
static esp_err_t parse_json_body(httpd_req_t *req, char *buffer, size_t buffer_size)
{
    int total_len = req->content_len;
    int cur_len = 0;
    
    if (total_len >= buffer_size) {
        // 请求体太大
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Content too large");
        return ESP_FAIL;
    }
    
    // 读取请求体数据
    int received = 0;
    while (cur_len < total_len) {
        received = httpd_req_recv(req, buffer + cur_len, total_len - cur_len);
        if (received <= 0) {
            // 连接关闭或错误
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to read request");
            return ESP_FAIL;
        }
        cur_len += received;
    }
    buffer[cur_len] = '\0';  // 添加字符串结束符
    
    return ESP_OK;
}

// HTTP处理函数 - 设置时间
static esp_err_t set_time_handler(httpd_req_t *req)
{
    char buffer[256] = {0};
    if (parse_json_body(req, buffer, sizeof(buffer)) != ESP_OK) {
        return ESP_FAIL;
    }
    
    cJSON *json = cJSON_Parse(buffer);
    if (!json) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }
    
    // 提取时间参数
    ds3231_time_t time = {0};
    bool valid_time = true;
    
    cJSON *year = cJSON_GetObjectItem(json, "year");
    cJSON *month = cJSON_GetObjectItem(json, "month");
    cJSON *date = cJSON_GetObjectItem(json, "date");
    cJSON *hour = cJSON_GetObjectItem(json, "hour");
    cJSON *minute = cJSON_GetObjectItem(json, "minute");
    cJSON *second = cJSON_GetObjectItem(json, "second");
    
    // 验证必须参数
    if (!year || !month || !date || !hour || !minute) {
        valid_time = false;
    } else {
        // 提取值并验证范围
        time.year = year->valueint;
        time.month = month->valueint;
        time.date = date->valueint;
        time.hour = hour->valueint;
        time.minute = minute->valueint;
        time.second = second ? second->valueint : 0;
        time.day_of_week = 1; // 暂时不设置星期几
        
        // 验证值范围
        if (time.year < 2000 || time.year > 2099 ||
            time.month < 1 || time.month > 12 ||
            time.date < 1 || time.date > 31 ||
            time.hour > 23 || time.minute > 59 || time.second > 59) {
            valid_time = false;
        }
    }
    
    cJSON_Delete(json);
    
    // 如果时间有效，设置RTC
    if (valid_time) {
        esp_err_t err = ds3231_set_time(&time);
        if (err != ESP_OK) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to set time");
            return ESP_FAIL;
        }
        
        // 发送成功响应
        const char* resp_str = "{\"status\":\"ok\",\"message\":\"Time set successfully\"}";
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, resp_str, strlen(resp_str));
        
        return ESP_OK;
    } else {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid time parameters");
        return ESP_FAIL;
    }
}

// HTTP处理函数 - 设置时间格式
static esp_err_t set_time_format_handler(httpd_req_t *req)
{
    char buffer[64] = {0};
    if (parse_json_body(req, buffer, sizeof(buffer)) != ESP_OK) {
        return ESP_FAIL;
    }
    
    cJSON *json = cJSON_Parse(buffer);
    if (!json) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }
    
    // 提取时间格式参数
    cJSON *format_24h = cJSON_GetObjectItem(json, "format_24h");
    if (!format_24h || !cJSON_IsBool(format_24h)) {
        cJSON_Delete(json);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing or invalid 'format_24h' parameter");
        return ESP_FAIL;
    }
    
    // 更新系统状态
    bool new_format = cJSON_IsTrue(format_24h);
    system_status.time_format_24h = new_format;
    
    // 声明外部函数，用于直接更新主程序中的时间格式
    extern void update_time_format(bool use_24h_format);
    
    // 调用外部函数更新时间格式
    update_time_format(new_format);
    
    ESP_LOGI(TAG, "时间格式已通过Web接口更新为: %s", new_format ? "24小时制" : "12小时制");
    
    cJSON_Delete(json);
    
    // 发送成功响应
    const char* resp_str = "{\"status\":\"ok\",\"message\":\"Time format updated\"}";
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp_str, strlen(resp_str));
    
    return ESP_OK;
}

// HTTP处理函数 - 设置闹钟
static esp_err_t set_alarm_handler(httpd_req_t *req)
{
    char buffer[128] = {0};
    if (parse_json_body(req, buffer, sizeof(buffer)) != ESP_OK) {
        return ESP_FAIL;
    }
    
    cJSON *json = cJSON_Parse(buffer);
    if (!json) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }
    
    // 提取闹钟参数
    cJSON *hour = cJSON_GetObjectItem(json, "hour");
    cJSON *minute = cJSON_GetObjectItem(json, "minute");
    cJSON *enabled = cJSON_GetObjectItem(json, "enabled");
    
    if (!hour || !minute || !enabled ||
        !cJSON_IsNumber(hour) || !cJSON_IsNumber(minute) || !cJSON_IsBool(enabled) ||
        hour->valueint < 0 || hour->valueint > 23 ||
        minute->valueint < 0 || minute->valueint > 59) {
        
        cJSON_Delete(json);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid alarm parameters");
        return ESP_FAIL;
    }
    
    // 更新系统状态
    uint8_t alarm_hour = hour->valueint;
    uint8_t alarm_minute = minute->valueint;
    bool alarm_enabled = cJSON_IsTrue(enabled);
    
    system_status.alarm.hour = alarm_hour;
    system_status.alarm.minute = alarm_minute;
    system_status.alarm.enabled = alarm_enabled;
    
    // 声明外部函数，用于直接更新主程序中的闹钟设置
    extern void update_alarm_settings(uint8_t hour, uint8_t minute, bool enabled);
    
    // 调用外部函数更新闹钟设置
    update_alarm_settings(alarm_hour, alarm_minute, alarm_enabled);
    
    ESP_LOGI(TAG, "闹钟已通过Web接口更新: %02d:%02d, 状态: %s", 
             alarm_hour, alarm_minute, alarm_enabled ? "启用" : "禁用");
    
    cJSON_Delete(json);
    
    // 发送成功响应
    const char* resp_str = "{\"status\":\"ok\",\"message\":\"Alarm set successfully\"}";
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp_str, strlen(resp_str));
    
    return ESP_OK;
}

// HTTP处理函数 - 设置定时器
static esp_err_t set_timer_handler(httpd_req_t *req)
{
    char buffer[128] = {0};
    if (parse_json_body(req, buffer, sizeof(buffer)) != ESP_OK) {
        return ESP_FAIL;
    }
    
    cJSON *json = cJSON_Parse(buffer);
    if (!json) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }
    
    // 提取定时器参数
    cJSON *hours = cJSON_GetObjectItem(json, "hours");
    cJSON *minutes = cJSON_GetObjectItem(json, "minutes");
    cJSON *seconds = cJSON_GetObjectItem(json, "seconds");
    cJSON *action = cJSON_GetObjectItem(json, "action"); // "start", "stop", "reset"
    
    // 检查参数有效性
    if (!hours || !minutes || !seconds || !action ||
        !cJSON_IsNumber(hours) || !cJSON_IsNumber(minutes) || !cJSON_IsNumber(seconds) ||
        !cJSON_IsString(action) ||
        hours->valueint < 0 || minutes->valueint < 0 || seconds->valueint < 0) {
        
        cJSON_Delete(json);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid timer parameters");
        return ESP_FAIL;
    }
    
    // 更新系统状态
    uint8_t timer_hours = hours->valueint;
    uint8_t timer_minutes = minutes->valueint;
    uint8_t timer_seconds = seconds->valueint;
    bool timer_running = false;
    
    system_status.timer.hours = timer_hours;
    system_status.timer.minutes = timer_minutes;
    system_status.timer.seconds = timer_seconds;
    
    // 处理动作
    const char *action_str = action->valuestring;
    if (strcmp(action_str, "start") == 0) {
        system_status.timer.running = true;
        timer_running = true;
    } else if (strcmp(action_str, "stop") == 0) {
        system_status.timer.running = false;
        timer_running = false;
    } else if (strcmp(action_str, "reset") == 0) {
        system_status.timer.running = false;
        timer_running = false;
    } else {
        cJSON_Delete(json);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid action parameter");
        return ESP_FAIL;
    }
    
    // 声明外部函数，用于直接更新主程序中的定时器设置
    extern void update_timer_settings(uint8_t hours, uint8_t minutes, uint8_t seconds, bool running, const char* action);
    
    // 调用外部函数更新定时器设置
    update_timer_settings(timer_hours, timer_minutes, timer_seconds, timer_running, action_str);
    
    ESP_LOGI(TAG, "定时器已通过Web接口更新: %02d:%02d:%02d, 动作: %s", 
             timer_hours, timer_minutes, timer_seconds, action_str);
    
    cJSON_Delete(json);
    
    // 发送成功响应
    const char* resp_str = "{\"status\":\"ok\",\"message\":\"Timer updated successfully\"}";
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp_str, strlen(resp_str));
    
    return ESP_OK;
}

// HTTP处理函数 - 设置提醒
static esp_err_t set_reminder_handler(httpd_req_t *req)
{
    char buffer[512] = {0};
    if (parse_json_body(req, buffer, sizeof(buffer)) != ESP_OK) {
        return ESP_FAIL;
    }
    
    cJSON *json = cJSON_Parse(buffer);
    if (!json) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }
    
    // 提取提醒参数
    cJSON *title = cJSON_GetObjectItem(json, "title");
    cJSON *description = cJSON_GetObjectItem(json, "description");
    cJSON *datetime = cJSON_GetObjectItem(json, "datetime");
    
    // 检查参数有效性
    if (!title || !datetime ||
        !cJSON_IsString(title) || !cJSON_IsString(datetime) ||
        (description && !cJSON_IsString(description))) {
        
        cJSON_Delete(json);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid reminder parameters");
        return ESP_FAIL;
    }
    
    // 更新系统状态
    strncpy(system_status.reminder.title, title->valuestring, sizeof(system_status.reminder.title) - 1);
    system_status.reminder.title[sizeof(system_status.reminder.title) - 1] = '\0';
    
    if (description) {
        strncpy(system_status.reminder.description, description->valuestring, sizeof(system_status.reminder.description) - 1);
    } else {
        system_status.reminder.description[0] = '\0';
    }
    system_status.reminder.description[sizeof(system_status.reminder.description) - 1] = '\0';
    
    strncpy(system_status.reminder.datetime, datetime->valuestring, sizeof(system_status.reminder.datetime) - 1);
    system_status.reminder.datetime[sizeof(system_status.reminder.datetime) - 1] = '\0';
    
    // 声明外部函数，用于直接更新主程序中的事件提醒设置
    extern void update_reminder_settings(const char* title, const char* description, const char* datetime);
    
    // 调用外部函数更新事件提醒设置
    update_reminder_settings(
        title->valuestring, 
        description ? description->valuestring : NULL, 
        datetime->valuestring
    );
    
    ESP_LOGI(TAG, "事件提醒已通过Web接口更新: 标题=\"%s\", 时间=\"%s\"", 
             title->valuestring, datetime->valuestring);
    
    cJSON_Delete(json);
    
    // 发送成功响应
    const char* resp_str = "{\"status\":\"ok\",\"message\":\"Reminder set successfully\"}";
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp_str, strlen(resp_str));
    
    return ESP_OK;
}

// 初始化Web服务器
esp_err_t web_server_init(void)
{
    ESP_LOGI(TAG, "Initializing web server");
    
    // 默认初始化系统状态
    system_status.time_format_24h = true;  // 默认24小时制
    system_status.alarm.hour = 7;
    system_status.alarm.minute = 0;
    system_status.alarm.enabled = false;
    system_status.timer.hours = 0;
    system_status.timer.minutes = 0;
    system_status.timer.seconds = 0;
    system_status.timer.running = false;
    memset(system_status.reminder.title, 0, sizeof(system_status.reminder.title));
    memset(system_status.reminder.description, 0, sizeof(system_status.reminder.description));
    memset(system_status.reminder.datetime, 0, sizeof(system_status.reminder.datetime));
    
    return ESP_OK;
}

// 启动Web服务器
esp_err_t web_server_start(void)
{
    if (server_handle != NULL) {
        ESP_LOGW(TAG, "Web server already started");
        return ESP_OK;
    }
    
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 10;
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.core_id = 0;   // 在核心0上运行
    config.stack_size = 8192;  // 增加栈大小
    
    ESP_LOGI(TAG, "Starting web server on port %d", config.server_port);
    esp_err_t ret = httpd_start(&server_handle, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start web server: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // 注册URI处理程序
    
    // Root handler
    httpd_uri_t root = {
        .uri       = "/",
        .method    = HTTP_GET,
        .handler   = root_handler,
        .user_ctx  = NULL
    };
    httpd_register_uri_handler(server_handle, &root);
    
    // Status handler
    httpd_uri_t status = {
        .uri       = "/api/status",
        .method    = HTTP_GET,
        .handler   = get_status_handler,
        .user_ctx  = NULL
    };
    httpd_register_uri_handler(server_handle, &status);
    
    // Set time handler
    httpd_uri_t set_time = {
        .uri       = "/api/time",
        .method    = HTTP_POST,
        .handler   = set_time_handler,
        .user_ctx  = NULL
    };
    httpd_register_uri_handler(server_handle, &set_time);
    
    // Set time format handler
    httpd_uri_t set_time_format = {
        .uri       = "/api/time_format",
        .method    = HTTP_POST,
        .handler   = set_time_format_handler,
        .user_ctx  = NULL
    };
    httpd_register_uri_handler(server_handle, &set_time_format);
    
    // Set alarm handler
    httpd_uri_t set_alarm = {
        .uri       = "/api/alarm",
        .method    = HTTP_POST,
        .handler   = set_alarm_handler,
        .user_ctx  = NULL
    };
    httpd_register_uri_handler(server_handle, &set_alarm);
    
    // Set timer handler
    httpd_uri_t set_timer = {
        .uri       = "/api/timer",
        .method    = HTTP_POST,
        .handler   = set_timer_handler,
        .user_ctx  = NULL
    };
    httpd_register_uri_handler(server_handle, &set_timer);
    
    // Set reminder handler
    httpd_uri_t set_reminder = {
        .uri       = "/api/reminder",
        .method    = HTTP_POST,
        .handler   = set_reminder_handler,
        .user_ctx  = NULL
    };
    httpd_register_uri_handler(server_handle, &set_reminder);
    
    ESP_LOGI(TAG, "Web server started successfully");
    
    return ESP_OK;
}

// 停止Web服务器
esp_err_t web_server_stop(void)
{
    if (server_handle == NULL) {
        ESP_LOGW(TAG, "Web server not running");
        return ESP_OK;
    }
    
    ESP_LOGI(TAG, "Stopping web server");
    esp_err_t ret = httpd_stop(server_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to stop web server: %s", esp_err_to_name(ret));
        return ret;
    }
    
    server_handle = NULL;
    
    ESP_LOGI(TAG, "Web server stopped");
    return ESP_OK;
}

// 更新闹钟状态
void web_server_update_alarm_status(uint8_t hour, uint8_t minute, bool enabled)
{
    system_status.alarm.hour = hour;
    system_status.alarm.minute = minute;
    system_status.alarm.enabled = enabled;
}

// 更新定时器状态
void web_server_update_timer_status(uint8_t hours, uint8_t minutes, uint8_t seconds, bool running)
{
    system_status.timer.hours = hours;
    system_status.timer.minutes = minutes;
    system_status.timer.seconds = seconds;
    system_status.timer.running = running;
}

// 更新时钟状态
void web_server_update_clock_status(bool use_24hour_format)
{
    system_status.time_format_24h = use_24hour_format;
}

// 推送系统状态
void web_server_push_system_status(void)
{
    // 简化版本不支持主动推送
}

// 获取活动连接数
uint32_t web_server_get_active_connections(void)
{
    return active_connections;
}

// 获取总连接计数
uint32_t web_server_get_total_connections(void)
{
    return total_connection_counter;
}
 