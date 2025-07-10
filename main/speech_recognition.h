#ifndef SPEECH_RECOGNITION_H
#define SPEECH_RECOGNITION_H

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>
#include "secrets.h"

#ifdef __cplusplus
extern "C" {
#endif

// 腾讯云API配置 - 请在这里填入您的密钥
#define TENCENT_APPID           1328329606     // 请替换为您的AppId
#define TENCENT_PROJECT_ID      0              // 项目ID，可以不填

// SecretId 和 SecretKey 已移至 secrets.h

// API配置
#define TENCENT_ASR_HOST        "asr.tencentcloudapi.com"
#define TENCENT_ASR_ACTION      "SentenceRecognition"
#define TENCENT_ASR_VERSION     "2019-06-14"
#define TENCENT_ASR_REGION      ""  // 留空使用就近地域

// 音频配置 - 启用PSRAM后恢复正常配置
#define SPEECH_SAMPLE_RATE      16000
#define SPEECH_BITS_PER_SAMPLE  16
#define SPEECH_CHANNELS         1
#define SPEECH_RECORD_TIME_MS   5000    // 恢复5秒录音时长
#define SPEECH_BUFFER_SIZE      (SPEECH_SAMPLE_RATE * SPEECH_BITS_PER_SAMPLE / 8 * SPEECH_CHANNELS * SPEECH_RECORD_TIME_MS / 1000)

// WAV文件头大小
#define WAV_HEADER_SIZE         44

// Base64编码后的大小估算 (WAV数据大小 * 4/3 + 填充 + 余量)
#define SPEECH_BASE64_SIZE      (((SPEECH_BUFFER_SIZE + WAV_HEADER_SIZE) * 4 + 2) / 3 + 100)

// HTTP响应缓冲区大小
#define HTTP_RESPONSE_BUFFER_SIZE   4096

// 语音识别状态
typedef enum {
    SPEECH_STATE_IDLE,
    SPEECH_STATE_RECORDING,
    SPEECH_STATE_PROCESSING,
    SPEECH_STATE_COMPLETED,
    SPEECH_STATE_ERROR
} speech_recognition_state_t;

// 语音识别结果结构
typedef struct {
    char result_text[512];          // 识别结果文本
    char ai_reply[1024];            // AI回复内容
    bool has_ai_reply;              // 是否有AI回复
    int audio_duration;             // 音频时长(毫秒)
    speech_recognition_state_t state;
    char error_message[256];        // 错误信息
    bool valid;                     // 结果是否有效
} speech_recognition_result_t;

// 函数声明
esp_err_t speech_recognition_init(void);
void speech_recognition_start(void);
esp_err_t speech_recognition_stop(void);
speech_recognition_result_t* speech_recognition_get_result(void);
void speech_recognition_deinit(void);
bool speech_recognition_is_active(void);
bool is_speech_recognition_running(void);

#ifdef __cplusplus
}
#endif

#endif // SPEECH_RECOGNITION_H 