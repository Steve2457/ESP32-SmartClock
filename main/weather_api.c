#include "weather_api.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"
#include "wifi_manager.h"
#include <string.h>
#include "secrets.h"

static const char *TAG = "WeatherAPI";

// 高德天气API配置
#define MAX_HTTP_RECV_BUFFER 2048

static char http_response_buffer[MAX_HTTP_RECV_BUFFER];
static int response_len = 0;

/* HTTP事件处理函数 */
static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    switch(evt->event_id) {
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
            ESP_LOGI(TAG, "HTTP_EVENT_ON_DATA, len=%d", evt->data_len);
            if (!esp_http_client_is_chunked_response(evt->client)) {
                if (response_len + evt->data_len < MAX_HTTP_RECV_BUFFER) {
                    memcpy(http_response_buffer + response_len, evt->data, evt->data_len);
                    response_len += evt->data_len;
                }
            }
            break;
        case HTTP_EVENT_ON_FINISH:
            ESP_LOGI(TAG, "HTTP_EVENT_ON_FINISH");
            http_response_buffer[response_len] = '\0';
            break;
        case HTTP_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "HTTP_EVENT_DISCONNECTED");
            break;
        case HTTP_EVENT_REDIRECT:
            ESP_LOGI(TAG, "HTTP_EVENT_REDIRECT");
            break;
    }
    return ESP_OK;
}

/* 解析天气JSON响应 */
static esp_err_t parse_weather_response(const char* json_string, weather_info_t* weather_info)
{
    cJSON *json = cJSON_Parse(json_string);
    if (json == NULL) {
        ESP_LOGE(TAG, "Failed to parse JSON");
        return ESP_FAIL;
    }

    // 检查status字段
    cJSON *status = cJSON_GetObjectItem(json, "status");
    if (status == NULL || !cJSON_IsString(status) || strcmp(status->valuestring, "1") != 0) {
        ESP_LOGE(TAG, "API returned error status");
        cJSON_Delete(json);
        return ESP_FAIL;
    }

    // 获取lives数组
    cJSON *lives = cJSON_GetObjectItem(json, "lives");
    if (lives == NULL || !cJSON_IsArray(lives)) {
        ESP_LOGE(TAG, "No lives array found");
        cJSON_Delete(json);
        return ESP_FAIL;
    }

    cJSON *live_data = cJSON_GetArrayItem(lives, 0);
    if (live_data == NULL) {
        ESP_LOGE(TAG, "No live data found");
        cJSON_Delete(json);
        return ESP_FAIL;
    }

    // 解析各个字段
    cJSON *province = cJSON_GetObjectItem(live_data, "province");
    cJSON *city = cJSON_GetObjectItem(live_data, "city");
    cJSON *adcode = cJSON_GetObjectItem(live_data, "adcode");
    cJSON *weather = cJSON_GetObjectItem(live_data, "weather");
    cJSON *temperature = cJSON_GetObjectItem(live_data, "temperature");
    cJSON *winddirection = cJSON_GetObjectItem(live_data, "winddirection");
    cJSON *windpower = cJSON_GetObjectItem(live_data, "windpower");
    cJSON *humidity = cJSON_GetObjectItem(live_data, "humidity");
    cJSON *reporttime = cJSON_GetObjectItem(live_data, "reporttime");

    // 填充天气信息结构体
    if (province && cJSON_IsString(province)) {
        strncpy(weather_info->province, province->valuestring, sizeof(weather_info->province) - 1);
        weather_info->province[sizeof(weather_info->province) - 1] = '\0';
    }

    if (city && cJSON_IsString(city)) {
        strncpy(weather_info->city, city->valuestring, sizeof(weather_info->city) - 1);
        weather_info->city[sizeof(weather_info->city) - 1] = '\0';
    }

    if (adcode && cJSON_IsString(adcode)) {
        strncpy(weather_info->adcode, adcode->valuestring, sizeof(weather_info->adcode) - 1);
        weather_info->adcode[sizeof(weather_info->adcode) - 1] = '\0';
    }

    if (weather && cJSON_IsString(weather)) {
        strncpy(weather_info->weather, weather->valuestring, sizeof(weather_info->weather) - 1);
        weather_info->weather[sizeof(weather_info->weather) - 1] = '\0';
    }

    if (temperature && cJSON_IsString(temperature)) {
        strncpy(weather_info->temperature, temperature->valuestring, sizeof(weather_info->temperature) - 1);
        weather_info->temperature[sizeof(weather_info->temperature) - 1] = '\0';
    }

    if (winddirection && cJSON_IsString(winddirection)) {
        strncpy(weather_info->winddirection, winddirection->valuestring, sizeof(weather_info->winddirection) - 1);
        weather_info->winddirection[sizeof(weather_info->winddirection) - 1] = '\0';
    }

    if (windpower && cJSON_IsString(windpower)) {
        strncpy(weather_info->windpower, windpower->valuestring, sizeof(weather_info->windpower) - 1);
        weather_info->windpower[sizeof(weather_info->windpower) - 1] = '\0';
    }

    if (humidity && cJSON_IsString(humidity)) {
        strncpy(weather_info->humidity, humidity->valuestring, sizeof(weather_info->humidity) - 1);
        weather_info->humidity[sizeof(weather_info->humidity) - 1] = '\0';
    }

    if (reporttime && cJSON_IsString(reporttime)) {
        strncpy(weather_info->reporttime, reporttime->valuestring, sizeof(weather_info->reporttime) - 1);
        weather_info->reporttime[sizeof(weather_info->reporttime) - 1] = '\0';
    }

    cJSON_Delete(json);
    ESP_LOGI(TAG, "Weather data parsed successfully");
    ESP_LOGI(TAG, "城市: %s, 天气: %s, 温度: %s°C", weather_info->city, weather_info->weather, weather_info->temperature);

    return ESP_OK;
}

