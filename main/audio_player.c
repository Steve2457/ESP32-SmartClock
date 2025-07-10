#include "audio_player.h"
#include "audio_data.h"
#include "esp_log.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>
// 添加SPIFFS支持
#include "esp_spiffs.h"
#include <dirent.h>
#include <sys/stat.h>

static const char *TAG = "AUDIO_PLAYER";

// 音频播放器状态
static audio_player_state_t player_state = AUDIO_PLAYER_IDLE;
static TaskHandle_t audio_task_handle = NULL;
static QueueHandle_t audio_queue = NULL;
static int current_volume = 80; // 默认音量80%
static i2s_chan_handle_t tx_handle = NULL;
static bool audio_initialized = false;
static bool spiffs_initialized = false;
static audio_state_t current_audio_state = AUDIO_STATE_IDLE;

// WAV文件头结构已在头文件中定义

// 音频命令结构
typedef enum {
    AUDIO_CMD_PLAY_PCM,
    AUDIO_CMD_PLAY_WAV,
    AUDIO_CMD_PLAY_TONE,
    AUDIO_CMD_STOP,
    AUDIO_CMD_PAUSE,
    AUDIO_CMD_RESUME
} audio_cmd_type_t;

typedef struct {
    audio_cmd_type_t type;
    const uint8_t *data;
    size_t length;
    float frequency;
    int duration;
    int volume;
} audio_cmd_t;

/**
 * @brief 应用音量到音频数据（增强版本）
 */
static void apply_volume(int16_t *samples, size_t num_samples, int volume) {
    float vol_factor = (float)volume / 100.0f;
    float gain = 2.5f;  // 增加2.5倍增益以提高音量
    
    for (size_t i = 0; i < num_samples; i++) {
        int32_t sample = (int32_t)(samples[i] * vol_factor * gain);
        // 防止溢出并进行软限幅
        if (sample > 32767) sample = 32767;
        if (sample < -32768) sample = -32768;
        samples[i] = (int16_t)sample;
    }
}

/**
 * @brief 生成正弦波音调
 */
static void generate_tone(float frequency, int duration_ms, int volume, int16_t **samples, size_t *num_samples) {
    *num_samples = (SAMPLE_RATE * duration_ms / 1000) * 2; // 立体声
    *samples = malloc(*num_samples * sizeof(int16_t));
    
    if (*samples == NULL) {
        ESP_LOGE(TAG, "内存分配失败");
        *num_samples = 0;
        return;
    }
    
    float vol_factor = (float)volume / 100.0f;
    float amplitude = 20000.0f * vol_factor; // 增加基础幅度
    
    for (size_t i = 0; i < *num_samples / 2; i++) {
        float sample_time = (float)i / SAMPLE_RATE;
        int16_t sample = (int16_t)(amplitude * sin(2 * M_PI * frequency * sample_time));
        
        (*samples)[i * 2] = sample;     // 左声道
        (*samples)[i * 2 + 1] = sample; // 右声道
    }
}

/**
 * @brief 音频播放任务
 */
