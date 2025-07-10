#include "speech_recognition.h"
#include "ai_chat.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_sntp.h"
#include "cJSON.h"
#include "mbedtls/base64.h"
#include "mbedtls/md.h"
#include "mbedtls/sha256.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/i2s_std.h"
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <inttypes.h>

// I2S pin definitions for speech recognition
#define I2S_SCK_IO          39  // Serial Clock
#define I2S_WS_IO           21  // Word Select (L/R Channel)
#define I2S_SD_IO           40  // Serial Data

// I2S configuration - 修复INMP441配置，优化参数
#define I2S_NUM             I2S_NUM_0
#define I2S_SAMPLE_RATE     16000
#define I2S_SAMPLE_BITS     32      // INMP441输出32位数据，但有效位为24位
#define I2S_CHANNEL_NUM     1
#define I2S_DMA_BUF_COUNT   6       // 减少DMA缓冲区数量，提高稳定性
#define I2S_DMA_BUF_LEN     512     // 减少DMA缓冲区长度，降低延迟

static const char *TAG = "SPEECH_REC";

// 全局变量
static speech_recognition_result_t g_speech_result = {
    .result_text = "",
    .ai_reply = "",
    .has_ai_reply = false,
    .audio_duration = 0,
    .state = SPEECH_STATE_IDLE,
    .error_message = "",
    .valid = false
};
static bool g_speech_active = false;
static TaskHandle_t g_speech_task_handle = NULL;
static SemaphoreHandle_t g_speech_mutex = NULL;
static bool g_time_synced = false;

// 音频缓冲区 - 使用动态分配以节省内存
static int32_t *g_raw_audio_buffer = NULL;  // 32位原始I2S数据缓冲区
static int16_t *g_audio_buffer = NULL;      // 16位PCM数据缓冲区
static char *g_base64_buffer = NULL;
static char *g_http_response_buffer = NULL;

// I2S通道句柄
static i2s_chan_handle_t g_rx_handle = NULL;
static bool g_i2s_initialized = false;

// SNTP时间同步回调
static void time_sync_notification_cb(struct timeval *tv)
{
    ESP_LOGI(TAG, "Time synchronized via SNTP");
    g_time_synced = true;
}

