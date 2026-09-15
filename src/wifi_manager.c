#include "wifi_manager.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"

typedef struct { bool scan; char ssid[33], password[65]; } wifi_command_t;
static QueueHandle_t commands;
static SemaphoreHandle_t mutex;
static pourbot_wifi_status_t state;
static char current_password[65];
static bool ignore_disconnect, stopped, sntp_started;
static unsigned retries;
static int64_t connection_started;

bool pourbot_time_valid(void) { return time(NULL) > 1700000000; }

static void event_handler(void *arg, esp_event_base_t base, int32_t event_id, void *data)
{
    (void)arg;
    xSemaphoreTake(mutex, portMAX_DELAY);
    if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        bool was_connected = state.connected;
        state.connected = false;
        state.ip[0] = 0;
        if (was_connected && !stopped && !ignore_disconnect) {
            state.connecting = true;
            retries = 0;
            connection_started = esp_timer_get_time();
        }
        if (!ignore_disconnect && !stopped && state.connecting && retries++ < 5) {
            if (esp_wifi_connect() != ESP_OK) state.connecting = false;
        } else if (!ignore_disconnect) {
            state.connecting = false;
            snprintf(state.message, sizeof(state.message), "Disconnected - select a network to retry");
        }
    } else if (base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP && !stopped) {
        ip_event_got_ip_t *ip = data;
        snprintf(state.ip, sizeof(state.ip), IPSTR, IP2STR(&ip->ip_info.ip));
        state.connected = true; state.connecting = false; retries = 0;
        snprintf(state.message, sizeof(state.message), "Connected - synchronizing time");
        nvs_handle_t nvs;
        if (nvs_open("pourbot", NVS_READWRITE, &nvs) == ESP_OK) {
            esp_err_t err = nvs_set_str(nvs, "wifi_ssid", state.ssid);
            if (err == ESP_OK) err = nvs_set_str(nvs, "wifi_pass", current_password);
            if (err == ESP_OK) err = nvs_commit(nvs);
            if (err != ESP_OK) snprintf(state.message, sizeof(state.message), "Connected; could not save credentials");
            nvs_close(nvs);
        }
        if (!sntp_started) {
            esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
            esp_sntp_setservername(0, "pool.ntp.org");
            esp_sntp_init();
            sntp_started = true;
        }
    }
    xSemaphoreGive(mutex);
}

static esp_err_t initialize(void)
{
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK) return err;
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    if (!esp_netif_create_default_wifi_sta()) return ESP_ERR_NO_MEM;
    wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&config);
    if (err != ESP_OK) return err;
    err = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, event_handler, NULL);
    if (err == ESP_OK) err = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, event_handler, NULL);
    /* Credentials are committed only after a successful connection. */
    if (err == ESP_OK) err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (err == ESP_OK) err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err == ESP_OK) err = esp_wifi_start();
    if (err == ESP_OK) err = esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    return err;
}

static void connect_now(const wifi_command_t *command)
{
    xSemaphoreTake(mutex, portMAX_DELAY);
    ignore_disconnect = true;
    state.connecting = false;
    xSemaphoreGive(mutex);
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(250));
    wifi_config_t config = {0};
    memcpy(config.sta.ssid, command->ssid, strlen(command->ssid));
    memcpy(config.sta.password, command->password, strlen(command->password));
    config.sta.pmf_cfg.capable = true;
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &config);
    xSemaphoreTake(mutex, portMAX_DELAY);
    strlcpy(state.ssid, command->ssid, sizeof(state.ssid));
    strlcpy(current_password, command->password, sizeof(current_password));
    state.connected = false; state.connecting = err == ESP_OK;
    ignore_disconnect = false; retries = 0; connection_started = esp_timer_get_time();
    snprintf(state.message, sizeof(state.message), "Connecting to %s", command->ssid);
    if (err == ESP_OK) err = esp_wifi_connect();
    if (err != ESP_OK) {
        state.connecting = false;
        snprintf(state.message, sizeof(state.message), "Connect failed: %s", esp_err_to_name(err));
    }
    xSemaphoreGive(mutex);
}

