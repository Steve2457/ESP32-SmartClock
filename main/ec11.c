#include "ec11.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "EC11";

/* EC11状态结构体 */
typedef struct {
    int s1_last;           // S1上次状态
    int s2_last;           // S2上次状态
    int key_last;          // 按键上次状态
    bool key_pressed;      // 按键按下标志
    TickType_t last_time;  // 上次检测时间（用于防抖）
} ec11_state_t;

static ec11_state_t ec11_state = {0};
static ec11_event_callback_t event_callback = NULL;
static TaskHandle_t ec11_task_handle = NULL;

/* 防抖时间定义（毫秒） */
#define EC11_DEBOUNCE_TIME_MS 5
#define EC11_KEY_DEBOUNCE_TIME_MS 50

/* EC11任务处理函数 */
static void ec11_task(void *arg)
{
    ec11_event_t event;
    TickType_t current_time;
    static TickType_t key_change_time = 0;  // 按键状态变化时间
    static bool key_debouncing = false;     // 按键防抖中标志
    static TickType_t last_debug_time = 0;  // 上次调试输出时间
    
    ESP_LOGI(TAG, "EC11任务开始运行");
    
    while (1) {
        current_time = xTaskGetTickCount();
        event.rotate = EC11_ROTATE_NONE;
        event.key = EC11_KEY_NONE;
        
        /* 读取当前GPIO状态 */
        int s1_current = gpio_get_level(EC11_S1_PIN);
        int s2_current = gpio_get_level(EC11_S2_PIN);
        int key_current = gpio_get_level(EC11_KEY_PIN);
        
        /* 每5秒输出一次GPIO状态，用于调试 */
        if ((current_time - last_debug_time) >= pdMS_TO_TICKS(5000)) {
            ESP_LOGI(TAG, "GPIO状态 - S1:%d S2:%d KEY:%d", s1_current, s2_current, key_current);
            last_debug_time = current_time;
        }
        
        /* 检测按键变化（简化防抖处理） */
        if (ec11_state.key_last != key_current) {
            if (!key_debouncing) {
                /* 检测到按键状态变化，开始防抖 */
                key_change_time = current_time;
                key_debouncing = true;
                ESP_LOGI(TAG, "按键状态变化: %d->%d, 开始防抖", ec11_state.key_last, key_current);
            }
        }
        
        /* 如果正在防抖，检查防抖时间是否结束 */
        if (key_debouncing) {
            if ((current_time - key_change_time) >= pdMS_TO_TICKS(EC11_KEY_DEBOUNCE_TIME_MS)) {
                /* 防抖时间结束，重新读取按键状态 */
                int key_stable = gpio_get_level(EC11_KEY_PIN);
                
                /* 检查按键状态是否确实发生了变化 */
                if (ec11_state.key_last != key_stable) {
                    if (key_stable == 0 && !ec11_state.key_pressed) {
                        /* 按键按下（下拉，按下为低电平） */
                        event.key = EC11_KEY_PRESSED;
                        ec11_state.key_pressed = true;
                        ESP_LOGI(TAG, "按键按下");
                    } else if (key_stable == 1 && ec11_state.key_pressed) {
                        /* 按键松开 */
                        event.key = EC11_KEY_RELEASED;
                        ec11_state.key_pressed = false;
                        ESP_LOGI(TAG, "按键松开");
                    }
                    ec11_state.key_last = key_stable;
                }
                key_debouncing = false;
            }
        } else {
            /* 如果没有在防抖状态，直接更新按键状态 */
            ec11_state.key_last = key_current;
        }
        
        /* 只有在没有按键事件且不在按键防抖期间时才检测旋转 */
        if (event.key == EC11_KEY_NONE && !key_debouncing) {
            /* 检测旋转编码器变化（防抖处理） */
            if ((current_time - ec11_state.last_time) >= pdMS_TO_TICKS(EC11_DEBOUNCE_TIME_MS)) {
                /* 检测S1信号的下降沿 */
                if (ec11_state.s1_last == 1 && s1_current == 0) {
                    if (s2_current == 1) {
                        /* S1下降沿时S2为高，右旋 */
                        event.rotate = EC11_ROTATE_RIGHT;
                        ESP_LOGI(TAG, "旋转编码器右旋");
                    } else {
                        /* S1下降沿时S2为低，左旋 */
                        event.rotate = EC11_ROTATE_LEFT;
                        ESP_LOGI(TAG, "旋转编码器左旋");
                    }
                    ec11_state.last_time = current_time;
                }
            }
        }
        
        /* 更新S1、S2状态 */
        ec11_state.s1_last = s1_current;
        ec11_state.s2_last = s2_current;
        
        /* 如果有事件且设置了回调函数，则调用回调 */
        if ((event.rotate != EC11_ROTATE_NONE || event.key != EC11_KEY_NONE) && 
            event_callback != NULL) {
            event_callback(&event);
        }
        
        vTaskDelay(pdMS_TO_TICKS(5)); // 5ms轮询间隔
    }
}

esp_err_t ec11_init(ec11_event_callback_t callback)
{
    ESP_LOGI(TAG, "初始化EC11旋转编码器...");
    
    /* 保存回调函数 */
    event_callback = callback;
    
    /* 配置S1引脚 */
    gpio_config_t s1_config = {
        .pin_bit_mask = (1ULL << EC11_S1_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    ESP_ERROR_CHECK(gpio_config(&s1_config));
    
    /* 配置S2引脚 */
    gpio_config_t s2_config = {
        .pin_bit_mask = (1ULL << EC11_S2_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    ESP_ERROR_CHECK(gpio_config(&s2_config));
    
    /* 配置KEY引脚 */
    gpio_config_t key_config = {
        .pin_bit_mask = (1ULL << EC11_KEY_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    ESP_ERROR_CHECK(gpio_config(&key_config));
    
    /* 初始化状态 */
    ec11_state.s1_last = gpio_get_level(EC11_S1_PIN);
    ec11_state.s2_last = gpio_get_level(EC11_S2_PIN);
    ec11_state.key_last = gpio_get_level(EC11_KEY_PIN);
    ec11_state.key_pressed = false;
    ec11_state.last_time = xTaskGetTickCount();
    
    /* 创建EC11处理任务 */
    BaseType_t task_result = xTaskCreate(
        ec11_task,
        "ec11_task",
        4096,  // 增加栈大小从2048到4096字节
        NULL,
        5,  // 优先级
        &ec11_task_handle
    );
    
    if (task_result != pdPASS) {
        ESP_LOGE(TAG, "创建EC11任务失败");
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "EC11旋转编码器初始化成功");
    return ESP_OK;
}

void ec11_get_event(ec11_event_t *event)
{
    if (event == NULL) {
        return;
    }
    
    /* 这个函数预留给轮询方式使用，当前使用任务方式 */
    event->rotate = EC11_ROTATE_NONE;
    event->key = EC11_KEY_NONE;
}

void ec11_deinit(void)
{
    /* 删除任务 */
    if (ec11_task_handle != NULL) {
        vTaskDelete(ec11_task_handle);
        ec11_task_handle = NULL;
    }
    
    /* 清除回调函数 */
    event_callback = NULL;
    
    ESP_LOGI(TAG, "EC11旋转编码器已清理");
} 