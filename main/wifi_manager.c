#include "wifi_manager.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "lwip/err.h"
#include "lwip/sys.h"
#include "nvs_flash.h"
#include <string.h>
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_event.h"

static const char *TAG = "WIFI_MANAGER";

/* WiFi事件组 */
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
#define WIFI_SCAN_DONE_BIT BIT2

static EventGroupHandle_t s_wifi_event_group;
static int s_retry_num = 0;
static wifi_status_t s_wifi_status = WIFI_STATUS_DISCONNECTED;
static esp_netif_t *s_sta_netif = NULL;

/* WiFi扫描相关变量 */
static uint16_t s_scan_ap_count = 0;
static wifi_ap_record_t *s_scan_ap_records = NULL;
static bool s_scan_done = false;
static char s_last_error[128] = "";

/* 记录错误信息 */
static void set_last_error(const char* error_msg)
{
    strncpy(s_last_error, error_msg, sizeof(s_last_error) - 1);
    s_last_error[sizeof(s_last_error) - 1] = '\0';
}

/* WiFi事件处理函数 */
static void event_handler(void* arg, esp_event_base_t event_base,
                         int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
            case WIFI_EVENT_STA_START:
                ESP_LOGI(TAG, "WiFi station started");
                break;
                
            case WIFI_EVENT_STA_DISCONNECTED:
                {
                    wifi_event_sta_disconnected_t* disconnected = (wifi_event_sta_disconnected_t*) event_data;
                    ESP_LOGI(TAG, "WiFi disconnected, reason: %d", disconnected->reason);
                    
                    // 根据断开原因设置错误信息
                    switch (disconnected->reason) {
                        case WIFI_REASON_NO_AP_FOUND:
                            set_last_error("SSID not found");
                            break;
                        case WIFI_REASON_AUTH_FAIL:
                            set_last_error("Authentication failed (wrong password)");
                            break;
                        case WIFI_REASON_ASSOC_FAIL:
                            set_last_error("Association failed");
                            break;
                        case WIFI_REASON_HANDSHAKE_TIMEOUT:
                            set_last_error("Handshake timeout");
                            break;
                        default:
                            snprintf(s_last_error, sizeof(s_last_error), "Disconnect reason: %d", disconnected->reason);
                            break;
                    }
                    
                    if (s_retry_num < WIFI_MAXIMUM_RETRY) {
                        esp_wifi_connect();
                        s_retry_num++;
                        s_wifi_status = WIFI_STATUS_CONNECTING;
                        ESP_LOGI(TAG, "Retry to connect to the AP (attempt %d/%d)", s_retry_num, WIFI_MAXIMUM_RETRY);
                    } else {
                        xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
                        s_wifi_status = WIFI_STATUS_FAILED;
                        ESP_LOGE(TAG, "Failed to connect to WiFi after %d attempts", WIFI_MAXIMUM_RETRY);
                    }
                }
                break;
                
            case WIFI_EVENT_SCAN_DONE:
                ESP_LOGI(TAG, "WiFi scan completed");
                s_scan_done = true;
                xEventGroupSetBits(s_wifi_event_group, WIFI_SCAN_DONE_BIT);
                break;
                
            default:
                break;
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Got IP:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        s_wifi_status = WIFI_STATUS_CONNECTED;
        set_last_error("Connected successfully");
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

esp_err_t wifi_init(void)
{
    ESP_LOGI(TAG, "Initializing WiFi...");
    
    /* 初始化NVS */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    
    /* 创建事件组 */
    s_wifi_event_group = xEventGroupCreate();
    if (s_wifi_event_group == NULL) {
        ESP_LOGE(TAG, "Failed to create event group");
        return ESP_FAIL;
    }
    
    /* 初始化网络接口 */
    ESP_ERROR_CHECK(esp_netif_init());
    
    /* 创建默认事件循环 */
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    
    /* 创建WiFi station接口 */
    s_sta_netif = esp_netif_create_default_wifi_sta();
    
    /* 初始化WiFi */
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    
    /* 注册事件处理器 */
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL));
    
    ESP_LOGI(TAG, "WiFi initialized successfully");
    return ESP_OK;
}

