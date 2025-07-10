#ifndef WIFI_STATUS_TASK_H
#define WIFI_STATUS_TASK_H

#include "freertos/FreeRTOS.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief WiFi状态更新任务
 * 
 * 该任务负责更新WiFi状态显示，监控手机连接情况，
 * 并根据连接状态更新UI（标题颜色等）
 * 
 * @param arg 未使用
 */
void wifi_status_update_task(void *arg);

#ifdef __cplusplus
}
#endif

#endif /* WIFI_STATUS_TASK_H */ 