// 初始化并同步时间
static esp_err_t sync_time_with_ntp(void)
{
    if (g_time_synced) {
        return ESP_OK;  // 已经同步过了
    }
    
    ESP_LOGI(TAG, "Initializing SNTP for time synchronization...");
    
    // 设置时区为UTC
    setenv("TZ", "UTC", 1);
    tzset();
    
    // 初始化SNTP
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_setservername(1, "time.nist.gov");
    esp_sntp_set_time_sync_notification_cb(time_sync_notification_cb);
    esp_sntp_init();
    
    // 等待时间同步，最多等待10秒
    int retry = 0;
    const int retry_count = 100;  // 10秒，每100ms检查一次
    
    while (!g_time_synced && ++retry < retry_count) {
        ESP_LOGI(TAG, "Waiting for time sync... (%d/%d)", retry, retry_count);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    
    if (g_time_synced) {
        time_t now;
        time(&now);
        ESP_LOGI(TAG, "Time synchronized successfully. Current time: %lld", (long long)now);
        return ESP_OK;
    } else {
        ESP_LOGW(TAG, "Time sync timeout, using fallback time");
        return ESP_ERR_TIMEOUT;
    }
}

// Base64编码函数
// WAV文件头结构
typedef struct {
    char riff[4];           // "RIFF"
    uint32_t file_size;     // 文件大小 - 8
    char wave[4];           // "WAVE"
    char fmt[4];            // "fmt "
    uint32_t fmt_size;      // fmt块大小 (16)
    uint16_t audio_format;  // 音频格式 (1 = PCM)
    uint16_t channels;      // 声道数
    uint32_t sample_rate;   // 采样率
    uint32_t byte_rate;     // 字节率
    uint16_t block_align;   // 块对齐
    uint16_t bits_per_sample; // 位深度
    char data[4];           // "data"
    uint32_t data_size;     // 数据大小
} __attribute__((packed)) wav_header_t;

static esp_err_t create_wav_data(const int16_t *pcm_data, size_t pcm_len, uint8_t **wav_data, size_t *wav_len)
{
    size_t samples = pcm_len / sizeof(int16_t);
    size_t data_size = samples * sizeof(int16_t);
    size_t total_size = sizeof(wav_header_t) + data_size;
    
    ESP_LOGI(TAG, "Creating WAV file: %zu samples, %zu PCM bytes, %zu total bytes", 
             samples, data_size, total_size);
    
    *wav_data = malloc(total_size);
    if (!*wav_data) {
        ESP_LOGE(TAG, "Failed to allocate WAV buffer (%zu bytes)", total_size);
        return ESP_FAIL;
    }
    
    wav_header_t *header = (wav_header_t*)*wav_data;
    
    // 填充WAV头部 - 确保格式正确
    memcpy(header->riff, "RIFF", 4);
    header->file_size = total_size - 8;                    // 文件大小减去RIFF头8字节
    memcpy(header->wave, "WAVE", 4);
    memcpy(header->fmt, "fmt ", 4);
    header->fmt_size = 16;                                 // PCM格式的fmt块大小固定为16
    header->audio_format = 1;                              // PCM格式
    header->channels = 1;                                  // 单声道
    header->sample_rate = I2S_SAMPLE_RATE;                 // 16000 Hz
    header->byte_rate = I2S_SAMPLE_RATE * 1 * 16 / 8;     // 32000 bytes/sec
    header->block_align = 1 * 16 / 8;                      // 2 bytes per sample
    header->bits_per_sample = 16;                          // 16位深度
    memcpy(header->data, "data", 4);
    header->data_size = data_size;                         // PCM数据大小
    
    // 复制PCM数据到WAV文件
    memcpy(*wav_data + sizeof(wav_header_t), pcm_data, data_size);
    
    *wav_len = total_size;
    
    ESP_LOGI(TAG, "WAV file created successfully");
    ESP_LOGI(TAG, "WAV Header - Format: PCM, Channels: %d, Sample Rate: %"PRIu32" Hz, Bit Depth: %d", 
             header->channels, header->sample_rate, header->bits_per_sample);
    ESP_LOGI(TAG, "WAV Data - Samples: %zu, Data Size: %"PRIu32" bytes, Total Size: %"PRIu32" bytes", 
             samples, header->data_size, header->file_size + 8);
    
    return ESP_OK;
}

static esp_err_t encode_base64(const uint8_t *input, size_t input_len, char *output, size_t output_size, size_t *output_len)
{
    size_t required_len;
    int ret = mbedtls_base64_encode(NULL, 0, &required_len, input, input_len);
    if (ret != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL) {
        return ESP_FAIL;
    }
    
    if (required_len > output_size) {
        ESP_LOGE(TAG, "Base64 output buffer too small: need %zu, have %zu", required_len, output_size);
        return ESP_FAIL;
    }
    
    ret = mbedtls_base64_encode((unsigned char*)output, output_size, output_len, input, input_len);
    if (ret != 0) {
        ESP_LOGE(TAG, "Base64 encoding failed: %d", ret);
        return ESP_FAIL;
    }
    
    return ESP_OK;
}

// I2S初始化函数 - 针对INMP441优化
static esp_err_t i2s_mic_init(void)
{
    if (g_i2s_initialized) {
        ESP_LOGW(TAG, "I2S microphone already initialized");
        return ESP_OK;
    }
    
    ESP_LOGI(TAG, "Initializing I2S microphone for INMP441...");
    
    // Create I2S channel with optimized parameters
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    chan_cfg.dma_desc_num = I2S_DMA_BUF_COUNT;
    chan_cfg.dma_frame_num = I2S_DMA_BUF_LEN;
    
    esp_err_t ret = i2s_new_channel(&chan_cfg, NULL, &g_rx_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create I2S channel: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // Configure I2S standard mode for INMP441 - 优化配置
    i2s_std_config_t std_cfg = {
        .clk_cfg = {
            .sample_rate_hz = I2S_SAMPLE_RATE,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,     // 256倍频，提供稳定时钟
        },
        .slot_cfg = {
            .data_bit_width = I2S_DATA_BIT_WIDTH_32BIT,  // INMP441输出32位
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_32BIT,  // 明确设置为32位
            .slot_mode = I2S_SLOT_MODE_MONO,             // 单声道
            .slot_mask = I2S_STD_SLOT_LEFT,              // L/R接GND时使用左声道
            .ws_width = 32,                              // WS信号宽度32位
            .ws_pol = false,                             // WS极性：低电平=左声道
            .bit_shift = true,                           // 启用位移（Philips标准）
            .left_align = true,                          // 左对齐
            .big_endian = false,                         // 小端序
            .bit_order_lsb = false,                      // MSB优先
        },
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_SCK_IO,                          // GPIO39 -> SCK
            .ws = I2S_WS_IO,                             // GPIO21 -> WS
            .dout = I2S_GPIO_UNUSED,
            .din = I2S_SD_IO,                            // GPIO40 -> SD
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    
    ret = i2s_channel_init_std_mode(g_rx_handle, &std_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize I2S standard mode: %s", esp_err_to_name(ret));
        i2s_del_channel(g_rx_handle);
        g_rx_handle = NULL;
        return ret;
    }
    
    g_i2s_initialized = true;
    ESP_LOGI(TAG, "I2S microphone initialized successfully for INMP441");
    ESP_LOGI(TAG, "I2S Config - SCK: GPIO%d, WS: GPIO%d, SD: GPIO%d", 
             I2S_SCK_IO, I2S_WS_IO, I2S_SD_IO);
    ESP_LOGI(TAG, "CRITICAL: Ensure INMP441 L/R pin is connected to GND for left channel");
    ESP_LOGI(TAG, "Sample Rate: %d Hz, Bit Width: 32-bit, DMA Buffers: %d x %d", 
             I2S_SAMPLE_RATE, I2S_DMA_BUF_COUNT, I2S_DMA_BUF_LEN);
    
    return ESP_OK;
}

// 音频数据质量检查函数
static bool check_audio_data_quality(int16_t *pcm_data, size_t sample_count)
{
    if (!pcm_data || sample_count == 0) {
        ESP_LOGE(TAG, "Invalid audio data parameters");
        return false;
    }
    
    // 统计音频数据特征
    int64_t sum = 0;
    int32_t max_val = INT16_MIN;
    int32_t min_val = INT16_MAX;
    uint32_t zero_count = 0;
    uint32_t non_zero_count = 0;
    
    for (size_t i = 0; i < sample_count; i++) {
        int16_t sample = pcm_data[i];
        sum += sample;
        
        if (sample > max_val) max_val = sample;
        if (sample < min_val) min_val = sample;
        
        if (sample == 0) {
            zero_count++;
        } else {
            non_zero_count++;
        }
    }
    
    int32_t avg = (int32_t)(sum / sample_count);
    int32_t range = max_val - min_val;
    float zero_ratio = (float)zero_count / sample_count;
    
    ESP_LOGI(TAG, "Audio Quality Check:");
    ESP_LOGI(TAG, "  Samples: %zu", sample_count);
    ESP_LOGI(TAG, "  Average: %"PRIi32"", avg);
    ESP_LOGI(TAG, "  Range: %"PRIi32" (Max: %"PRIi32", Min: %"PRIi32")", range, max_val, min_val);
    ESP_LOGI(TAG, "  Zero samples: %"PRIu32" (%.1f%%)", zero_count, zero_ratio * 100.0f);
    ESP_LOGI(TAG, "  Non-zero samples: %"PRIu32" (%.1f%%)", non_zero_count, (1.0f - zero_ratio) * 100.0f);
    
    // 数据质量判断标准
    bool quality_ok = true;
    
    if (zero_ratio > 0.95f) {
        ESP_LOGW(TAG, "WARNING: Too many zero samples (%.1f%%), possible microphone issue", zero_ratio * 100.0f);
        quality_ok = false;
    }
    
    if (range < 100) {
        ESP_LOGW(TAG, "WARNING: Very low signal range (%"PRIi32"), possible no audio input", range);
    } else if (range > 30000) {
        ESP_LOGI(TAG, "Good signal range detected (%"PRIi32")", range);
    }
    
    if (abs(avg) > 1000) {
        ESP_LOGW(TAG, "WARNING: High DC offset detected (%"PRIi32")", avg);
    }
    
    return quality_ok;
}

// 将32位I2S数据转换为16位PCM数据 (INMP441专用) - 改进转换算法
static void convert_32bit_to_16bit(int32_t *src, int16_t *dest, size_t sample_count)
{
    ESP_LOGI(TAG, "Converting %zu samples from 32-bit to 16-bit (INMP441 optimized)", sample_count);
    
    // 首先检查原始32位数据的特征
    int64_t sum_32 = 0;
    int32_t max_32 = INT32_MIN;
    int32_t min_32 = INT32_MAX;
    
    for (size_t i = 0; i < sample_count; i++) {
        int32_t sample = src[i];
        sum_32 += sample;
        if (sample > max_32) max_32 = sample;
        if (sample < min_32) min_32 = sample;
    }
    
    ESP_LOGI(TAG, "32-bit source data - Range: %"PRIi32", Avg: %lld", max_32 - min_32, sum_32 / sample_count);
    
    for (size_t i = 0; i < sample_count; i++) {
        // INMP441输出32位数据，有效音频数据在高24位
        int32_t sample_32 = src[i];
        
        // 方法：取高16位，这样可以保留最大的动态范围
        // INMP441的数据格式：高24位有效，低8位为0
        int16_t sample_16 = (int16_t)(sample_32 >> 16);
        
        // 可选：应用简单的增益来提高信号强度（如果需要）
        // sample_16 = (int16_t)((int32_t)sample_16 * 2);  // 2倍增益
        
        dest[i] = sample_16;
    }
    
    ESP_LOGI(TAG, "32-bit to 16-bit conversion completed");
}

// I2S清理函数
static void i2s_mic_deinit(void)
{
    if (g_i2s_initialized && g_rx_handle) {
        // Disable channel if enabled
        i2s_channel_disable(g_rx_handle);
        // Delete channel
        i2s_del_channel(g_rx_handle);
        g_rx_handle = NULL;
        g_i2s_initialized = false;
        ESP_LOGI(TAG, "I2S microphone deinitialized");
    }
}

// 生成HMAC-SHA256签名（二进制输出）
static esp_err_t hmac_sha256_binary(const unsigned char *key, size_t key_len, const char *data, unsigned char *output)
{
    mbedtls_md_context_t ctx;
    const mbedtls_md_info_t *md_info;
    
    mbedtls_md_init(&ctx);
    md_info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    
    if (mbedtls_md_setup(&ctx, md_info, 1) != 0) {
        mbedtls_md_free(&ctx);
        return ESP_FAIL;
    }
    
    if (mbedtls_md_hmac_starts(&ctx, key, key_len) != 0 ||
        mbedtls_md_hmac_update(&ctx, (const unsigned char*)data, strlen(data)) != 0 ||
        mbedtls_md_hmac_finish(&ctx, output) != 0) {
        mbedtls_md_free(&ctx);
        return ESP_FAIL;
    }
    
    mbedtls_md_free(&ctx);
    return ESP_OK;
}

// 生成HMAC-SHA256签名（十六进制字符串输出）
static esp_err_t hmac_sha256(const char *key, const char *data, char *output, size_t output_size)
{
    unsigned char hash[32];
    
    if (hmac_sha256_binary((const unsigned char*)key, strlen(key), data, hash) != ESP_OK) {
        return ESP_FAIL;
    }
    
    // 转换为十六进制字符串
    for (int i = 0; i < 32; i++) {
        snprintf(output + i * 2, output_size - i * 2, "%02x", hash[i]);
    }
    
    return ESP_OK;
}

// 生成腾讯云TC3签名
static esp_err_t generate_tc3_signature(const char *payload, const char *timestamp, const char *date, char *signature, size_t sig_size)
{
    ESP_LOGI(TAG, "Generating TC3 signature...");
    ESP_LOGI(TAG, "Timestamp: %s", timestamp);
    ESP_LOGI(TAG, "Date: %s", date);
    ESP_LOGI(TAG, "Payload length: %d", strlen(payload));
    
    // 1. 计算payload的SHA256哈希
    unsigned char payload_hash[32];
    mbedtls_sha256_context sha256_ctx;
    mbedtls_sha256_init(&sha256_ctx);
    mbedtls_sha256_starts(&sha256_ctx, 0);
    mbedtls_sha256_update(&sha256_ctx, (const unsigned char*)payload, strlen(payload));
    mbedtls_sha256_finish(&sha256_ctx, payload_hash);
    mbedtls_sha256_free(&sha256_ctx);
    
    // 转换为十六进制字符串
    char payload_hash_hex[65] = {0};
    for (int i = 0; i < 32; i++) {
        snprintf(payload_hash_hex + i * 2, sizeof(payload_hash_hex) - i * 2, "%02x", payload_hash[i]);
    }
    ESP_LOGI(TAG, "Payload hash: %s", payload_hash_hex);
    
    // 2. 构建规范请求字符串 - 按照腾讯云TC3标准格式
    char canonical_request[2048];
    snprintf(canonical_request, sizeof(canonical_request),
             "POST\n"
             "/\n"
             "\n"
             "content-type:application/json; charset=utf-8\n"
             "host:%s\n"
             "\n"
             "content-type;host\n"
             "%s",
             TENCENT_ASR_HOST, payload_hash_hex);
    
    ESP_LOGI(TAG, "Canonical request:\n%s", canonical_request);
    
    // 3. 计算规范请求字符串的SHA256哈希
    unsigned char canonical_hash[32];
    mbedtls_sha256_init(&sha256_ctx);
    mbedtls_sha256_starts(&sha256_ctx, 0);
    mbedtls_sha256_update(&sha256_ctx, (const unsigned char*)canonical_request, strlen(canonical_request));
    mbedtls_sha256_finish(&sha256_ctx, canonical_hash);
    mbedtls_sha256_free(&sha256_ctx);
    
    // 转换为十六进制字符串
    char canonical_hash_hex[65] = {0};
    for (int i = 0; i < 32; i++) {
        snprintf(canonical_hash_hex + i * 2, sizeof(canonical_hash_hex) - i * 2, "%02x", canonical_hash[i]);
    }
    ESP_LOGI(TAG, "Canonical request hash: %s", canonical_hash_hex);
    
    // 4. 构建待签名字符串
    char string_to_sign[2048];
    snprintf(string_to_sign, sizeof(string_to_sign),
             "TC3-HMAC-SHA256\n"
             "%s\n"
             "%s/asr/tc3_request\n"
             "%s",
             timestamp, date, canonical_hash_hex);
    
    ESP_LOGI(TAG, "String to sign:\n%s", string_to_sign);
    
    // 5. 计算签名密钥
    char tc3_secret_key[256];
    snprintf(tc3_secret_key, sizeof(tc3_secret_key), "TC3%s", TENCENT_SECRET_KEY);
    
    unsigned char date_key[32];
    unsigned char service_key[32];
    unsigned char signing_key[32];
    
    if (hmac_sha256_binary((const unsigned char*)tc3_secret_key, strlen(tc3_secret_key), date, date_key) != ESP_OK ||
        hmac_sha256_binary(date_key, 32, "asr", service_key) != ESP_OK ||
        hmac_sha256_binary(service_key, 32, "tc3_request", signing_key) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to generate signing keys");
        return ESP_FAIL;
    }
    
    // 6. 计算最终签名
    unsigned char final_signature[32];
    if (hmac_sha256_binary(signing_key, 32, string_to_sign, final_signature) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to generate final signature");
        return ESP_FAIL;
    }
    
    // 转换为十六进制字符串
    for (int i = 0; i < 32; i++) {
        snprintf(signature + i * 2, sig_size - i * 2, "%02x", final_signature[i]);
    }
    
    ESP_LOGI(TAG, "Generated signature: %s", signature);
    return ESP_OK;
}

// HTTP事件处理器
static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    switch (evt->event_id) {
        case HTTP_EVENT_ON_DATA:
            if (evt->data_len > 0 && g_http_response_buffer) {
                size_t current_len = strlen(g_http_response_buffer);
                size_t remaining = HTTP_RESPONSE_BUFFER_SIZE - current_len - 1;
                size_t copy_len = (evt->data_len < remaining) ? evt->data_len : remaining;
                
                if (copy_len > 0) {
                    strncat(g_http_response_buffer, (char*)evt->data, copy_len);
                }
            }
            break;
        default:
            break;
    }
    return ESP_OK;
}

// 解析API响应
static esp_err_t parse_api_response(const char *response)
{
    cJSON *json = cJSON_Parse(response);
    if (!json) {
        ESP_LOGE(TAG, "Failed to parse JSON response");
        return ESP_FAIL;
    }
    
    cJSON *response_obj = cJSON_GetObjectItem(json, "Response");
    if (!response_obj) {
        ESP_LOGE(TAG, "No Response object in JSON");
        cJSON_Delete(json);
        return ESP_FAIL;
    }
    
    // 检查是否有错误
    cJSON *error_obj = cJSON_GetObjectItem(response_obj, "Error");
    if (error_obj) {
        cJSON *code = cJSON_GetObjectItem(error_obj, "Code");
        cJSON *message = cJSON_GetObjectItem(error_obj, "Message");
        
        snprintf(g_speech_result.error_message, sizeof(g_speech_result.error_message),
                "API Error: %s - %s",
                code ? code->valuestring : "Unknown",
                message ? message->valuestring : "Unknown error");
        
        g_speech_result.state = SPEECH_STATE_ERROR;
        cJSON_Delete(json);
        return ESP_FAIL;
    }
    
    // 获取识别结果
    cJSON *result = cJSON_GetObjectItem(response_obj, "Result");
    if (result && result->valuestring) {
        strncpy(g_speech_result.result_text, result->valuestring, sizeof(g_speech_result.result_text) - 1);
        g_speech_result.result_text[sizeof(g_speech_result.result_text) - 1] = '\0';
    } else {
        // 结果为空
        g_speech_result.result_text[0] = '\0';
    }
    
    // 获取音频时长
    cJSON *duration = cJSON_GetObjectItem(response_obj, "AudioDuration");
    if (duration) {
        g_speech_result.audio_duration = duration->valueint;
    }
    
    // 检查是否有有效的识别结果
    if (strlen(g_speech_result.result_text) > 0) {
        g_speech_result.state = SPEECH_STATE_COMPLETED;
        g_speech_result.valid = true;
        ESP_LOGI(TAG, "Speech recognition successful: '%s'", g_speech_result.result_text);
        
        // 调用AI对话功能
        ESP_LOGI(TAG, "Sending to GLM-4-Flash AI: '%s'", g_speech_result.result_text);
        
        ai_chat_response_t ai_response;
        esp_err_t ai_ret = ai_chat_send_message(g_speech_result.result_text, &ai_response);
        
        if (ai_ret == ESP_OK && ai_response.success) {
            ESP_LOGI(TAG, "=== AI回复开始 ===");
            ESP_LOGI(TAG, "%s", ai_response.content);
            ESP_LOGI(TAG, "=== AI回复结束 ===");
            
            // 保存AI回复到结果结构体中
            strncpy(g_speech_result.ai_reply, ai_response.content, sizeof(g_speech_result.ai_reply) - 1);
            g_speech_result.ai_reply[sizeof(g_speech_result.ai_reply) - 1] = '\0';
            
            // 处理标点符号 - 使用更简单的方法替换常见中文标点符号
            char temp_buffer[sizeof(g_speech_result.ai_reply)];
            char *dest = temp_buffer;
            const char *src = g_speech_result.ai_reply;
            
            while (*src) {
                // 检查UTF-8的多字节字符
                if ((unsigned char)*src > 0x7F) {  // 非ASCII字符
                    // 处理常见中文标点
                    // 中文逗号 (，)
                    if ((unsigned char)src[0] == 0xEF && (unsigned char)src[1] == 0xBC && (unsigned char)src[2] == 0x8C) {
                        *dest++ = ',';
                        src += 3;
                    } 
                    // 中文句号 (。)
                    else if ((unsigned char)src[0] == 0xE3 && (unsigned char)src[1] == 0x80 && (unsigned char)src[2] == 0x82) {
                        *dest++ = '.';
                        src += 3;
                    }
                    // 中文顿号 (、)
                    else if ((unsigned char)src[0] == 0xE3 && (unsigned char)src[1] == 0x80 && (unsigned char)src[2] == 0x81) {
                        *dest++ = ',';
                        src += 3;
                    }
                    // 中文冒号 (：)
                    else if ((unsigned char)src[0] == 0xEF && (unsigned char)src[1] == 0xBC && (unsigned char)src[2] == 0x9A) {
                        *dest++ = ':';
                        src += 3;
                    }
                    // 中文分号 (；)
                    else if ((unsigned char)src[0] == 0xEF && (unsigned char)src[1] == 0xBC && (unsigned char)src[2] == 0x9B) {
                        *dest++ = ';';
                        src += 3;
                    }
                    // 中文感叹号 (！)
                    else if ((unsigned char)src[0] == 0xEF && (unsigned char)src[1] == 0xBC && (unsigned char)src[2] == 0x81) {
                        *dest++ = '!';
                        src += 3;
                    }
                    // 中文问号 (？)
                    else if ((unsigned char)src[0] == 0xEF && (unsigned char)src[1] == 0xBC && (unsigned char)src[2] == 0x9F) {
                        *dest++ = '?';
                        src += 3;
                    }
                    // 左括号 (（)
                    else if ((unsigned char)src[0] == 0xEF && (unsigned char)src[1] == 0xBC && (unsigned char)src[2] == 0x88) {
                        *dest++ = '(';
                        src += 3;
                    }
                    // 右括号 (）)
                    else if ((unsigned char)src[0] == 0xEF && (unsigned char)src[1] == 0xBC && (unsigned char)src[2] == 0x89) {
                        *dest++ = ')';
                        src += 3;
                    }
                    // 左方括号 ([)
                    else if ((unsigned char)src[0] == 0xEF && (unsigned char)src[1] == 0xBC && (unsigned char)src[2] == 0xBB) {
                        *dest++ = '[';
                        src += 3;
                    }
                    // 右方括号 (])
                    else if ((unsigned char)src[0] == 0xEF && (unsigned char)src[1] == 0xBC && (unsigned char)src[2] == 0xBD) {
                        *dest++ = ']';
                        src += 3;
                    }
                    // 引号 (") - 替换为英文双引号
                    else if ((unsigned char)src[0] == 0xE2 && (unsigned char)src[1] == 0x80 && (unsigned char)src[2] == 0x9C) {
                        *dest++ = '"';
                        src += 3;
                    }
                    // 引号 (") - 替换为英文双引号
                    else if ((unsigned char)src[0] == 0xE2 && (unsigned char)src[1] == 0x80 && (unsigned char)src[2] == 0x9D) {
                        *dest++ = '"';
                        src += 3;
                    } else {
                        // 复制不处理的UTF-8字符
                        if ((unsigned char)*src >= 0xF0) {  // 4字节UTF-8
                            *dest++ = *src++;
                            if (*src) *dest++ = *src++;
                            if (*src) *dest++ = *src++;
                            if (*src) *dest++ = *src++;
                        } else if ((unsigned char)*src >= 0xE0) {  // 3字节UTF-8
                            *dest++ = *src++;
                            if (*src) *dest++ = *src++;
                            if (*src) *dest++ = *src++;
                        } else if ((unsigned char)*src >= 0xC0) {  // 2字节UTF-8
                            *dest++ = *src++;
                            if (*src) *dest++ = *src++;
                        }
                    }
                } else {
                    // 复制ASCII字符
                    *dest++ = *src++;
                }
            }
            
            *dest = '\0';  // 确保字符串结束
            
            // 将处理后的字符串复制回原缓冲区
            strcpy(g_speech_result.ai_reply, temp_buffer);
            
            g_speech_result.has_ai_reply = true;
            
            // 释放AI响应内存
            ai_chat_free_response(&ai_response);
        } else {
            ESP_LOGE(TAG, "AI对话失败: %s", ai_response.error_msg);
            g_speech_result.has_ai_reply = false;
            snprintf(g_speech_result.ai_reply, sizeof(g_speech_result.ai_reply), "AI对话失败: %s", ai_response.error_msg);
        }
    } else {
        g_speech_result.state = SPEECH_STATE_COMPLETED;
        g_speech_result.valid = false;
        g_speech_result.has_ai_reply = false;
        g_speech_result.ai_reply[0] = '\0';
        strcpy(g_speech_result.error_message, "No speech detected");
        ESP_LOGW(TAG, "No speech detected in audio");
    }
    
    cJSON_Delete(json);
    return ESP_OK;
}

// 发送API请求
static esp_err_t send_api_request(const char *audio_data_base64, size_t data_len)
{
    esp_err_t ret = ESP_FAIL;
    
    // 清空响应缓冲区
    if (g_http_response_buffer) {
        memset(g_http_response_buffer, 0, HTTP_RESPONSE_BUFFER_SIZE);
    }
    
    // 构建请求体 - 按照腾讯云语音识别API的正确格式
    cJSON *json = cJSON_CreateObject();
    cJSON *eng_service_type = cJSON_CreateString("16k_zh");
    cJSON *source_type = cJSON_CreateNumber(1);
    cJSON *voice_format = cJSON_CreateString("wav");
    cJSON *usr_audio_key = cJSON_CreateString("esp32-speech-recognition");
    cJSON *data = cJSON_CreateString(audio_data_base64);
    cJSON *data_len_json = cJSON_CreateNumber(data_len);
    
    cJSON_AddItemToObject(json, "EngSerViceType", eng_service_type);
    cJSON_AddItemToObject(json, "SourceType", source_type);
    cJSON_AddItemToObject(json, "VoiceFormat", voice_format);
    cJSON_AddItemToObject(json, "UsrAudioKey", usr_audio_key);
    cJSON_AddItemToObject(json, "Data", data);
    cJSON_AddItemToObject(json, "DataLen", data_len_json);
    
    char *json_string = cJSON_Print(json);
    if (!json_string) {
        ESP_LOGE(TAG, "Failed to create JSON string");
        cJSON_Delete(json);
        return ESP_FAIL;
    }
    
    // 配置HTTP客户端
    esp_http_client_config_t config = {
        .host = TENCENT_ASR_HOST,
        .path = "/",
        .port = 443,
        .transport_type = HTTP_TRANSPORT_OVER_SSL,
        .event_handler = http_event_handler,
        .timeout_ms = 30000,
        .skip_cert_common_name_check = true,    // 跳过证书通用名检查
        .crt_bundle_attach = esp_crt_bundle_attach,  // 使用证书包而不是全局CA存储
    };
    
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "Failed to initialize HTTP client");
        free(json_string);
        cJSON_Delete(json);
        return ESP_FAIL;
    }
    
    // 设置请求头
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json; charset=utf-8");
    esp_http_client_set_header(client, "Host", TENCENT_ASR_HOST);
    
    // 尝试同步时间
    esp_err_t sync_ret = sync_time_with_ntp();
    
    // 获取当前时间戳
    time_t now;
    time(&now);
    
    // 如果时间同步失败或时间看起来不合理，使用合理的时间戳
    if (sync_ret != ESP_OK || now < 1577836800) {  // 2020-01-01 00:00:00 UTC
        // 使用一个基于当前日期的合理时间戳（2025年6月15日 15:30:00 UTC）
        now = 1749972600;  // 稍微调整时间，避免过期
        ESP_LOGW(TAG, "Using fallback time: %lld", (long long)now);
    } else {
        ESP_LOGI(TAG, "Using synchronized time: %lld", (long long)now);
    }
    
    char timestamp_str[32];
    snprintf(timestamp_str, sizeof(timestamp_str), "%lld", (long long)now);
    
    // 生成当前日期字符串
    struct tm *timeinfo = gmtime(&now);
    char date_str[16];
    strftime(date_str, sizeof(date_str), "%Y-%m-%d", timeinfo);
    
    // 设置腾讯云API必需的请求头
    esp_http_client_set_header(client, "X-TC-Action", TENCENT_ASR_ACTION);
    esp_http_client_set_header(client, "X-TC-Version", TENCENT_ASR_VERSION);
    esp_http_client_set_header(client, "X-TC-Timestamp", timestamp_str);
    
    // 生成TC3签名
    char signature[65] = {0};
    esp_err_t sig_ret = generate_tc3_signature(json_string, timestamp_str, date_str, signature, sizeof(signature));
    if (sig_ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to generate TC3 signature");
        strcpy(signature, "placeholder_signature");
    }
    
    // 构建正确的认证头
    char auth_header[512];
    snprintf(auth_header, sizeof(auth_header), 
             "TC3-HMAC-SHA256 Credential=%s/%s/asr/tc3_request, SignedHeaders=content-type;host, Signature=%s", 
             TENCENT_SECRET_ID, date_str, signature);
    esp_http_client_set_header(client, "Authorization", auth_header);
    
    ESP_LOGI(TAG, "Authorization header: %s", auth_header);
    
    esp_http_client_set_post_field(client, json_string, strlen(json_string));
    
    // 发送请求
    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        int status_code = esp_http_client_get_status_code(client);
        ESP_LOGI(TAG, "HTTP Status = %d", status_code);
        
        if (status_code == 200 && g_http_response_buffer) {
            ESP_LOGI(TAG, "API Response: %s", g_http_response_buffer);
            ret = parse_api_response(g_http_response_buffer);
        } else {
            snprintf(g_speech_result.error_message, sizeof(g_speech_result.error_message),
                    "HTTP Error: Status %d", status_code);
            g_speech_result.state = SPEECH_STATE_ERROR;
        }
    } else {
        ESP_LOGE(TAG, "HTTP request failed: %s", esp_err_to_name(err));
        snprintf(g_speech_result.error_message, sizeof(g_speech_result.error_message),
                "HTTP Request Failed: %s", esp_err_to_name(err));
        g_speech_result.state = SPEECH_STATE_ERROR;
    }
    
    esp_http_client_cleanup(client);
    free(json_string);
    cJSON_Delete(json);
    
    return ret;
}