static void audio_task(void *pvParameters) {
    audio_cmd_t cmd;
    size_t bytes_written;
    
    while (1) {
        if (xQueueReceive(audio_queue, &cmd, portMAX_DELAY) == pdTRUE) {
            switch (cmd.type) {
                case AUDIO_CMD_PLAY_PCM:
                    player_state = AUDIO_PLAYER_PLAYING;
                    ESP_LOGI(TAG, "播放PCM数据，长度: %zu字节，音量: %d%%", cmd.length, current_volume);
                    
                    // 复制数据并应用音量
                    int16_t *pcm_data = malloc(cmd.length);
                    if (pcm_data) {
                        memcpy(pcm_data, cmd.data, cmd.length);
                        apply_volume(pcm_data, cmd.length / 2, current_volume);
                        
                        esp_err_t ret = i2s_channel_write(tx_handle, pcm_data, cmd.length, &bytes_written, portMAX_DELAY);
                        ESP_LOGI(TAG, "I2S写入结果: %s, 请求: %zu, 实际: %zu", 
                                esp_err_to_name(ret), cmd.length, bytes_written);
                        free(pcm_data);
                    }
                    player_state = AUDIO_PLAYER_IDLE;
                    break;
                    
                case AUDIO_CMD_PLAY_WAV:
                    player_state = AUDIO_PLAYER_PLAYING;
                    ESP_LOGI(TAG, "播放WAV文件，大小: %zu字节", cmd.length);
                    
                    wav_header_t *header = (wav_header_t *)cmd.data;
                    
                    // 验证WAV文件头
                    if (strncmp(header->riff, "RIFF", 4) != 0 || 
                        strncmp(header->wave, "WAVE", 4) != 0 ||
                        strncmp(header->data_chunk, "data", 4) != 0) {
                        ESP_LOGE(TAG, "无效的WAV文件格式");
                        player_state = AUDIO_PLAYER_IDLE;
                        break;
                    }
                    
                    ESP_LOGI(TAG, "WAV信息: 采样率=%lu, 声道=%d, 位深=%d", 
                             header->sample_rate, header->num_channels, header->bit_depth);
                    
                    // 播放音频数据
                    const uint8_t *audio_data = cmd.data + sizeof(wav_header_t);
                    size_t audio_size = header->data_bytes;
                    
                    // 复制数据并应用音量
                    int16_t *wav_data = malloc(audio_size);
                    if (wav_data) {
                        memcpy(wav_data, audio_data, audio_size);
                        apply_volume(wav_data, audio_size / 2, current_volume);
                        
                        esp_err_t ret = i2s_channel_write(tx_handle, wav_data, audio_size, &bytes_written, portMAX_DELAY);
                        ESP_LOGI(TAG, "WAV播放结果: %s, 写入字节: %zu", esp_err_to_name(ret), bytes_written);
                        free(wav_data);
                    }
                    player_state = AUDIO_PLAYER_IDLE;
                    break;
                    
                case AUDIO_CMD_PLAY_TONE:
                    player_state = AUDIO_PLAYER_PLAYING;
                    ESP_LOGI(TAG, "播放音调: %.1fHz, %dms, 音量%d%%", 
                             cmd.frequency, cmd.duration, cmd.volume);
                    
                    int16_t *tone_samples;
                    size_t tone_num_samples;
                    generate_tone(cmd.frequency, cmd.duration, cmd.volume, &tone_samples, &tone_num_samples);
                    
                    if (tone_samples) {
                        esp_err_t ret = i2s_channel_write(tx_handle, tone_samples, tone_num_samples * sizeof(int16_t), 
                                         &bytes_written, portMAX_DELAY);
                        ESP_LOGI(TAG, "音调播放结果: %s, 写入字节: %zu", esp_err_to_name(ret), bytes_written);
                        free(tone_samples);
                    }
                    player_state = AUDIO_PLAYER_IDLE;
                    break;
                    

                    
                case AUDIO_CMD_STOP:
                    ESP_LOGI(TAG, "停止播放");
                    i2s_channel_disable(tx_handle);
                    i2s_channel_enable(tx_handle);
                    player_state = AUDIO_PLAYER_STOPPED;
                    break;
                    
                case AUDIO_CMD_PAUSE:
                    ESP_LOGI(TAG, "暂停播放");
                    i2s_channel_disable(tx_handle);
                    player_state = AUDIO_PLAYER_PAUSED;
                    break;
                    
                case AUDIO_CMD_RESUME:
                    ESP_LOGI(TAG, "恢复播放");
                    i2s_channel_enable(tx_handle);
                    player_state = AUDIO_PLAYER_PLAYING;
                    break;
                    
                default:
                    break;
            }
        }
    }
}

