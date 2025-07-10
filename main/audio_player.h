#ifndef AUDIO_PLAYER_H
#define AUDIO_PLAYER_H

#include "esp_err.h"
#include "driver/i2s_std.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// MAX98357A引脚配置
#define I2S_BCK_IO      17  // BCLK引脚
#define I2S_WS_IO       18  // LRC引脚
#define I2S_DO_IO       16  // DIN引脚
#define I2S_DI_IO       -1  // 不使用输入

// I2S配置
#define I2S_NUM         I2S_NUM_0
#define SAMPLE_RATE     44100
#define I2S_BITS        I2S_BITS_PER_SAMPLE_16BIT
#define I2S_CHANNELS    I2S_CHANNEL_STEREO

// WAV文件头结构体
typedef struct {
    char riff[4];           // "RIFF"
    uint32_t overall_size;  // 文件大小 - 8
    char wave[4];           // "WAVE"
    char fmt_chunk[4];      // "fmt "
    uint32_t fmt_length;    // fmt块长度
    uint16_t audio_format;  // 音频格式 (1 = PCM)
    uint16_t num_channels;  // 声道数
    uint32_t sample_rate;   // 采样率
    uint32_t byte_rate;     // 比特率
    uint16_t sample_alignment; // 对齐
    uint16_t bit_depth;     // 位深度
    char data_chunk[4];     // "data"
    uint32_t data_bytes;    // 音频数据字节数
} wav_header_t;

// WAV文件信息结构体
typedef struct {
    uint32_t sample_rate;
    uint16_t num_channels;
    uint16_t bit_depth;
    uint32_t data_size;
    uint8_t* data;
} wav_file_t;

// 播放状态
typedef enum {
    AUDIO_PLAYER_IDLE,
    AUDIO_PLAYER_PLAYING,
    AUDIO_PLAYER_PAUSED,
    AUDIO_PLAYER_STOPPED
} audio_player_state_t;

// 音频播放状态
typedef enum {
    AUDIO_STATE_IDLE,
    AUDIO_STATE_PLAYING,
    AUDIO_STATE_PAUSED,
    AUDIO_STATE_ERROR
} audio_state_t;

/**
 * @brief 初始化音频播放器
 * @return ESP_OK 成功, 其他值失败
 */
esp_err_t audio_player_init(void);

/**
 * @brief 播放PCM音频数据
 * @param data PCM音频数据指针
 * @param length 数据长度（字节）
 * @return ESP_OK 成功, 其他值失败
 */
esp_err_t audio_player_play_pcm(const uint8_t *data, size_t length);

/**
 * @brief 播放WAV文件（从flash中）
 * @param wav_data WAV文件数据指针
 * @param wav_size WAV文件大小
 * @return ESP_OK 成功, 其他值失败
 */
esp_err_t audio_player_play_wav(const uint8_t *wav_data, size_t wav_size);

/**
 * @brief 播放提示音
 * @param frequency 频率 (Hz)
 * @param duration 持续时间 (ms)
 * @param volume 音量 (0-100)
 * @return ESP_OK 成功, 其他值失败
 */
esp_err_t audio_player_play_tone(float frequency, int duration, int volume);



/**
 * @brief 暂停播放
 * @return ESP_OK 成功, 其他值失败
 */
esp_err_t audio_player_pause(void);

/**
 * @brief 恢复播放
 * @return ESP_OK 成功, 其他值失败
 */
esp_err_t audio_player_resume(void);

/**
 * @brief 停止播放
 * @return ESP_OK 成功, 其他值失败
 */
esp_err_t audio_player_stop(void);

/**
 * @brief 设置音量
 * @param volume 音量 (0-100)
 * @return ESP_OK 成功, 其他值失败
 */
esp_err_t audio_player_set_volume(int volume);

/**
 * @brief 获取播放状态
 * @return 当前播放状态
 */
audio_player_state_t audio_player_get_state(void);

/**
 * @brief 反初始化音频播放器
 * @return ESP_OK 成功, 其他值失败
 */
esp_err_t audio_player_deinit(void);

/**
 * @brief 初始化SPIFFS文件系统
 * @return ESP_OK 成功，其他值失败
 */
esp_err_t audio_spiffs_init(void);

/**
 * @brief 反初始化SPIFFS文件系统
 */
void audio_spiffs_deinit(void);

/**
 * @brief 列出SPIFFS中的所有WAV文件
 * @param file_list 文件名列表（输出）
 * @param max_files 最大文件数
 * @return 实际文件数量
 */
int audio_list_wav_files(char file_list[][64], int max_files);

/**
 * @brief 从SPIFFS加载WAV文件
 * @param filename 文件名（如"sound.wav"）
 * @param wav_file WAV文件信息结构体（输出）
 * @return ESP_OK 成功，其他值失败
 */
esp_err_t audio_load_wav_file(const char* filename, wav_file_t* wav_file);

/**
 * @brief 释放WAV文件内存
 * @param wav_file WAV文件信息结构体
 */
void audio_free_wav_file(wav_file_t* wav_file);

/**
 * @brief 播放WAV文件
 * @param filename 文件名
 * @param volume 音量 (0-100)
 * @return ESP_OK 成功，其他值失败
 */
esp_err_t audio_play_wav_file(const char* filename, uint8_t volume);

/**
 * @brief 停止当前播放
 */
void audio_stop_playback(void);

/**
 * @brief 获取当前播放状态
 * @return audio_state_t 播放状态
 */
audio_state_t audio_get_state(void);

#endif // AUDIO_PLAYER_H 