// 录音任务
static void speech_recognition_task(void *arg)
{
    ESP_LOGI(TAG, "Speech recognition task started");
    
    // 检查可用内存
    size_t free_heap = esp_get_free_heap_size();
    size_t min_heap = esp_get_minimum_free_heap_size();
    ESP_LOGI(TAG, "Free heap: %zu bytes, Min free heap: %zu bytes", free_heap, min_heap);
    
    // 估算需要的内存 (32位原始缓冲区 + 16位PCM缓冲区 + HTTP缓冲区)
    size_t raw_buffer_size = SPEECH_BUFFER_SIZE * 2;  // 32位数据需要2倍空间
    size_t required_memory = raw_buffer_size + SPEECH_BUFFER_SIZE + HTTP_RESPONSE_BUFFER_SIZE + 8192;
    ESP_LOGI(TAG, "Required memory: %zu bytes (raw:%zu + pcm:%d + http:%d)", 
             required_memory, raw_buffer_size, SPEECH_BUFFER_SIZE, HTTP_RESPONSE_BUFFER_SIZE);
    
    if (free_heap < required_memory) {
        ESP_LOGE(TAG, "Insufficient memory: need %zu, have %zu", required_memory, free_heap);
        g_speech_result.state = SPEECH_STATE_ERROR;
        strcpy(g_speech_result.error_message, "Insufficient memory");
        goto cleanup;
    }
    
    // 分配32位原始音频缓冲区 (用于I2S读取)
    g_raw_audio_buffer = heap_caps_malloc(raw_buffer_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!g_raw_audio_buffer) {
        ESP_LOGE(TAG, "Failed to allocate raw audio buffer in PSRAM (%zu bytes)", raw_buffer_size);
        g_raw_audio_buffer = malloc(raw_buffer_size);
        if (!g_raw_audio_buffer) {
            ESP_LOGE(TAG, "Failed to allocate raw audio buffer in internal RAM (%zu bytes)", raw_buffer_size);
            g_speech_result.state = SPEECH_STATE_ERROR;
            strcpy(g_speech_result.error_message, "Raw audio buffer allocation failed");
            goto cleanup;
        }
        ESP_LOGI(TAG, "Raw audio buffer allocated in internal RAM: %zu bytes", raw_buffer_size);
    } else {
        ESP_LOGI(TAG, "Raw audio buffer allocated in PSRAM: %zu bytes", raw_buffer_size);
    }
    
    // 分配16位PCM音频缓冲区 (用于WAV生成)
    g_audio_buffer = heap_caps_malloc(SPEECH_BUFFER_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!g_audio_buffer) {
        ESP_LOGE(TAG, "Failed to allocate PCM audio buffer in PSRAM (%d bytes)", SPEECH_BUFFER_SIZE);
        g_audio_buffer = malloc(SPEECH_BUFFER_SIZE);
        if (!g_audio_buffer) {
            ESP_LOGE(TAG, "Failed to allocate PCM audio buffer in internal RAM (%d bytes)", SPEECH_BUFFER_SIZE);
            g_speech_result.state = SPEECH_STATE_ERROR;
            strcpy(g_speech_result.error_message, "PCM audio buffer allocation failed");
            goto cleanup;
        }
        ESP_LOGI(TAG, "PCM audio buffer allocated in internal RAM: %d bytes", SPEECH_BUFFER_SIZE);
    } else {
        ESP_LOGI(TAG, "PCM audio buffer allocated in PSRAM: %d bytes", SPEECH_BUFFER_SIZE);
    }
    
    // HTTP响应缓冲区
    g_http_response_buffer = malloc(HTTP_RESPONSE_BUFFER_SIZE);
    if (!g_http_response_buffer) {
        ESP_LOGE(TAG, "Failed to allocate HTTP response buffer (%d bytes)", HTTP_RESPONSE_BUFFER_SIZE);
        g_speech_result.state = SPEECH_STATE_ERROR;
        strcpy(g_speech_result.error_message, "HTTP buffer allocation failed");
        goto cleanup;
    }
    ESP_LOGI(TAG, "HTTP response buffer allocated: %d bytes", HTTP_RESPONSE_BUFFER_SIZE);
    
    memset(g_raw_audio_buffer, 0, raw_buffer_size);
    memset(g_audio_buffer, 0, SPEECH_BUFFER_SIZE);
    memset(g_http_response_buffer, 0, HTTP_RESPONSE_BUFFER_SIZE);
    
    // 初始化I2S
    if (i2s_mic_init() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize I2S microphone");
        g_speech_result.state = SPEECH_STATE_ERROR;
        strcpy(g_speech_result.error_message, "Microphone initialization failed");
        goto cleanup;
    }
    
    // 使用全局I2S句柄
    if (!g_rx_handle) {
        ESP_LOGE(TAG, "I2S RX handle is NULL");
        g_speech_result.state = SPEECH_STATE_ERROR;
        strcpy(g_speech_result.error_message, "I2S handle error");
        goto cleanup;
    }
    
    // 启用I2S通道
    esp_err_t ret = i2s_channel_enable(g_rx_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable I2S channel: %s", esp_err_to_name(ret));
        g_speech_result.state = SPEECH_STATE_ERROR;
        strcpy(g_speech_result.error_message, "I2S channel enable failed");
        goto cleanup;
    }
    
    // 开始录音
    g_speech_result.state = SPEECH_STATE_RECORDING;
    ESP_LOGI(TAG, "Starting audio recording for %d ms (32-bit I2S data)", SPEECH_RECORD_TIME_MS);
    
    size_t bytes_read = 0;
    size_t total_bytes_read = 0;
    TickType_t start_time = xTaskGetTickCount();
    TickType_t timeout_ticks = pdMS_TO_TICKS(SPEECH_RECORD_TIME_MS + 1000); // 额外1秒超时
    
    // 读取32位I2S数据到原始缓冲区 - 优化读取策略
    ESP_LOGI(TAG, "Starting I2S data read loop...");
    uint32_t read_attempts = 0;
    uint32_t successful_reads = 0;
    
    while (total_bytes_read < raw_buffer_size && 
           (xTaskGetTickCount() - start_time) < timeout_ticks) {
        
        size_t remaining = raw_buffer_size - total_bytes_read;
        size_t read_size = (remaining > 1024) ? 1024 : remaining; // 优化读取块大小
        
        read_attempts++;
        esp_err_t read_ret = i2s_channel_read(g_rx_handle, 
                                       (char*)g_raw_audio_buffer + total_bytes_read,
                                       read_size, &bytes_read, 200); // 增加超时时间
        
        if (read_ret == ESP_OK && bytes_read > 0) {
            total_bytes_read += bytes_read;
            successful_reads++;
            
            // 每读取一定数据量后打印进度
            if (successful_reads % 50 == 0) {
                float progress = (float)total_bytes_read / raw_buffer_size * 100.0f;
                ESP_LOGI(TAG, "Recording progress: %.1f%% (%zu/%zu bytes)", 
                         progress, total_bytes_read, raw_buffer_size);
            }
        } else if (read_ret != ESP_ERR_TIMEOUT) {
            ESP_LOGE(TAG, "I2S read error: %s (attempt %"PRIu32")", esp_err_to_name(read_ret), read_attempts);
            break;
        }
        
        // 减少不必要的延迟
        if (bytes_read == 0) {
            vTaskDelay(pdMS_TO_TICKS(2)); // 只在没有数据时延迟
        }
    }
    
        ESP_LOGI(TAG, "I2S read completed: %"PRIu32" attempts, %"PRIu32" successful, %zu bytes total",
             read_attempts, successful_reads, total_bytes_read);
    
    ESP_LOGI(TAG, "Raw audio recording completed, read %zu bytes (32-bit)", total_bytes_read);
    
    // 转换32位数据为16位PCM数据
    size_t sample_count = total_bytes_read / sizeof(int32_t);
    ESP_LOGI(TAG, "Converting %zu samples from 32-bit to 16-bit", sample_count);
    
    convert_32bit_to_16bit(g_raw_audio_buffer, g_audio_buffer, sample_count);
    
    // 更新实际的16位数据大小
    size_t pcm_bytes = sample_count * sizeof(int16_t);
    ESP_LOGI(TAG, "PCM conversion completed, %zu bytes (16-bit)", pcm_bytes);
    
    // 检查音频数据质量
    bool data_quality_ok = check_audio_data_quality(g_audio_buffer, sample_count);
    if (!data_quality_ok) {
        ESP_LOGW(TAG, "Audio data quality check failed, but continuing with processing");
    }
    
    // 禁用I2S通道
    esp_err_t disable_ret = i2s_channel_disable(g_rx_handle);
    if (disable_ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to disable I2S channel: %s", esp_err_to_name(disable_ret));
    }
    
    if (total_bytes_read == 0 || sample_count == 0) {
        ESP_LOGE(TAG, "No audio data recorded");
        g_speech_result.state = SPEECH_STATE_ERROR;
        strcpy(g_speech_result.error_message, "No audio data recorded");
        goto cleanup;
    }
    
    // 释放32位原始缓冲区，节省内存
    if (g_raw_audio_buffer) {
        free(g_raw_audio_buffer);
        g_raw_audio_buffer = NULL;
        ESP_LOGI(TAG, "Raw audio buffer freed");
    }
    
    ESP_LOGI(TAG, "Audio recording and conversion completed");
    size_t free_heap_after_recording = esp_get_free_heap_size();
    ESP_LOGI(TAG, "Free heap after recording: %zu bytes", free_heap_after_recording);
    
    // 创建WAV格式数据
    g_speech_result.state = SPEECH_STATE_PROCESSING;
    ESP_LOGI(TAG, "Creating WAV format from PCM data (%zu bytes)", pcm_bytes);
    
    uint8_t *wav_data = NULL;
    size_t wav_len = 0;
    if (create_wav_data(g_audio_buffer, pcm_bytes, &wav_data, &wav_len) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create WAV data");
        g_speech_result.state = SPEECH_STATE_ERROR;
        strcpy(g_speech_result.error_message, "WAV creation failed");
        goto cleanup;
    }
    
    ESP_LOGI(TAG, "WAV data created: %zu bytes", wav_len);
    
    // Base64编码 - 在需要时分配缓冲区
    ESP_LOGI(TAG, "Allocating Base64 buffer and encoding WAV data");
    
    // 使用PSRAM分配Base64缓冲区
    g_base64_buffer = heap_caps_malloc(SPEECH_BASE64_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!g_base64_buffer) {
        ESP_LOGE(TAG, "Failed to allocate Base64 buffer in PSRAM (%d bytes)", SPEECH_BASE64_SIZE);
        // 尝试从内部RAM分配
        g_base64_buffer = malloc(SPEECH_BASE64_SIZE);
        if (!g_base64_buffer) {
            ESP_LOGE(TAG, "Failed to allocate Base64 buffer in internal RAM (%d bytes)", SPEECH_BASE64_SIZE);
            g_speech_result.state = SPEECH_STATE_ERROR;
            strcpy(g_speech_result.error_message, "Base64 buffer allocation failed");
            if (wav_data) free(wav_data);
            goto cleanup;
        }
        ESP_LOGI(TAG, "Base64 buffer allocated in internal RAM: %d bytes", SPEECH_BASE64_SIZE);
    } else {
        ESP_LOGI(TAG, "Base64 buffer allocated in PSRAM: %d bytes", SPEECH_BASE64_SIZE);
    }
    memset(g_base64_buffer, 0, SPEECH_BASE64_SIZE);
    
    size_t base64_len;
    if (encode_base64(wav_data, wav_len, 
                     g_base64_buffer, SPEECH_BASE64_SIZE, &base64_len) != ESP_OK) {
        ESP_LOGE(TAG, "Base64 encoding failed");
        g_speech_result.state = SPEECH_STATE_ERROR;
        strcpy(g_speech_result.error_message, "Base64 encoding failed");
        if (wav_data) free(wav_data);
        goto cleanup;
    }
    
    ESP_LOGI(TAG, "Base64 encoding completed, length: %zu", base64_len);
    
    // 释放WAV数据缓冲区
    if (wav_data) {
        free(wav_data);
        wav_data = NULL;
    }
    
    // 发送API请求
    ESP_LOGI(TAG, "Sending API request to Tencent Cloud");
    if (send_api_request(g_base64_buffer, wav_len) != ESP_OK) {
        ESP_LOGE(TAG, "API request failed");
        if (g_speech_result.state != SPEECH_STATE_ERROR) {
            g_speech_result.state = SPEECH_STATE_ERROR;
            strcpy(g_speech_result.error_message, "API request failed");
        }
    }
    
cleanup:
    // 释放内存
    if (g_raw_audio_buffer) {
        free(g_raw_audio_buffer);
        g_raw_audio_buffer = NULL;
    }
    if (g_audio_buffer) {
        free(g_audio_buffer);
        g_audio_buffer = NULL;
    }
    if (g_base64_buffer) {
        free(g_base64_buffer);
        g_base64_buffer = NULL;
    }
    if (g_http_response_buffer) {
        free(g_http_response_buffer);
        g_http_response_buffer = NULL;
    }
    
    // I2S保持初始化状态以供下次使用
    
    g_speech_active = false;
    g_speech_task_handle = NULL;
    
    ESP_LOGI(TAG, "Speech recognition task completed");
    vTaskDelete(NULL);
}