static void scan_now(void)
{
    xSemaphoreTake(mutex, portMAX_DELAY);
    bool connecting = state.connecting;
    state.scanning = !connecting;
    snprintf(state.message, sizeof(state.message), connecting ? "Connecting - wait before scanning" : "Scanning nearby networks...");
    xSemaphoreGive(mutex);
    esp_err_t err = connecting ? ESP_ERR_INVALID_STATE : esp_wifi_scan_start(NULL, true);
    wifi_ap_record_t records[POURBOT_WIFI_NETWORKS];
    uint16_t count = POURBOT_WIFI_NETWORKS;
    if (err == ESP_OK) err = esp_wifi_scan_get_ap_records(&count, records);
    xSemaphoreTake(mutex, portMAX_DELAY);
    state.scanning = false;
    if (err == ESP_OK) {
        state.count = 0;
        for (unsigned i = 0; i < count; ++i) {
            if (!records[i].ssid[0]) continue;
            unsigned j;
            for (j = 0; j < state.count; ++j)
                if (!strcmp(state.networks[j].ssid, (char *)records[i].ssid)) break;
            if (j < state.count) continue;
            pourbot_network_t *n = &state.networks[state.count++];
            strlcpy(n->ssid, (char *)records[i].ssid, sizeof(n->ssid));
            n->rssi = records[i].rssi; n->secure = records[i].authmode != WIFI_AUTH_OPEN;
        }
        snprintf(state.message, sizeof(state.message), "Select a network (%u found)", state.count);
    } else {
        snprintf(state.message, sizeof(state.message), connecting ? "Connecting - tap scan later" : "Scan failed - tap scan to retry");
    }
    state.scan_generation++;
    xSemaphoreGive(mutex);
}

static void wifi_task(void *arg)
{
    (void)arg;
    esp_err_t err = initialize();
    xSemaphoreTake(mutex, portMAX_DELAY);
    state.ready = err == ESP_OK;
    snprintf(state.message, sizeof(state.message), err == ESP_OK ? "Wi-Fi ready - tap SCAN" : "Wi-Fi startup failed: %s", esp_err_to_name(err));
    xSemaphoreGive(mutex);
    if (err != ESP_OK) { vTaskDelete(NULL); return; }
    wifi_command_t command = {0};
    nvs_handle_t nvs;
    if (nvs_open("pourbot", NVS_READONLY, &nvs) == ESP_OK) {
        size_t ss = sizeof(command.ssid), ps = sizeof(command.password);
        if (nvs_get_str(nvs, "wifi_ssid", command.ssid, &ss) == ESP_OK)
            nvs_get_str(nvs, "wifi_pass", command.password, &ps);
        nvs_close(nvs);
    }
    if (command.ssid[0]) connect_now(&command);
    memset(&command, 0, sizeof(command));
    while (true) {
        if (xQueueReceive(commands, &command, pdMS_TO_TICKS(1000))) {
            xSemaphoreTake(mutex, portMAX_DELAY);
            bool running = !stopped;
            xSemaphoreGive(mutex);
            if (running) { if (command.scan) scan_now(); else connect_now(&command); }
            memset(&command, 0, sizeof(command));
        }
        xSemaphoreTake(mutex, portMAX_DELAY);
        bool expired = state.connecting && esp_timer_get_time() - connection_started > 30000000;
        if (expired) {
            state.connecting = false; ignore_disconnect = true;
            snprintf(state.message, sizeof(state.message), "Connection timed out - check password and retry");
        }
        xSemaphoreGive(mutex);
        if (expired) esp_wifi_disconnect();
    }
}

bool pourbot_wifi_start(void)
{
    mutex = xSemaphoreCreateMutex();
    commands = xQueueCreate(2, sizeof(wifi_command_t));
    return mutex && commands && xTaskCreate(wifi_task, "wifi_setup", 6144, NULL, 2, NULL) == pdPASS;
}
bool pourbot_wifi_scan(void)
{
    pourbot_wifi_status_t s; pourbot_wifi_status(&s);
    wifi_command_t c = {.scan = true};
    return s.ready && !s.scanning && commands && xQueueSend(commands, &c, 0) == pdTRUE;
}
bool pourbot_wifi_connect(const char *ssid, const char *password)
{
    if (!ssid || !password || !ssid[0] || strlen(ssid) > 32 || strlen(password) > 64) return false;
    pourbot_wifi_status_t s; pourbot_wifi_status(&s);
    if (!s.ready || s.scanning) return false;
    wifi_command_t c = {0};
    strlcpy(c.ssid, ssid, sizeof(c.ssid)); strlcpy(c.password, password, sizeof(c.password));
    return commands && xQueueSend(commands, &c, 0) == pdTRUE;
}
void pourbot_wifi_status(pourbot_wifi_status_t *status)
{
    if (!mutex) { memset(status, 0, sizeof(*status)); return; }
    xSemaphoreTake(mutex, portMAX_DELAY); *status = state; xSemaphoreGive(mutex);
}
void pourbot_wifi_stop(void)
{
    if (!mutex) return;
    xSemaphoreTake(mutex, portMAX_DELAY);
    stopped = true; ignore_disconnect = true;
    bool ready = state.ready;
    xSemaphoreGive(mutex);
    if (ready) esp_wifi_stop();
}
