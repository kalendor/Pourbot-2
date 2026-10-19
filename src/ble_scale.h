#pragma once

#include <stdbool.h>
#include <stdint.h>

bool pourbot_ble_start(void);
void pourbot_ble_set_enabled(bool enabled);
bool pourbot_ble_enabled(void);
bool pourbot_ble_connected(void);
bool pourbot_ble_take_tare_request(void);
void pourbot_ble_update(float weight_g, float flow_gps, int battery_percent);
void pourbot_ble_prepare_sleep(void);

