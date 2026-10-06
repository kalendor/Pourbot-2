#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "driver/gpio.h"

#define BATTERY_I2C_SDA GPIO_NUM_7
#define BATTERY_I2C_SCL GPIO_NUM_15

/* Dedicated controller 1; never reconfigures the touchscreen controller 0. */
typedef struct {
    uint8_t percent;
    uint16_t millivolts;
    int16_t rate_tenths_per_hour;
    bool charging;
} battery_gauge_sample_t;

esp_err_t battery_gauge_init(void);
esp_err_t battery_gauge_read(battery_gauge_sample_t *sample);
