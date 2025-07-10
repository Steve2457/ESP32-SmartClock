#include "ai_chat.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"
#include "wifi_manager.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "AI_CHAT";

// HTTP响应数据结构
typedef struct {
    char *data;
    size_t len;
    size_t capacity;
} http_response_data_t;

/**
 * @brief HTTP事件处理函数
 */
static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    http_response_data_t *response_data = (http_response_data_t *)evt->user_data;
    
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
            ESP_LOGD(TAG, "HTTP_EVENT_ON_HEADER: %s: %s", evt->header_key, evt->header_value);
            break;
        case HTTP_EVENT_ON_DATA:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_DATA: 接收到 %d 字节数据", evt->data_len);
            
            // 确保有足够的缓冲区空间
            if (response_data->len + evt->data_len >= response_data->capacity) {
                size_t new_capacity = response_data->capacity * 1.5;  // 使用更保守的增长因子
                if (new_capacity < response_data->len + evt->data_len + 1) {
                    new_capacity = response_data->len + evt->data_len + 512;  // 更小的缓冲区增量
                }
                
                ESP_LOGI(TAG, "扩展响应缓冲区: %d -> %d 字节", response_data->capacity, new_capacity);
                
                // 使用PSRAM分配内存
                char *new_data = heap_caps_realloc(response_data->data, new_capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                if (new_data == NULL) {
                    ESP_LOGE(TAG, "重新分配内存失败 (%d 字节), 当前可用HEAP: %d 字节", 
                            new_capacity, heap_caps_get_free_size(MALLOC_CAP_8BIT));
                    break;
                }
                response_data->data = new_data;
                response_data->capacity = new_capacity;
            }
            
            // 复制数据
            memcpy(response_data->data + response_data->len, evt->data, evt->data_len);
            response_data->len += evt->data_len;
            response_data->data[response_data->len] = '\0';  // 确保字符串结束
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

/**
 * @brief 构建GLM-4-Flash API请求JSON
 */
static char* build_request_json(const char *user_message)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *model = cJSON_CreateString(GLM_MODEL_NAME);
    cJSON *messages = cJSON_CreateArray();
    cJSON *message = cJSON_CreateObject();
    cJSON *role = cJSON_CreateString("user");
    
    // 构建带字数限制的消息内容
    char limited_message[1024];
    snprintf(limited_message, sizeof(limited_message), "%s 回答控制在60字以内", user_message);
    
    cJSON *content = cJSON_CreateString(limited_message);
    cJSON *max_tokens = cJSON_CreateNumber(1000);
    cJSON *temperature = cJSON_CreateNumber(0.7);
    
    // 构建消息对象
    cJSON_AddItemToObject(message, "role", role);
    cJSON_AddItemToObject(message, "content", content);
    cJSON_AddItemToArray(messages, message);
    
    // 构建根对象
    cJSON_AddItemToObject(root, "model", model);
    cJSON_AddItemToObject(root, "messages", messages);
    cJSON_AddItemToObject(root, "max_tokens", max_tokens);
    cJSON_AddItemToObject(root, "temperature", temperature);
    
    char *json_string = cJSON_Print(root);
    cJSON_Delete(root);
    
    return json_string;
}

/**
 * @brief 解析GLM-4-Flash API响应JSON
 */
