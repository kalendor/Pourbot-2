#include "ble_scale.h"

#include <math.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "host/ble_uuid.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "nvs.h"
#include "os/os_mbuf.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#define BOOKOO_SERVICE_UUID 0x0FFE
#define BOOKOO_WEIGHT_UUID  0xFF11
#define BOOKOO_COMMAND_UUID 0xFF12
#define BLE_CONN_NONE       BLE_HS_CONN_HANDLE_NONE

static const char *TAG = "ble_scale";
static uint16_t weight_value_handle;
static volatile uint16_t connection_handle = BLE_CONN_NONE;
static volatile bool notifications_enabled;
static volatile bool host_synced;
static volatile bool radio_enabled = true;
static volatile bool stack_started;
static volatile bool ota_stopped;
static volatile bool tare_requested;
static volatile int32_t weight_hundredths;
static volatile int16_t flow_hundredths;
static volatile uint8_t battery_level;
static volatile bool timer_running;
static int64_t timer_started_us;
static uint32_t timer_elapsed_ms;
static uint8_t own_addr_type;
static TaskHandle_t notify_task_handle;

static void advertise(void);

static uint8_t xor_checksum(const uint8_t *data, size_t length)
{
    uint8_t value = 0;
    for (size_t i = 0; i < length; ++i) value ^= data[i];
    return value;
}

static uint32_t scale_timer_ms(void)
{
    uint32_t elapsed = timer_elapsed_ms;
    if (timer_running) {
        int64_t delta = esp_timer_get_time() - timer_started_us;
        if (delta > 0) elapsed += (uint32_t)(delta / 1000);
    }
    return elapsed;
}

static void build_weight_packet(uint8_t packet[20])
{
    memset(packet, 0, 20);
    uint32_t milliseconds = scale_timer_ms() & 0xFFFFFFU;
    int32_t weight = weight_hundredths;
    int32_t flow = flow_hundredths;
    packet[0] = 0x03;
    packet[1] = 0x0B;
    packet[2] = (uint8_t)(milliseconds >> 16);
    packet[3] = (uint8_t)(milliseconds >> 8);
    packet[4] = (uint8_t)milliseconds;
    packet[5] = 0x00; /* grams */
    packet[6] = weight < 0 ? '-' : '+';
    if (weight < 0) weight = -weight;
    if (weight > 0xFFFFFF) weight = 0xFFFFFF;
    packet[7] = (uint8_t)((uint32_t)weight >> 16);
    packet[8] = (uint8_t)((uint32_t)weight >> 8);
    packet[9] = (uint8_t)weight;
    packet[10] = flow < 0 ? '-' : '+';
    if (flow < 0) flow = -flow;
    if (flow > UINT16_MAX) flow = UINT16_MAX;
    packet[11] = (uint8_t)((uint16_t)flow >> 8);
    packet[12] = (uint8_t)flow;
    packet[13] = battery_level;
    packet[14] = 0;
    packet[15] = 0; /* standby duration */
    packet[16] = 0; /* buzzer disabled */
    packet[17] = 1; /* flow smoothing enabled */
    packet[18] = 0;
    packet[19] = xor_checksum(packet, 19);
}

static void handle_command(const uint8_t *command, size_t length)
{
    if (length != 6 || command[0] != 0x03 || command[1] != 0x0A ||
        xor_checksum(command, 5) != command[5]) return;

    switch (command[2]) {
    case 0x01: /* tare */
        tare_requested = true;
        break;
    case 0x04: /* start timer */
        if (!timer_running) {
            timer_started_us = esp_timer_get_time();
            timer_running = true;
        }
        break;
    case 0x05: /* stop timer */
        if (timer_running) {
            timer_elapsed_ms = scale_timer_ms();
            timer_running = false;
        }
        break;
    case 0x06: /* reset timer */
        timer_running = false;
        timer_elapsed_ms = 0;
        break;
    case 0x07: /* tare and start timer */
        tare_requested = true;
        timer_elapsed_ms = 0;
        timer_started_us = esp_timer_get_time();
        timer_running = true;
        break;
    default:
        break;
    }
}

static int weight_access(uint16_t conn_handle, uint16_t attr_handle,
                         struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) return BLE_ATT_ERR_UNLIKELY;
    uint8_t packet[20];
    build_weight_packet(packet);
    return os_mbuf_append(ctxt->om, packet, sizeof(packet)) == 0
        ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int command_access(uint16_t conn_handle, uint16_t attr_handle,
                          struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return BLE_ATT_ERR_UNLIKELY;
    uint8_t command[20];
    uint16_t length = 0;
    if (ble_hs_mbuf_to_flat(ctxt->om, command, sizeof(command), &length) != 0)
        return BLE_ATT_ERR_UNLIKELY;
    handle_command(command, length);
    return 0;
}

static const struct ble_gatt_svc_def services[] = {{
    .type = BLE_GATT_SVC_TYPE_PRIMARY,
    .uuid = BLE_UUID16_DECLARE(BOOKOO_SERVICE_UUID),
    .characteristics = (struct ble_gatt_chr_def[]) {{
        .uuid = BLE_UUID16_DECLARE(BOOKOO_WEIGHT_UUID),
        .access_cb = weight_access,
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
        .val_handle = &weight_value_handle,
    }, {
        .uuid = BLE_UUID16_DECLARE(BOOKOO_COMMAND_UUID),
        .access_cb = command_access,
        .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
    }, {0}},
}, {0}};

