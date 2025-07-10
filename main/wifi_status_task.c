#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "lvgl.h"
#include "wifi_manager.h"
#include "web_server.h"

static const char *TAG = "WIFI_STATUS";

// 从main.c中引用的外部变量
extern lv_obj_t *wifi_status_label;
extern lv_obj_t *wifi_scan_label;
extern lv_obj_t *title_label;
extern bool wifi_scan_requested;
extern TickType_t last_scan_time;
extern uint16_t connected_stations;
extern bool phone_connected;
extern TickType_t last_station_check;

#define SCAN_INTERVAL_MS 30000
#define STATION_CHECK_INTERVAL_MS 3000

/* WiFi状态更新任务 */
void wifi_status_update_task(void *arg)
{
    char wifi_str[128];
    char ip_str[32];
    char scan_str[512];  // WiFi扫描结果字符串
    bool prev_phone_connected = false;  // 上一次手机连接状态
    
    while (1) {
        wifi_status_t status = wifi_get_status();
        
        switch (status) {
            case WIFI_STATUS_DISCONNECTED:
                snprintf(wifi_str, sizeof(wifi_str), "WiFi: Disconnected - %s", wifi_get_last_error());
                connected_stations = 0;  // 断开时重置连接设备数
                phone_connected = false;
                break;
                
            case WIFI_STATUS_CONNECTING:
                strcpy(wifi_str, "WiFi: Connecting...");
                break;
                
            case WIFI_STATUS_CONNECTED:
                if (wifi_get_ip_string(ip_str, sizeof(ip_str)) == ESP_OK) {
                    snprintf(wifi_str, sizeof(wifi_str), "WiFi: Connected (%s)", ip_str);
                } else {
                    strcpy(wifi_str, "WiFi: Connected");
                }
                
                // 检查是否有手机连接
                TickType_t current_time = xTaskGetTickCount();
                if (current_time - last_station_check > pdMS_TO_TICKS(STATION_CHECK_INTERVAL_MS)) {
                    last_station_check = current_time;
                    
                    // 获取Web服务器的连接信息
                    uint32_t active = web_server_get_active_connections();
                    uint32_t total = web_server_get_total_connections();
                    
                    // 检查是否有手机连接
                    // 如果总连接数大于0，表示至少有过一个连接
                    connected_stations = active;
                    
                    // 如果有活跃连接或者在过去30秒内有过连接，则认为手机已连接
                    static TickType_t last_connection_time = 0;
                    static uint32_t last_total_connections = 0;
                    
                    if (active > 0 || total > last_total_connections) {
                        // 有新连接或活跃连接
                        phone_connected = true;
                        last_connection_time = current_time;
                        last_total_connections = total;
                    } else if (current_time - last_connection_time > pdMS_TO_TICKS(30000)) {
                        // 超过30秒无新连接，认为断开
                        phone_connected = false;
                    }
                        
                    // 在串口监视器上显示连接状态
                    ESP_LOGI(TAG, "手机连接状态: %s (活跃连接: %lu, 总连接数: %lu)", 
                            phone_connected ? "已连接" : "未连接", 
                            (unsigned long)active, (unsigned long)total);
                    
                    // 如果有HTTP连接，记录日志
                    if (active > 0) {
                        ESP_LOGI(TAG, "检测到活跃HTTP连接，已激活手机连接状态");
                    }
                    
                    // 如果连接状态变化，更新标题颜色
                    if (phone_connected != prev_phone_connected && title_label != NULL) {
                        if (phone_connected) {
                            // 设置标题为绿色
                            lv_obj_set_style_text_color(title_label, lv_color_make(0, 180, 0), 0);
                            ESP_LOGI(TAG, "标题颜色已更改为绿色，表示有手机连接");
                        } else {
                            // 恢复标题为黑色
                            lv_obj_set_style_text_color(title_label, lv_color_black(), 0);
                            ESP_LOGI(TAG, "标题颜色已恢复为黑色，表示无手机连接");
                        }
                        prev_phone_connected = phone_connected;
                    }
                }
                break;
                
            case WIFI_STATUS_FAILED:
                snprintf(wifi_str, sizeof(wifi_str), "WiFi: Failed - %s", wifi_get_last_error());
                connected_stations = 0;
                phone_connected = false;
                
                /* 如果连接失败且未请求扫描，开始扫描 */
                if (!wifi_scan_requested && 
                    (xTaskGetTickCount() - last_scan_time) > pdMS_TO_TICKS(SCAN_INTERVAL_MS)) {
                    ESP_LOGI(TAG, "WiFi connection failed, starting scan...");
                    wifi_start_scan();
                    wifi_scan_requested = true;
                    last_scan_time = xTaskGetTickCount();
                }
                break;
                
            case WIFI_STATUS_SCANNING:
                strcpy(wifi_str, "WiFi: Scanning networks...");
                break;
                
            default:
                strcpy(wifi_str, "WiFi: Unknown");
                break;
        }
        
        /* 更新WiFi状态显示 */
        if (wifi_status_label) {
            lv_label_set_text(wifi_status_label, wifi_str);
        }
        
        /* 检查WiFi扫描结果 */
        if (wifi_scan_requested && wifi_is_scan_done()) {
            wifi_scan_result_t scan_results[10];  // 最多显示10个网络
            int found_count = wifi_get_scan_results(scan_results, 10);
            
            if (found_count > 0) {
                strcpy(scan_str, "找到网络:\n");
                for (int i = 0; i < found_count && i < 3; i++) {  // 只显示前3个
                    char network_info[48];
                    // 缩短SSID显示，如果太长则截断
                    char short_ssid[16];
                    snprintf(short_ssid, sizeof(short_ssid), "%.15s", scan_results[i].ssid);
                    if (strlen((char*)scan_results[i].ssid) > 15) {
                        strcpy(short_ssid + 12, "...");  // 添加省略号
                    }
                    
                    snprintf(network_info, sizeof(network_info), "%s %ddBm\n", 
                            short_ssid, scan_results[i].rssi);
                    strcat(scan_str, network_info);
                }
            } else {
                strcpy(scan_str, "未找到网络");
            }
            
            /* 更新扫描结果显示 */
            if (wifi_scan_label) {
                lv_label_set_text(wifi_scan_label, scan_str);
            }
            
            wifi_scan_requested = false;
            ESP_LOGI(TAG, "Found %d WiFi networks", found_count);
        }
        
        vTaskDelay(pdMS_TO_TICKS(2000));  // 每2秒更新一次
    }
} 