static bool parse_response_json(const char *json_str, ai_chat_response_t *response)
{
    cJSON *root = cJSON_Parse(json_str);
    if (root == NULL) {
        snprintf(response->error_msg, sizeof(response->error_msg), "JSON解析失败");
        ESP_LOGE(TAG, "JSON解析失败: %s", json_str);
        return false;
    }
    
    // 检查是否有错误
    cJSON *error = cJSON_GetObjectItem(root, "error");
    if (error != NULL) {
        cJSON *error_message = cJSON_GetObjectItem(error, "message");
        cJSON *error_code = cJSON_GetObjectItem(error, "code");
        if (error_message && cJSON_IsString(error_message)) {
            if (error_code && cJSON_IsString(error_code)) {
                snprintf(response->error_msg, sizeof(response->error_msg), 
                        "API错误[%s]: %s", error_code->valuestring, error_message->valuestring);
            } else {
                snprintf(response->error_msg, sizeof(response->error_msg), 
                        "API错误: %s", error_message->valuestring);
            }
        } else {
            snprintf(response->error_msg, sizeof(response->error_msg), "未知API错误");
        }
        ESP_LOGE(TAG, "API返回错误: %s", response->error_msg);
        cJSON_Delete(root);
        return false;
    }
    
    // 解析正常响应
    cJSON *choices = cJSON_GetObjectItem(root, "choices");
    if (!choices || !cJSON_IsArray(choices) || cJSON_GetArraySize(choices) == 0) {
        snprintf(response->error_msg, sizeof(response->error_msg), "响应格式错误: 缺少choices");
        ESP_LOGE(TAG, "响应格式错误: 缺少choices");
        cJSON_Delete(root);
        return false;
    }
    
    cJSON *first_choice = cJSON_GetArrayItem(choices, 0);
    cJSON *message = cJSON_GetObjectItem(first_choice, "message");
    cJSON *content = cJSON_GetObjectItem(message, "content");
    
    if (!content || !cJSON_IsString(content)) {
        snprintf(response->error_msg, sizeof(response->error_msg), "响应格式错误: 缺少content");
        ESP_LOGE(TAG, "响应格式错误: 缺少content");
        cJSON_Delete(root);
        return false;
    }
    
    // 复制内容
    response->content_len = strlen(content->valuestring);
    response->content = malloc(response->content_len + 1);
    if (response->content == NULL) {
        snprintf(response->error_msg, sizeof(response->error_msg), "内存分配失败");
        ESP_LOGE(TAG, "内存分配失败");
        cJSON_Delete(root);
        return false;
    }
    
    strcpy(response->content, content->valuestring);
    
    ESP_LOGI(TAG, "AI回复: %s", response->content);
    
    cJSON_Delete(root);
    return true;
}

