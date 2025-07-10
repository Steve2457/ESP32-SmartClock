#ifndef DS3231_H
#define DS3231_H

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t second;
    uint8_t minute;
    uint8_t hour;
    uint8_t day_of_week;
    uint8_t date;
    uint8_t month;
    uint16_t year;
} ds3231_time_t;

esp_err_t ds3231_init(void);
esp_err_t ds3231_get_time(ds3231_time_t *time);
esp_err_t ds3231_set_time(const ds3231_time_t *time);

#ifdef __cplusplus
}
#endif

#endif /* DS3231_H */ 