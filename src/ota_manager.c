#include "ota_manager.h"

#include <stdio.h>
#include <string.h>
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "wifi_manager.h"

#define OTA_URL "https://github.com/kalendor/Pourbot-2/releases/latest/download/firmware.bin"

static SemaphoreHandle_t ota_mutex;
static pourbot_ota_status_t ota_status;

/* Older builds accidentally embedded IDF's default version "1". */
static bool parse_version(const char *text, unsigned parts[3])
{
    if (*text == 'v') ++text;
    parts[0] = parts[1] = parts[2] = 0;
    for (int i = 0; i < 3; ++i) {
        if (*text < '0' || *text > '9') return false;
        while (*text >= '0' && *text <= '9') {
            if (parts[i] > 999999) return false;
            parts[i] = parts[i] * 10 + (unsigned)(*text++ - '0');
        }
        if (!*text) return i == 2 || i == 0;
        if (*text++ != '.' || i == 2) return false;
    }
    return false;
}

static void status_set(bool running, bool finished, bool ok, int progress,
                       const char *message)
{
    xSemaphoreTake(ota_mutex, portMAX_DELAY);
    ota_status.running = running;
    ota_status.finished = finished;
    ota_status.ok = ok;
    if (progress >= 0) ota_status.progress = progress > 100 ? 100 : progress;
    if (message) strlcpy(ota_status.message, message, sizeof(ota_status.message));
    xSemaphoreGive(ota_mutex);
}

static void ota_task(void *argument)
{
    const bool install = (bool)(uintptr_t)argument;
    pourbot_wifi_status_t wifi;
    pourbot_wifi_status(&wifi);
    if (!wifi.connected) {
        status_set(false, true, false, 0, "Connect to Wi-Fi before updating");
        vTaskDelete(NULL);
    }

    const esp_http_client_config_t http = {
        .url = OTA_URL,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 15000,
        .keep_alive_enable = true,
        .buffer_size = 4096,
        .buffer_size_tx = 1024,
    };
    const esp_https_ota_config_t config = { .http_config = &http };
    esp_https_ota_handle_t handle = NULL;
    status_set(true, false, false, 0, "Connecting to GitHub...");
    esp_err_t err = esp_https_ota_begin(&config, &handle);
    if (err != ESP_OK) {
        status_set(false, true, false, 0,
                   "Update unavailable - publish firmware.bin in the latest GitHub release");
        vTaskDelete(NULL);
    }

    esp_app_desc_t incoming = {0};
    err = esp_https_ota_get_img_desc(handle, &incoming);
    if (err != ESP_OK) {
        esp_https_ota_abort(handle);
        status_set(false, true, false, 0, "Could not read the update image");
        vTaskDelete(NULL);
    }
    xSemaphoreTake(ota_mutex, portMAX_DELAY);
    strlcpy(ota_status.available_version, incoming.version,
            sizeof(ota_status.available_version));
    xSemaphoreGive(ota_mutex);
    unsigned remote[3], current[3];
    if (!parse_version(incoming.version, remote) ||
        !parse_version(esp_app_get_description()->version, current)) {
        esp_https_ota_abort(handle);
        status_set(false, true, false, 0, "Cannot compare firmware versions");
        vTaskDelete(NULL);
    }
    int comparison = 0;
    for (int i = 0; i < 3; ++i) {
        if (remote[i] != current[i]) {
            comparison = remote[i] > current[i] ? 1 : -1;
            break;
        }
    }
    if (comparison <= 0) {
        esp_https_ota_abort(handle);
        xSemaphoreTake(ota_mutex, portMAX_DELAY);
        ota_status.update_available = false;
        xSemaphoreGive(ota_mutex);
        status_set(false, true, true, 0, comparison == 0 ?
                   "PourBot is already up to date" : "Installed version is newer than GitHub");
        vTaskDelete(NULL);
    }
    if (!install) {
        esp_https_ota_abort(handle);
        xSemaphoreTake(ota_mutex, portMAX_DELAY);
        ota_status.update_available = true;
        xSemaphoreGive(ota_mutex);
        status_set(false, true, true, 0, "New firmware is ready to install");
        vTaskDelete(NULL);
    }

    status_set(true, false, false, 1, "Downloading update - keep power connected");
    do {
        err = esp_https_ota_perform(handle);
        int total = esp_https_ota_get_image_size(handle);
        int received = esp_https_ota_get_image_len_read(handle);
        if (total > 0 && received >= 0)
            status_set(true, false, false, (int)((int64_t)received * 100 / total), NULL);
    } while (err == ESP_ERR_HTTPS_OTA_IN_PROGRESS);

    if (err != ESP_OK || !esp_https_ota_is_complete_data_received(handle)) {
        esp_https_ota_abort(handle);
        status_set(false, true, false, -1, "Download failed - current firmware is unchanged");
        vTaskDelete(NULL);
    }
    err = esp_https_ota_finish(handle);
    if (err != ESP_OK) {
        status_set(false, true, false, -1, "Update validation failed - current firmware is unchanged");
        vTaskDelete(NULL);
    }
    status_set(false, true, true, 100, "Update installed - restarting...");
    vTaskDelay(pdMS_TO_TICKS(2500));
    esp_restart();
}

static bool ota_start(bool install)
{
    if (!ota_mutex) {
        ota_mutex = xSemaphoreCreateMutex();
        if (!ota_mutex) return false;
        strlcpy(ota_status.current_version, esp_app_get_description()->version,
                sizeof(ota_status.current_version));
        strlcpy(ota_status.message, "Ready to check the latest GitHub release",
                sizeof(ota_status.message));
    }
    xSemaphoreTake(ota_mutex, portMAX_DELAY);
    bool busy = ota_status.running;
    bool available = ota_status.update_available;
    if (install && !available) busy = true;
    if (!busy) {
        ota_status.running = true;
        ota_status.finished = false;
        ota_status.ok = false;
        ota_status.progress = 0;
        if (!install) {
            ota_status.update_available = false;
            ota_status.available_version[0] = 0;
        }
    }
    xSemaphoreGive(ota_mutex);
    if (busy) return false;
    if (xTaskCreate(ota_task, "pour_ota", 8192, (void *)(uintptr_t)install, 2, NULL) != pdPASS) {
        status_set(false, true, false, 0, "Not enough memory to start update");
        return false;
    }
    return true;
}

bool pourbot_ota_check(void) { return ota_start(false); }
bool pourbot_ota_install(void) { return ota_start(true); }

void pourbot_ota_status(pourbot_ota_status_t *status)
{
    if (!status) return;
    if (!ota_mutex) {
        memset(status, 0, sizeof(*status));
        strlcpy(status->current_version, esp_app_get_description()->version,
                sizeof(status->current_version));
        strlcpy(status->message, "Ready to check the latest GitHub release",
                sizeof(status->message));
        return;
    }
    xSemaphoreTake(ota_mutex, portMAX_DELAY);
    *status = ota_status;
    xSemaphoreGive(ota_mutex);
}
