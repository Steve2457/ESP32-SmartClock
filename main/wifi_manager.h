#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include "esp_err.h"
#include "esp_event.h"
#include "esp_wifi.h"

#ifdef __cplusplus
extern "C" {
#endif

/* WiFi配置信息 */
#define WIFI_SSID           "STEVE_HU"           // 请修改为你的WiFi名称
#define WIFI_PASSWORD       "11235813"         // 请修改为你的WiFi密码
#define WIFI_MAXIMUM_RETRY  5                   // 最大重连次数

/* WiFi连接状态 */
typedef enum {
    WIFI_STATUS_DISCONNECTED = 0,
    WIFI_STATUS_CONNECTING,
    WIFI_STATUS_CONNECTED,
    WIFI_STATUS_FAILED,
    WIFI_STATUS_SCANNING
} wifi_status_t;

/* WiFi扫描结果结构体 */
typedef struct {
    uint8_t ssid[33];     // SSID (最大32字符 + null终止符)
    int8_t rssi;          // 信号强度
    wifi_auth_mode_t authmode;  // 认证模式
} wifi_scan_result_t;

/**
 * @brief 初始化WiFi
 * @return ESP_OK on success
 */
esp_err_t wifi_init(void);

/**
 * @brief 连接WiFi
 * @return ESP_OK on success
 */
esp_err_t wifi_connect(void);

/**
 * @brief 断开WiFi连接
 * @return ESP_OK on success
 */
esp_err_t wifi_disconnect(void);

/**
 * @brief 获取WiFi连接状态
 * @return wifi_status_t 当前WiFi状态
 */
wifi_status_t wifi_get_status(void);

/**
 * @brief 获取IP地址字符串
 * @param ip_str 用于存储IP地址的字符串缓冲区
 * @param max_len 缓冲区最大长度
 * @return ESP_OK on success
 */
esp_err_t wifi_get_ip_string(char *ip_str, size_t max_len);

/* 新增WiFi扫描功能 */
esp_err_t wifi_start_scan(void);
int wifi_get_scan_results(wifi_scan_result_t *results, int max_results);
bool wifi_is_scan_done(void);
const char* wifi_get_last_error(void);

#ifdef __cplusplus
}
#endif

#endif /* WIFI_MANAGER_H */ 