esp_err_t wifi_connect(void)
{
    ESP_LOGI(TAG, "Connecting to WiFi SSID: %s", WIFI_SSID);
    
    /* 配置WiFi */
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASSWORD,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    
    /* 设置WiFi模式和配置 */
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    
    s_wifi_status = WIFI_STATUS_CONNECTING;
    set_last_error("Connecting...");
    
    /* 开始连接 */
    esp_err_t ret = esp_wifi_connect();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start WiFi connection: %s", esp_err_to_name(ret));
        s_wifi_status = WIFI_STATUS_FAILED;
        set_last_error("Failed to start connection");
        return ret;
    }
    
    /* 等待连接结果 */
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
            pdFALSE,
            pdFALSE,
            portMAX_DELAY);
    
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Connected to WiFi successfully");
        return ESP_OK;
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGE(TAG, "Failed to connect to WiFi");
        return ESP_FAIL;
    } else {
        ESP_LOGE(TAG, "Unexpected event");
        return ESP_FAIL;
    }
}

esp_err_t wifi_disconnect(void)
{
    esp_err_t ret = esp_wifi_disconnect();
    if (ret == ESP_OK) {
        s_wifi_status = WIFI_STATUS_DISCONNECTED;
        ESP_LOGI(TAG, "WiFi已断开连接");
    }
    return ret;
}

wifi_status_t wifi_get_status(void)
{
    return s_wifi_status;
}

esp_err_t wifi_get_ip_string(char *ip_str, size_t max_len)
{
    if (s_sta_netif == NULL || ip_str == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    esp_netif_ip_info_t ip_info;
    esp_err_t ret = esp_netif_get_ip_info(s_sta_netif, &ip_info);
    if (ret != ESP_OK) {
        return ret;
    }
    
    snprintf(ip_str, max_len, IPSTR, IP2STR(&ip_info.ip));
    return ESP_OK;
}

/* 开始WiFi扫描 */
esp_err_t wifi_start_scan(void)
{
    ESP_LOGI(TAG, "Starting WiFi scan...");
    
    /* 确保WiFi已启动 */
    esp_err_t ret = esp_wifi_set_mode(WIFI_MODE_STA);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set WiFi mode: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ret = esp_wifi_start();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "WiFi start returned: %s", esp_err_to_name(ret));
        // 继续执行，可能WiFi已经启动
    }
    
    s_scan_done = false;
    s_wifi_status = WIFI_STATUS_SCANNING;
    
    /* 开始扫描 */
    ret = esp_wifi_scan_start(NULL, false);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start scan: %s", esp_err_to_name(ret));
        s_wifi_status = WIFI_STATUS_DISCONNECTED;
        return ret;
    }
    
    return ESP_OK;
}

/* 获取扫描结果 */
int wifi_get_scan_results(wifi_scan_result_t *results, int max_results)
{
    if (!s_scan_done || results == NULL || max_results <= 0) {
        return 0;
    }
    
    /* 获取扫描到的AP数量 */
    esp_err_t ret = esp_wifi_scan_get_ap_num(&s_scan_ap_count);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get AP count: %s", esp_err_to_name(ret));
        return 0;
    }
    
    if (s_scan_ap_count == 0) {
        ESP_LOGI(TAG, "No WiFi networks found");
        return 0;
    }
    
    /* 分配内存存储扫描结果 */
    if (s_scan_ap_records) {
        free(s_scan_ap_records);
    }
    s_scan_ap_records = malloc(sizeof(wifi_ap_record_t) * s_scan_ap_count);
    if (s_scan_ap_records == NULL) {
        ESP_LOGE(TAG, "Failed to allocate memory for scan results");
        return 0;
    }
    
    /* 获取扫描结果 */
    ret = esp_wifi_scan_get_ap_records(&s_scan_ap_count, s_scan_ap_records);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get scan results: %s", esp_err_to_name(ret));
        free(s_scan_ap_records);
        s_scan_ap_records = NULL;
        return 0;
    }
    
    /* 复制结果到用户缓冲区 */
    int copy_count = (s_scan_ap_count < max_results) ? s_scan_ap_count : max_results;
    for (int i = 0; i < copy_count; i++) {
        memcpy(results[i].ssid, s_scan_ap_records[i].ssid, sizeof(results[i].ssid));
        results[i].rssi = s_scan_ap_records[i].rssi;
        results[i].authmode = s_scan_ap_records[i].authmode;
    }
    
    ESP_LOGI(TAG, "Retrieved %d WiFi networks", copy_count);
    return copy_count;
}

/* 检查扫描是否完成 */
bool wifi_is_scan_done(void)
{
    return s_scan_done;
}

/* 获取最后的错误信息 */
const char* wifi_get_last_error(void)
{
    return s_last_error;
} 