// 公共函数实现
esp_err_t speech_recognition_init(void)
{
    if (g_speech_mutex == NULL) {
        g_speech_mutex = xSemaphoreCreateMutex();
        if (g_speech_mutex == NULL) {
            ESP_LOGE(TAG, "Failed to create mutex");
            return ESP_FAIL;
        }
    }
    
    // 初始化结果结构
    memset(&g_speech_result, 0, sizeof(g_speech_result));
    g_speech_result.state = SPEECH_STATE_IDLE;
    
    // 初始化AI对话模块
    esp_err_t ai_ret = ai_chat_init();
    if (ai_ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize AI chat module");
        return ai_ret;
    }
    
    ESP_LOGI(TAG, "Speech recognition initialized");
    return ESP_OK;
}

esp_err_t speech_recognition_start(void)
{
    if (xSemaphoreTake(g_speech_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_FAIL;
    }
    
    if (g_speech_active) {
        xSemaphoreGive(g_speech_mutex);
        ESP_LOGW(TAG, "Speech recognition already active");
        return ESP_ERR_INVALID_STATE;
    }
    
    // 重置结果
    memset(&g_speech_result, 0, sizeof(g_speech_result));
    g_speech_result.state = SPEECH_STATE_IDLE;
    
    // 创建任务 - 减少栈大小
    BaseType_t ret = xTaskCreate(speech_recognition_task, "speech_rec", 
                                8192, NULL, 5, &g_speech_task_handle);
    
    if (ret != pdPASS) {
        xSemaphoreGive(g_speech_mutex);
        ESP_LOGE(TAG, "Failed to create speech recognition task");
        return ESP_FAIL;
    }
    
    g_speech_active = true;
    xSemaphoreGive(g_speech_mutex);
    
    ESP_LOGI(TAG, "Speech recognition started");
    return ESP_OK;
}

esp_err_t speech_recognition_stop(void)
{
    if (xSemaphoreTake(g_speech_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_FAIL;
    }
    
    g_speech_active = false;
    
    // 等待任务结束
    if (g_speech_task_handle != NULL) {
        // 任务会自动删除自己
        g_speech_task_handle = NULL;
    }
    
    xSemaphoreGive(g_speech_mutex);
    
    ESP_LOGI(TAG, "Speech recognition stopped");
    return ESP_OK;
}

speech_recognition_result_t* speech_recognition_get_result(void)
{
    return &g_speech_result;
}

void speech_recognition_deinit(void)
{
    speech_recognition_stop();
    
    // 清理I2S资源
    i2s_mic_deinit();
    
    if (g_speech_mutex != NULL) {
        vSemaphoreDelete(g_speech_mutex);
        g_speech_mutex = NULL;
    }
    
    ESP_LOGI(TAG, "Speech recognition deinitialized");
}

bool speech_recognition_is_active(void)
{
    return g_speech_active;
} 