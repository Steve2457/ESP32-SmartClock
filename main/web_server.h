#ifndef WEB_SERVER_H
#define WEB_SERVER_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化Web服务器
 * 
 * @return esp_err_t 成功返回ESP_OK，失败返回对应错误码
 */
esp_err_t web_server_init(void);

/**
 * @brief 启动Web服务器
 * 
 * @return esp_err_t 成功返回ESP_OK，失败返回对应错误码
 */
esp_err_t web_server_start(void);

/**
 * @brief 更新时钟状态到Web界面
 * 
 * @param use_24h_format 是否使用24小时制
 */
void web_server_update_clock_status(bool use_24h_format);

/**
 * @brief 更新闹钟状态到Web界面
 * 
 * @param hour 闹钟小时
 * @param minute 闹钟分钟
 * @param enabled 闹钟是否启用
 */
void web_server_update_alarm_status(uint8_t hour, uint8_t minute, bool enabled);

/**
 * @brief 更新定时器状态到Web界面
 * 
 * @param hours 定时器小时
 * @param minutes 定时器分钟
 * @param seconds 定时器秒数
 * @param running 定时器是否运行中
 */
void web_server_update_timer_status(uint8_t hours, uint8_t minutes, uint8_t seconds, bool running);

/**
 * @brief 推送系统状态到Web界面
 */
void web_server_push_system_status(void);

/**
 * @brief 获取当前活跃连接数
 * 
 * @return uint32_t 活跃连接数
 */
uint32_t web_server_get_active_connections(void);

/**
 * @brief 获取总连接数
 * 
 * @return uint32_t 总连接数
 */
uint32_t web_server_get_total_connections(void);

#ifdef __cplusplus
}
#endif

#endif /* WEB_SERVER_H */ 