#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool running;
    bool finished;
    bool ok;
    bool update_available;
    uint8_t progress;
    char current_version[32];
    char available_version[32];
    char message[128];
} pourbot_ota_status_t;

bool pourbot_ota_check(void);
bool pourbot_ota_install(void);
void pourbot_ota_status(pourbot_ota_status_t *status);
