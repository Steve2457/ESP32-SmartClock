#ifndef AI_CHAT_H
#define AI_CHAT_H

#ifdef __cplusplus
extern "C" {
#endif

#include "esp_err.h"
#include <stdbool.h>
#include "esp_http_client.h"
#include "cJSON.h"
#include "secrets.h"

// GLM-4-Flash API配置
#define GLM_API_URL "https://open.bigmodel.cn/api/paas/v4/chat/completions"
#define GLM_MODEL_NAME "glm-4-flash"
#define GLM_MAX_RESPONSE_SIZE 4096
#define GLM_REQUEST_TIMEOUT_MS 30000

// API Key配置 - 现在从 secrets.h 文件中读取

/**
 * @brief AI对话响应结构体
 */
typedef struct {
    char *content;          // AI回复内容
    size_t content_len;     // 内容长度
    bool success;           // 是否成功
    char error_msg[256];    // 错误信息
} ai_chat_response_t;

/**
 * @brief 初始化AI对话模块
 * @return ESP_OK 成功，其他值表示失败
 */
esp_err_t ai_chat_init(void);

/**
 * @brief 发送用户消息给GLM-4-Flash并获取回复
 * @param user_message 用户输入的消息
 * @param response 输出参数，存储AI的回复
 * @return ESP_OK 成功，其他值表示失败
 */
esp_err_t ai_chat_send_message(const char *user_message, ai_chat_response_t *response);

/**
 * @brief 释放AI对话响应资源
 * @param response 要释放的响应结构体
 */
void ai_chat_free_response(ai_chat_response_t *response);

/**
 * @brief 清理AI对话模块
 */
void ai_chat_cleanup(void);

#ifdef __cplusplus
}
#endif

#endif // AI_CHAT_H 