#ifndef AUDIO_DATA_H
#define AUDIO_DATA_H

#include <stdint.h>
#include <stddef.h>

// 启动提示音 - 440Hz正弦波（0.1秒）
extern uint8_t *startup_sound_data;
extern const size_t startup_sound_size;

// 按键提示音 - 800Hz正弦波（0.05秒）
extern uint8_t *beep_sound_data;
extern const size_t beep_sound_size;

// 警告音 - 1000Hz正弦波（0.1秒）
extern uint8_t *warning_sound_data;
extern const size_t warning_sound_size;

// 示例音乐片段 - 简单的旋律（0.3秒）
extern uint8_t *melody_data;
extern const size_t melody_size;

// 铃声2 - 双音调报警音（0.8秒）
extern uint8_t *alarm_tone_data;
extern const size_t alarm_tone_size;

/**
 * @brief 初始化音频数据
 */
void audio_data_init(void);

#endif // AUDIO_DATA_H 