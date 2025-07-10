#include "audio_data.h"
#include "esp_log.h"
#include <math.h>
#include <stdlib.h>

#define SAMPLE_RATE 44100
#define PI 3.14159265359

// 生成正弦波音频数据的辅助函数
static void generate_sine_wave(uint8_t *buffer, size_t size, float frequency, float duration) {
    int16_t *samples = (int16_t *)buffer;
    size_t num_samples = size / sizeof(int16_t) / 2; // 立体声
    float amplitude = 8192; // 25%音量
    
    for (size_t i = 0; i < num_samples; i++) {
        float t = (float)i / SAMPLE_RATE;
        if (t >= duration) break;
        
        int16_t sample = (int16_t)(amplitude * sin(2 * PI * frequency * t));
        samples[i * 2] = sample;     // 左声道
        samples[i * 2 + 1] = sample; // 右声道
    }
}

// 音频数据指针（动态分配）
uint8_t *startup_sound_data = NULL;
uint8_t *beep_sound_data = NULL;
uint8_t *warning_sound_data = NULL;
uint8_t *melody_data = NULL;
uint8_t *alarm_tone_data = NULL;

// 音频数据大小（减小到合理大小）
const size_t startup_sound_size = 8820 * sizeof(int16_t); // 0.1秒立体声16位
const size_t beep_sound_size = 4410 * sizeof(int16_t);    // 0.05秒立体声16位  
const size_t warning_sound_size = 8820 * sizeof(int16_t); // 0.1秒立体声16位
const size_t melody_size = 26460 * sizeof(int16_t);       // 0.3秒立体声16位
const size_t alarm_tone_size = 70560 * sizeof(int16_t);   // 0.8秒立体声16位

// 生成旋律的辅助函数
static void generate_melody(uint8_t *buffer, size_t size) {
    int16_t *samples = (int16_t *)buffer;
    size_t total_samples = size / sizeof(int16_t) / 2; // 立体声
    float amplitude = 8192; // 25%音量
    
    // 音符频率 (Hz)
    float notes[] = {
        261.63, // C4 - 哆
        293.66, // D4 - 来
        329.63, // E4 - 咪
        349.23, // F4 - 发
        392.00, // G4 - 索
    };
    
    float note_duration = 0.06; // 每个音符0.06秒
    int notes_count = sizeof(notes) / sizeof(notes[0]);
    
    size_t sample_index = 0;
    
    for (int note = 0; note < notes_count && sample_index < total_samples; note++) {
        size_t note_samples = (size_t)(SAMPLE_RATE * note_duration);
        
        for (size_t i = 0; i < note_samples && sample_index < total_samples; i++) {
            float t = (float)i / SAMPLE_RATE;
            int16_t sample = (int16_t)(amplitude * sin(2 * PI * notes[note] * t));
            
            samples[sample_index * 2] = sample;     // 左声道
            samples[sample_index * 2 + 1] = sample; // 右声道
            sample_index++;
        }
    }
    
    // 填充剩余的静音
    while (sample_index < total_samples) {
        samples[sample_index * 2] = 0;
        samples[sample_index * 2 + 1] = 0;
        sample_index++;
    }
}

// 生成双音调报警音的辅助函数
static void generate_alarm_tone(uint8_t *buffer, size_t size) {
    int16_t *samples = (int16_t *)buffer;
    size_t total_samples = size / sizeof(int16_t) / 2; // 立体声
    float amplitude = 12288; // 37.5%音量，比普通音效稍大
    
    // 双音调频率
    float freq1 = 800.0;  // 高音
    float freq2 = 600.0;  // 低音
    
    float cycle_duration = 0.4; // 每个周期0.4秒（高低音各0.2秒）
    float tone_duration = 0.2;  // 每个音调持续0.2秒
    
    size_t sample_index = 0;
    float current_time = 0.0;
    
    while (sample_index < total_samples) {
        // 计算当前在周期中的位置
        float cycle_position = fmod(current_time, cycle_duration);
        float frequency;
        
        // 选择当前频率（前0.2秒用高音，后0.2秒用低音）
        if (cycle_position < tone_duration) {
            frequency = freq1; // 高音
        } else {
            frequency = freq2; // 低音
        }
        
        // 生成音调，添加渐变效果避免突变
        float fade_factor = 1.0;
        float transition_time = 0.02; // 2ms渐变时间
        if (cycle_position < transition_time || 
            (cycle_position > tone_duration - transition_time && cycle_position < tone_duration + transition_time) ||
            cycle_position > cycle_duration - transition_time) {
            float min_pos = fmin(cycle_position, fmin(fabs(cycle_position - tone_duration), cycle_duration - cycle_position));
            fade_factor = fmin(1.0, min_pos / transition_time);
        }
        
        int16_t sample = (int16_t)(amplitude * fade_factor * sin(2 * PI * frequency * current_time));
        
        samples[sample_index * 2] = sample;     // 左声道
        samples[sample_index * 2 + 1] = sample; // 右声道
        
        sample_index++;
        current_time = (float)sample_index / SAMPLE_RATE;
    }
}

// 初始化音频数据（在程序启动时调用）
void audio_data_init(void) {
    // 动态分配内存
    startup_sound_data = malloc(startup_sound_size);
    beep_sound_data = malloc(beep_sound_size);
    warning_sound_data = malloc(warning_sound_size);
    melody_data = malloc(melody_size);
    alarm_tone_data = malloc(alarm_tone_size);
    
    if (startup_sound_data == NULL || beep_sound_data == NULL || 
        warning_sound_data == NULL || melody_data == NULL || alarm_tone_data == NULL) {
        ESP_LOGE("AUDIO_DATA", "音频数据内存分配失败");
        return;
    }
    
    // 生成启动提示音 (0.1秒)
    generate_sine_wave(startup_sound_data, startup_sound_size, 440.0, 0.1);
    
    // 生成按键提示音 (0.05秒)
    generate_sine_wave(beep_sound_data, beep_sound_size, 800.0, 0.05);
    
    // 生成警告音 (0.1秒)
    generate_sine_wave(warning_sound_data, warning_sound_size, 1000.0, 0.1);
    
    // 生成旋律 (0.3秒)
    generate_melody(melody_data, melody_size);
    
    // 生成双音调报警音 (0.8秒)
    generate_alarm_tone(alarm_tone_data, alarm_tone_size);
    
    ESP_LOGI("AUDIO_DATA", "音频数据初始化完成");
} 