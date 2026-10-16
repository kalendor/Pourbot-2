#pragma once

#include <stdbool.h>
#include <stdint.h>

/* The NAU7802 shares controller 1 with the MAX17048. Both boards include
 * pull-ups and use different addresses (0x2A and 0x36). */
bool nau7802_init(void);
bool nau7802_reconnect(void);
bool nau7802_read(int32_t *value);
bool nau7802_average(int32_t *average, int samples);
void nau7802_power_down(void);