/* 初始化天气API客户端 */
esp_err_t weather_api_init(void)
{
    ESP_LOGI(TAG, "Initializing weather API client");
    return ESP_OK;
}

/* 获取天气信息 */
esp_err_t weather_api_get_weather(const char* city_code, weather_info_t* weather_info)
{
    if (weather_info == NULL) {
        ESP_LOGE(TAG, "Weather info pointer is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    // 检查WiFi连接状态
    wifi_status_t wifi_status = wifi_get_status();
    if (wifi_status != WIFI_STATUS_CONNECTED) {
        ESP_LOGW(TAG, "WiFi not connected, cannot get weather info");
        return ESP_ERR_WIFI_NOT_CONNECT;
    }

    // 构建请求URL
    char url[256];
    snprintf(url, sizeof(url), "http://restapi.amap.com/v3/weather/weatherInfo?city=%s&key=%s&extensions=all&output=json",
             city_code, WEATHER_API_KEY);

    ESP_LOGI(TAG, "Requesting weather info from: %s", url);

    // 重置响应缓冲区
    response_len = 0;
    memset(http_response_buffer, 0, sizeof(http_response_buffer));

    // 配置HTTP客户端
    esp_http_client_config_t config = {
        .url = url,
        .event_handler = http_event_handler,
        .timeout_ms = 10000,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG, "Failed to initialize HTTP client");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Starting HTTP request to API...");

    // 执行HTTP GET请求
    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        int status_code = esp_http_client_get_status_code(client);
        int content_length = esp_http_client_get_content_length(client);
        
        ESP_LOGI(TAG, "HTTP GET Status = %d, content_length = %d", status_code, content_length);
        
        if (status_code == 200 && response_len > 0) {
            ESP_LOGI(TAG, "HTTP response: %s", http_response_buffer);
            err = parse_weather_response(http_response_buffer, weather_info);
        } else {
            ESP_LOGE(TAG, "HTTP request failed with status %d", status_code);
            err = ESP_FAIL;
        }
    } else {
        ESP_LOGE(TAG, "HTTP request failed: %s", esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);
    return err;
}

/* 释放天气API客户端资源 */
void weather_api_deinit(void)
{
    ESP_LOGI(TAG, "Weather API client deinitialized");
} 