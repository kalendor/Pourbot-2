#pragma once
#include <stdbool.h>
#include <stdint.h>
#define POURBOT_WIFI_NETWORKS 12
typedef struct { char ssid[33]; int8_t rssi; bool secure; } pourbot_network_t;
typedef struct {
    bool ready, connected, connecting, scanning;
    char ssid[33], ip[16], message[96];
    uint32_t scan_generation;
    uint8_t count;
    pourbot_network_t networks[POURBOT_WIFI_NETWORKS];
} pourbot_wifi_status_t;
bool pourbot_wifi_start(void);
bool pourbot_wifi_scan(void);
bool pourbot_wifi_connect(const char *ssid, const char *password);
void pourbot_wifi_status(pourbot_wifi_status_t *status);
void pourbot_wifi_stop(void);
bool pourbot_time_valid(void);
