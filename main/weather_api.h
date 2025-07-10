#ifndef WEATHER_API_H
#define WEATHER_API_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 天气信息结构体 */
typedef struct {
    char province[32];      // 省份名
    char city[32];          // 城市名
    char adcode[16];        // 区域编码
    char weather[32];       // 天气现象（汉字描述）
    char temperature[8];    // 实时气温，单位：摄氏度
    char winddirection[16]; // 风向描述
    char windpower[8];      // 风力级别，单位：级
    char humidity[8];       // 空气湿度
    char reporttime[32];    // 数据发布的时间
} weather_info_t;

/* 初始化天气API客户端 */
esp_err_t weather_api_init(void);

/* 获取天气信息 */
esp_err_t weather_api_get_weather(const char* city_code, weather_info_t* weather_info);

/* 释放天气API客户端资源 */
void weather_api_deinit(void);

#ifdef __cplusplus
}
#endif

#endif // WEATHER_API_H 