esp_err_t audio_player_init(void) {
    esp_err_t ret;
    
    ESP_LOGI(TAG, "初始化音频播放器");
    
    // 【新增】确保GPIO引脚模式正确设置
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << I2S_BCK_IO) | (1ULL << I2S_WS_IO) | (1ULL << I2S_DO_IO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ret = gpio_config(&io_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "GPIO配置失败: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "GPIO配置完成: BCK=%d, WS=%d, DO=%d", I2S_BCK_IO, I2S_WS_IO, I2S_DO_IO);
    
    // I2S通道配置（增强版本）
    i2s_chan_config_t chan_cfg = {
        .id = I2S_NUM_AUTO,
        .role = I2S_ROLE_MASTER,
        .dma_desc_num = 8,    // 增加DMA描述符数量
        .dma_frame_num = 1024, // 增加帧数量  
        .auto_clear = true,
    };
    
    // 创建I2S TX通道
    ret = i2s_new_channel(&chan_cfg, &tx_handle, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2S通道创建失败: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "I2S通道创建成功");
    
    // I2S标准配置（改进版本）
    i2s_std_config_t std_cfg = {
        .clk_cfg = {
            .sample_rate_hz = SAMPLE_RATE,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
        },
        .slot_cfg = {
            .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT,
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
            .slot_mode = I2S_SLOT_MODE_STEREO,
            .slot_mask = I2S_STD_SLOT_BOTH,
            .ws_width = I2S_DATA_BIT_WIDTH_16BIT,
            .ws_pol = false,
            .bit_shift = true,
        },
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_BCK_IO,
            .ws = I2S_WS_IO,
            .dout = I2S_DO_IO,
            .din = I2S_DI_IO,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    
    // 初始化I2S通道
    ret = i2s_channel_init_std_mode(tx_handle, &std_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2S标准模式初始化失败: %s", esp_err_to_name(ret));
        i2s_del_channel(tx_handle);
        return ret;
    }
    ESP_LOGI(TAG, "I2S标准模式初始化成功");
    
    // 启用I2S通道
    ret = i2s_channel_enable(tx_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2S通道启用失败: %s", esp_err_to_name(ret));
        i2s_del_channel(tx_handle);
        return ret;
    }
    ESP_LOGI(TAG, "I2S通道启用成功");
    
    // 创建命令队列
    audio_queue = xQueueCreate(10, sizeof(audio_cmd_t));
    if (audio_queue == NULL) {
        ESP_LOGE(TAG, "创建音频命令队列失败");
        i2s_del_channel(tx_handle);
        return ESP_ERR_NO_MEM;
    }
    
    // 创建音频播放任务
    if (xTaskCreate(audio_task, "audio_task", 4096, NULL, 5, &audio_task_handle) != pdTRUE) {
        ESP_LOGE(TAG, "创建音频播放任务失败");
        vQueueDelete(audio_queue);
        i2s_del_channel(tx_handle);
        return ESP_ERR_NO_MEM;
    }
    
    player_state = AUDIO_PLAYER_IDLE;
    ESP_LOGI(TAG, "音频播放器初始化成功 - 采样率: %dHz, 位深: 16bit, 声道: 立体声", SAMPLE_RATE);
    
    audio_initialized = true;
    return ESP_OK;
}

esp_err_t audio_player_play_pcm(const uint8_t *data, size_t length) {
    if (data == NULL || length == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    
    audio_cmd_t cmd = {
        .type = AUDIO_CMD_PLAY_PCM,
        .data = data,
        .length = length
    };
    
    return (xQueueSend(audio_queue, &cmd, pdMS_TO_TICKS(100)) == pdTRUE) ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t audio_player_play_wav(const uint8_t *wav_data, size_t wav_size) {
    if (wav_data == NULL || wav_size < sizeof(wav_header_t)) {
        return ESP_ERR_INVALID_ARG;
    }
    
    audio_cmd_t cmd = {
        .type = AUDIO_CMD_PLAY_WAV,
        .data = wav_data,
        .length = wav_size
    };
    
    return (xQueueSend(audio_queue, &cmd, pdMS_TO_TICKS(100)) == pdTRUE) ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t audio_player_play_tone(float frequency, int duration, int volume) {
    if (frequency <= 0 || duration <= 0 || volume < 0 || volume > 100) {
        return ESP_ERR_INVALID_ARG;
    }
    
    audio_cmd_t cmd = {
        .type = AUDIO_CMD_PLAY_TONE,
        .frequency = frequency,
        .duration = duration,
        .volume = volume
    };
    
    return (xQueueSend(audio_queue, &cmd, pdMS_TO_TICKS(100)) == pdTRUE) ? ESP_OK : ESP_ERR_TIMEOUT;
}



esp_err_t audio_player_pause(void) {
    audio_cmd_t cmd = { .type = AUDIO_CMD_PAUSE };
    return (xQueueSend(audio_queue, &cmd, pdMS_TO_TICKS(100)) == pdTRUE) ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t audio_player_resume(void) {
    audio_cmd_t cmd = { .type = AUDIO_CMD_RESUME };
    return (xQueueSend(audio_queue, &cmd, pdMS_TO_TICKS(100)) == pdTRUE) ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t audio_player_stop(void) {
    audio_cmd_t cmd = { .type = AUDIO_CMD_STOP };
    return (xQueueSend(audio_queue, &cmd, pdMS_TO_TICKS(100)) == pdTRUE) ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t audio_player_set_volume(int volume) {
    if (volume < 0 || volume > 100) {
        return ESP_ERR_INVALID_ARG;
    }
    
    current_volume = volume;
    ESP_LOGI(TAG, "设置音量为: %d%%", volume);
    
    return ESP_OK;
}

audio_player_state_t audio_player_get_state(void) {
    return player_state;
}

esp_err_t audio_player_deinit(void) {
    ESP_LOGI(TAG, "反初始化音频播放器");
    
    // 停止播放
    audio_player_stop();
    
    // 删除任务
    if (audio_task_handle) {
        vTaskDelete(audio_task_handle);
        audio_task_handle = NULL;
    }
    
    // 删除队列
    if (audio_queue) {
        vQueueDelete(audio_queue);
        audio_queue = NULL;
    }
    
    // 卸载I2S通道
    if (tx_handle) {
        i2s_channel_disable(tx_handle);
        i2s_del_channel(tx_handle);
        tx_handle = NULL;
    }
    
    player_state = AUDIO_PLAYER_IDLE;
    
    audio_initialized = false;
    return ESP_OK;
}

/**
 * @brief 初始化SPIFFS文件系统
 */
esp_err_t audio_spiffs_init(void) {
    if (spiffs_initialized) {
        ESP_LOGI(TAG, "SPIFFS already initialized");
        return ESP_OK;
    }

    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs",
        .partition_label = "storage",
        .max_files = 20,
        .format_if_mount_failed = true
    };

    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount or format filesystem");
        } else if (ret == ESP_ERR_NOT_FOUND) {
            ESP_LOGE(TAG, "Failed to find SPIFFS partition");
        } else {
            ESP_LOGE(TAG, "Failed to initialize SPIFFS (%s)", esp_err_to_name(ret));
        }
        return ret;
    }

    size_t total = 0, used = 0;
    ret = esp_spiffs_info("storage", &total, &used);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get SPIFFS partition information (%s)", esp_err_to_name(ret));
        esp_vfs_spiffs_unregister("storage");
        return ret;
    }

    ESP_LOGI(TAG, "SPIFFS initialized successfully");
    ESP_LOGI(TAG, "Partition size: total: %d, used: %d", total, used);
    
    spiffs_initialized = true;
    return ESP_OK;
}

/**
 * @brief 反初始化SPIFFS文件系统
 */
void audio_spiffs_deinit(void) {
    if (spiffs_initialized) {
        esp_vfs_spiffs_unregister("storage");
        spiffs_initialized = false;
        ESP_LOGI(TAG, "SPIFFS deinitialized");
    }
}

/**
 * @brief 列出SPIFFS中的所有WAV文件
 */
int audio_list_wav_files(char file_list[][64], int max_files) {
    if (!spiffs_initialized) {
        ESP_LOGE(TAG, "SPIFFS not initialized");
        return 0;
    }

    DIR* dir = opendir("/spiffs");
    if (dir == NULL) {
        ESP_LOGE(TAG, "Failed to open directory /spiffs");
        return 0;
    }

    struct dirent* entry;
    int count = 0;
    
    while ((entry = readdir(dir)) != NULL && count < max_files) {
        // 检查是否为WAV文件
        char* ext = strrchr(entry->d_name, '.');
        if (ext != NULL && (strcasecmp(ext, ".wav") == 0)) {
            strncpy(file_list[count], entry->d_name, 63);
            file_list[count][63] = '\0';
            ESP_LOGI(TAG, "Found WAV file: %s", file_list[count]);
            count++;
        }
    }
    
    closedir(dir);
    ESP_LOGI(TAG, "Found %d WAV files", count);
    return count;
}

/**
 * @brief 从SPIFFS加载WAV文件
 */
esp_err_t audio_load_wav_file(const char* filename, wav_file_t* wav_file) {
    if (!spiffs_initialized || !filename || !wav_file) {
        ESP_LOGE(TAG, "Invalid parameters for WAV file loading");
        return ESP_ERR_INVALID_ARG;
    }

    char filepath[128];
    snprintf(filepath, sizeof(filepath), "/spiffs/%s", filename);

    FILE* file = fopen(filepath, "rb");
    if (!file) {
        ESP_LOGE(TAG, "Failed to open WAV file: %s", filepath);
        return ESP_ERR_NOT_FOUND;
    }

    // 读取WAV文件头
    wav_header_t header;
    size_t read_size = fread(&header, 1, sizeof(wav_header_t), file);
    if (read_size != sizeof(wav_header_t)) {
        ESP_LOGE(TAG, "Failed to read WAV header");
        fclose(file);
        return ESP_ERR_INVALID_SIZE;
    }

    // 验证WAV文件格式
    if (strncmp(header.riff, "RIFF", 4) != 0 || 
        strncmp(header.wave, "WAVE", 4) != 0 ||
        strncmp(header.fmt_chunk, "fmt ", 4) != 0 ||
        strncmp(header.data_chunk, "data", 4) != 0) {
        ESP_LOGE(TAG, "Invalid WAV file format");
        fclose(file);
        return ESP_ERR_INVALID_ARG;
    }

    // 检查音频格式
    if (header.audio_format != 1) {
        ESP_LOGE(TAG, "Unsupported audio format: %d (only PCM supported)", header.audio_format);
        fclose(file);
        return ESP_ERR_NOT_SUPPORTED;
    }

        ESP_LOGI(TAG, "WAV file info: %luHz, %d channels, %d bits, %lu bytes",
             header.sample_rate, header.num_channels, header.bit_depth, header.data_bytes);

    // 分配内存存储音频数据
    wav_file->data = malloc(header.data_bytes);
    if (!wav_file->data) {
        ESP_LOGE(TAG, "Failed to allocate memory for WAV data (%lu bytes)", header.data_bytes);
        fclose(file);
        return ESP_ERR_NO_MEM;
    }

    // 读取音频数据
    read_size = fread(wav_file->data, 1, header.data_bytes, file);
    if (read_size != header.data_bytes) {
        ESP_LOGE(TAG, "Failed to read WAV data: read %zu, expected %lu", read_size, header.data_bytes);
        free(wav_file->data);
        wav_file->data = NULL;
        fclose(file);
        return ESP_ERR_INVALID_SIZE;
    }

    // 填充WAV文件信息
    wav_file->sample_rate = header.sample_rate;
    wav_file->num_channels = header.num_channels;
    wav_file->bit_depth = header.bit_depth;
    wav_file->data_size = header.data_bytes;

    fclose(file);
    ESP_LOGI(TAG, "WAV file loaded successfully: %s", filename);
    return ESP_OK;
}

/**
 * @brief 释放WAV文件内存
 */
void audio_free_wav_file(wav_file_t* wav_file) {
    if (wav_file && wav_file->data) {
        free(wav_file->data);
        wav_file->data = NULL;
        wav_file->data_size = 0;
        ESP_LOGI(TAG, "WAV file memory freed");
    }
}

/**
 * @brief 播放WAV文件
 */
esp_err_t audio_play_wav_file(const char* filename, uint8_t volume) {
    if (!audio_initialized) {
        ESP_LOGE(TAG, "Audio player not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    wav_file_t wav_file = {0};
    esp_err_t ret = audio_load_wav_file(filename, &wav_file);
    if (ret != ESP_OK) {
        return ret;
    }

    current_audio_state = AUDIO_STATE_PLAYING;

    // 播放WAV数据
    size_t bytes_written = 0;
    int16_t* samples = (int16_t*)wav_file.data;
    size_t sample_count = wav_file.data_size / sizeof(int16_t);
    
    // 音量调节
    float volume_factor = volume / 100.0f;
    
    for (size_t i = 0; i < sample_count; i++) {
        samples[i] = (int16_t)(samples[i] * volume_factor);
    }

    ret = i2s_channel_write(tx_handle, wav_file.data, wav_file.data_size, &bytes_written, portMAX_DELAY);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write WAV data to I2S");
        current_audio_state = AUDIO_STATE_ERROR;
    } else {
        ESP_LOGI(TAG, "WAV file played successfully: %s", filename);
        current_audio_state = AUDIO_STATE_IDLE;
    }

    audio_free_wav_file(&wav_file);
    return ret;
}

/**
 * @brief 停止当前播放
 */
void audio_stop_playback(void) {
    if (current_audio_state == AUDIO_STATE_PLAYING) {
        current_audio_state = AUDIO_STATE_IDLE;
        ESP_LOGI(TAG, "Audio playback stopped");
    }
}

/**
 * @brief 获取当前播放状态
 */
audio_state_t audio_get_state(void) {
    return current_audio_state;
} 