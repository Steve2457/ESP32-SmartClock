#ifndef EC11_H
#define EC11_H

#include "driver/gpio.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* EC11引脚定义 */
#define EC11_S1_PIN     GPIO_NUM_45
#define EC11_S2_PIN     GPIO_NUM_48
#define EC11_KEY_PIN    GPIO_NUM_47  // 改为GPIO47，因为GPIO57不存在

/* 旋转方向枚举 */
typedef enum {
    EC11_ROTATE_NONE = 0,
    EC11_ROTATE_LEFT,       // 左旋
    EC11_ROTATE_RIGHT       // 右旋
} ec11_rotate_t;

/* 按键状态枚举 */
typedef enum {
    EC11_KEY_NONE = 0,
    EC11_KEY_PRESSED,       // 按键按下
    EC11_KEY_RELEASED       // 按键松开
} ec11_key_t;

/* EC11事件结构体 */
typedef struct {
    ec11_rotate_t rotate;
    ec11_key_t key;
} ec11_event_t;

/* 事件回调函数类型 */
typedef void (*ec11_event_callback_t)(ec11_event_t *event);

/**
 * @brief 初始化EC11旋转编码器
 * 
 * @param callback 事件回调函数
 * @return esp_err_t 
 */
esp_err_t ec11_init(ec11_event_callback_t callback);

/**
 * @brief 获取当前EC11状态（轮询方式）
 * 
 * @param event 输出事件结构体
 */
void ec11_get_event(ec11_event_t *event);

/**
 * @brief 清理EC11资源
 */
void ec11_deinit(void);

#ifdef __cplusplus
}
#endif

#endif /* EC11_H */ 