esp_err_t ai_chat_init(void)
{
    ESP_LOGI(TAG, "初始化AI对话模块");
    
    // 显示当前内存状态
    ESP_LOGI(TAG, "当前可用内存: %d 字节 (内部RAM), %d 字节 (PSRAM)",
             heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    
    // 检查API Key是否已配置 - 此检查已不再需要
    /*
    if (strcmp(GLM_API_KEY, "YOUR_API_KEY_HERE") == 0) {
        ESP_LOGE(TAG, "请在ai_chat.h中配置您的GLM-4-Flash API Key!");
        return ESP_FAIL;
    }
    */
    
    ESP_LOGI(TAG, "AI对话模块初始化完成");
    ESP_LOGI(TAG, "使用模型: %s", GLM_MODEL_NAME);
    ESP_LOGI(TAG, "API地址: %s", GLM_API_URL);
    
    return ESP_OK;
}

esp_err_t ai_chat_send_message(const char *user_message, ai_chat_response_t *response)
{
    if (user_message == NULL || response == NULL) {
        ESP_LOGE(TAG, "参数不能为空");
        return ESP_ERR_INVALID_ARG;
    }
    
    // 检查WiFi连接状态
    wifi_status_t wifi_status = wifi_get_status();
    if (wifi_status != WIFI_STATUS_CONNECTED) {
        snprintf(response->error_msg, sizeof(response->error_msg), 
                "WiFi未连接，当前状态: %d", wifi_status);
        ESP_LOGE(TAG, "WiFi未连接，当前状态: %d", wifi_status);
        response->success = false;
        return ESP_ERR_WIFI_NOT_CONNECT;
    }
    
    // 获取并打印IP地址
    char ip_str[16];
    if (wifi_get_ip_string(ip_str, sizeof(ip_str)) == ESP_OK) {
        ESP_LOGI(TAG, "当前IP地址: %s", ip_str);
    }
    
    // 打印内存状态
    ESP_LOGI(TAG, "发送请求前内存状态: 可用内存: %d 字节 (内部RAM), %d 字节 (PSRAM)",
             heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    
    // 初始化响应结构体
    memset(response, 0, sizeof(ai_chat_response_t));
    
    ESP_LOGI(TAG, "发送消息给GLM-4-Flash: %s", user_message);
    
    // 构建请求JSON
    char *request_json = build_request_json(user_message);
    if (request_json == NULL) {
        snprintf(response->error_msg, sizeof(response->error_msg), "构建请求JSON失败");
        ESP_LOGE(TAG, "构建请求JSON失败");
        return ESP_FAIL;
    }
    
    ESP_LOGD(TAG, "请求JSON: %s", request_json);
    
    // 初始化HTTP响应数据 - 使用PSRAM
    http_response_data_t http_response = {0};
    http_response.capacity = 1024;  // 使用较小的初始大小
    http_response.data = heap_caps_malloc(http_response.capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (http_response.data == NULL) {
        free(request_json);
        snprintf(response->error_msg, sizeof(response->error_msg), "内存分配失败");
        ESP_LOGE(TAG, "内存分配失败");
        return ESP_FAIL;
    }
    http_response.data[0] = '\0';
    
    // 配置HTTP客户端 - 使用优化的SSL配置
    esp_http_client_config_t config = {
        .url = GLM_API_URL,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 30000,
        .event_handler = http_event_handler,
        .user_data = &http_response,
        .transport_type = HTTP_TRANSPORT_OVER_SSL,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .skip_cert_common_name_check = false,
        .use_global_ca_store = false,
        .disable_auto_redirect = true,
        .is_async = false,
        .buffer_size = 2048,  // 减小缓冲区大小
        .buffer_size_tx = 2048,  // 减小发送缓冲区大小
    };
    
    // 分配额外内存以避免堆栈溢出
    ESP_LOGI(TAG, "初始化HTTP客户端前可用内存: %d 字节", heap_caps_get_free_size(MALLOC_CAP_8BIT));
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        free(request_json);
        free(http_response.data);
        snprintf(response->error_msg, sizeof(response->error_msg), "HTTP客户端初始化失败");
        ESP_LOGE(TAG, "HTTP客户端初始化失败");
        return ESP_FAIL;
    }
    
    // 设置请求头
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Accept", "application/json");
    esp_http_client_set_header(client, "User-Agent", "ESP32-GLM4-Client/1.0");
    
    // 设置Authorization头 - Bearer token格式
    char auth_header[512];
    snprintf(auth_header, sizeof(auth_header), "Bearer %s", GLM_API_KEY);
    esp_http_client_set_header(client, "Authorization", auth_header);
    
    // 设置请求体
    esp_http_client_set_post_field(client, request_json, strlen(request_json));
    
    // 准备发送请求前内存状态
    ESP_LOGI(TAG, "发送请求前可用内存: %d 字节", heap_caps_get_free_size(MALLOC_CAP_8BIT));
    
    // 发送请求
    esp_err_t err = esp_http_client_perform(client);
    int status_code = esp_http_client_get_status_code(client);
    
    ESP_LOGI(TAG, "HTTP状态码: %d", status_code);
    
    if (err == ESP_OK && status_code == 200) {
        ESP_LOGI(TAG, "API响应: %s", http_response.data);
        
        // 解析响应JSON
        if (parse_response_json(http_response.data, response)) {
            response->success = true;
            err = ESP_OK;
        } else {
            response->success = false;
            err = ESP_FAIL;
        }
    } else {
        snprintf(response->error_msg, sizeof(response->error_msg), 
                "HTTP请求失败: 状态码=%d, 错误=%s", status_code, esp_err_to_name(err));
        ESP_LOGE(TAG, "HTTP请求失败: 状态码=%d, 错误=%s", status_code, esp_err_to_name(err));
        if (http_response.len > 0) {
            ESP_LOGE(TAG, "响应内容: %s", http_response.data);
        }
        response->success = false;
        err = ESP_FAIL;
    }
    
    // 清理资源
    esp_http_client_cleanup(client);
    free(request_json);
    free(http_response.data);
    
    return err;
}

void ai_chat_free_response(ai_chat_response_t *response)
{
    if (response && response->content) {
        free(response->content);
        response->content = NULL;
        response->content_len = 0;
    }
}

void ai_chat_cleanup(void)
{
    ESP_LOGI(TAG, "清理AI对话模块");
} 