static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            connection_handle = event->connect.conn_handle;
            ESP_LOGI(TAG, "Gaggiuino scale client connected");
        } else {
            advertise();
        }
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        connection_handle = BLE_CONN_NONE;
        notifications_enabled = false;
        if (radio_enabled) advertise();
        return 0;
    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == weight_value_handle)
            notifications_enabled = event->subscribe.cur_notify;
        return 0;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        if (radio_enabled && connection_handle == BLE_CONN_NONE) advertise();
        return 0;
    default:
        return 0;
    }
}

static void advertise(void)
{
    if (!host_synced || !radio_enabled || ota_stopped ||
        connection_handle != BLE_CONN_NONE ||
        ble_gap_adv_active()) return;
    struct ble_hs_adv_fields fields = {0};
    const char *name = ble_svc_gap_device_name();
    const ble_uuid16_t service = BLE_UUID16_INIT(BOOKOO_SERVICE_UUID);
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name = (const uint8_t *)name;
    fields.name_len = strlen(name);
    fields.name_is_complete = 1;
    fields.uuids16 = &service;
    fields.num_uuids16 = 1;
    fields.uuids16_is_complete = 1;
    if (ble_gap_adv_set_fields(&fields) != 0) return;
    struct ble_gap_adv_params params = {0};
    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER, &params,
                      gap_event, NULL);
}

static void on_sync(void)
{
    if (ble_hs_util_ensure_addr(0) != 0 ||
        ble_hs_id_infer_auto(0, &own_addr_type) != 0) return;
    host_synced = true;
    advertise();
}

static void host_task(void *argument)
{
    (void)argument;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static void notify_task(void *argument)
{
    (void)argument;
    while (true) {
        uint16_t conn = connection_handle;
        if (radio_enabled && notifications_enabled && conn != BLE_CONN_NONE) {
            uint8_t packet[20];
            build_weight_packet(packet);
            struct os_mbuf *om = ble_hs_mbuf_from_flat(packet, sizeof(packet));
            if (om) ble_gatts_notify_custom(conn, weight_value_handle, om);
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

static void load_enabled(void)
{
    nvs_handle_t handle;
    uint8_t saved = 1;
    if (nvs_open("ble_scale", NVS_READONLY, &handle) == ESP_OK) {
        if (nvs_get_u8(handle, "enabled", &saved) != ESP_OK) saved = 1;
        nvs_close(handle);
    }
    radio_enabled = saved != 0;
}

bool pourbot_ble_start(void)
{
    load_enabled();
    if (nimble_port_init() != ESP_OK) return false;
    stack_started = true;
    ota_stopped = false;
    ble_hs_cfg.sync_cb = on_sync;
    ble_svc_gap_init();
    ble_svc_gatt_init();
    if (ble_gatts_count_cfg(services) != 0 || ble_gatts_add_svcs(services) != 0)
        return false;
    /* Gaggiuino's tested Bookoo driver accepts the BOOKOO_SC prefix. This
     * follows the Themis Mini's documented "BOOKOO_SC xxxx" GAP format. */
    if (ble_svc_gap_device_name_set("BOOKOO_SC 0000") != 0) return false;
    nimble_port_freertos_init(host_task);
    xTaskCreate(notify_task, "ble_scale", 3072, NULL, 3,
                &notify_task_handle);
    return true;
}

void pourbot_ble_set_enabled(bool enabled)
{
    radio_enabled = enabled;
    nvs_handle_t handle;
    if (nvs_open("ble_scale", NVS_READWRITE, &handle) == ESP_OK) {
        nvs_set_u8(handle, "enabled", enabled ? 1 : 0);
        nvs_commit(handle);
        nvs_close(handle);
    }
    if (enabled) {
        advertise();
    } else {
        ble_gap_adv_stop();
        uint16_t conn = connection_handle;
        if (conn != BLE_CONN_NONE) ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
    }
}

bool pourbot_ble_enabled(void) { return radio_enabled; }
bool pourbot_ble_connected(void) { return connection_handle != BLE_CONN_NONE; }

bool pourbot_ble_take_tare_request(void)
{
    if (!tare_requested) return false;
    tare_requested = false;
    return true;
}

void pourbot_ble_update(float weight_g, float flow_gps, int battery_percent)
{
    if (!isfinite(weight_g)) weight_g = 0.0f;
    if (!isfinite(flow_gps)) flow_gps = 0.0f;
    if (weight_g > 167772.15f) weight_g = 167772.15f;
    if (weight_g < -167772.15f) weight_g = -167772.15f;
    if (flow_gps > 327.67f) flow_gps = 327.67f;
    if (flow_gps < -327.67f) flow_gps = -327.67f;
    weight_hundredths = (int32_t)lroundf(weight_g * 100.0f);
    flow_hundredths = (int16_t)lroundf(flow_gps * 100.0f);
    if (battery_percent >= 0 && battery_percent <= 100)
        battery_level = (uint8_t)battery_percent;
}

void pourbot_ble_prepare_sleep(void)
{
    ble_gap_adv_stop();
    uint16_t conn = connection_handle;
    if (conn != BLE_CONN_NONE) ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
}

bool pourbot_ble_stop_for_ota(void)
{
    if (!stack_started || ota_stopped) return true;
    ota_stopped = true;
    if (notify_task_handle) {
        vTaskDelete(notify_task_handle);
        notify_task_handle = NULL;
    }
    ble_gap_adv_stop();
    int rc = nimble_port_stop();
    if (rc != 0) {
        ESP_LOGE(TAG, "Could not stop BLE for OTA: %d", rc);
        return false;
    }
    esp_err_t err = nimble_port_deinit();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not release BLE memory for OTA: %s",
                 esp_err_to_name(err));
        return false;
    }
    stack_started = false;
    host_synced = false;
    notifications_enabled = false;
    connection_handle = BLE_CONN_NONE;
    ESP_LOGI(TAG, "BLE stopped and memory released for OTA");
    return true;
}

