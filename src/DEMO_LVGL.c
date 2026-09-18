#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "display.h"
#include "battery_gauge.h"
#include "pour_archive.h"
#include "ota_manager.h"
#include "wifi_manager.h"
#include "esp_heap_caps.h"
#include "esp_bsp.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_rom_sys.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
LV_FONT_DECLARE(font_weight_72);
#include "nvs.h"
#include "nvs_flash.h"
#include "recipes.h"
#include "splash_logo.h"

#define HX711_DOUT GPIO_NUM_17
#define HX711_SCK  GPIO_NUM_18
#define STANDBY_WAKE_GPIO GPIO_NUM_6
#define FLOW_OPTIMAL_MIN_GPS 3.0f
#define FLOW_OPTIMAL_MAX_GPS 6.0f

static const char *TAG = "pourbot";
static lv_obj_t *weight_label;
static lv_obj_t *weight_unit_label;
static lv_obj_t *status_label;
static lv_obj_t *tare_button;
static lv_obj_t *tare_button_label;
static lv_obj_t *progress_panel;
static lv_obj_t *timer_label;
static lv_obj_t *flow_label;
static lv_obj_t *flow_bar;
static lv_obj_t *brew_button_label;
/* The former live-chart page is no longer created. These pointers keep its
 * builder self-contained while the dashboard uses the active chart below. */
static lv_obj_t *weight_chart;
static lv_chart_series_t *weight_chart_series;
static lv_chart_series_t *flow_chart_series;
static lv_obj_t *chart_time_labels[4];
static lv_obj_t *chart_weight_labels[5];
static lv_obj_t *recipe_title_label;
static lv_obj_t *recipe_dose_value_label;
static lv_obj_t *recipe_target_value_label;
static lv_obj_t *main_screen;
static lv_obj_t *page_tileview;
static lv_obj_t *graph_screen;
static lv_obj_t *graph_recipe_label;
static lv_obj_t *graph_weight_label;
static lv_obj_t *graph_flow_label;
static lv_obj_t *graph_timer_label;
static lv_obj_t *graph_brew_button_label;
static lv_obj_t *dashboard_screen;
static lv_obj_t *dashboard_recipe_label;
static lv_obj_t *dashboard_recipe_detail_label;
static lv_obj_t *dashboard_weight_label;
static lv_obj_t *dashboard_flow_label;
static lv_obj_t *dashboard_timer_label;
static lv_obj_t *dashboard_target_label;
static lv_obj_t *dashboard_current_label;
static lv_obj_t *dashboard_remaining_label;
static lv_obj_t *dashboard_average_label;
static lv_obj_t *dashboard_chart;
static lv_chart_series_t *dashboard_weight_series;
static lv_chart_series_t *dashboard_flow_series;
static lv_obj_t *dashboard_time_labels[4];
static lv_obj_t *dashboard_weight_labels[5];
static lv_obj_t *dashboard_brew_button_label;
static lv_obj_t *main_dose_card;
static lv_obj_t *main_target_card;
static lv_obj_t *main_weight_group;
static lv_obj_t *main_brew_button;
static lv_obj_t *recipe_edit_values[4];
static lv_obj_t *menu_overlay;
static lv_obj_t *calibration_status_label;
static lv_obj_t *calibration_weight_button;
static lv_obj_t *calibration_tare_button;
static lv_obj_t *calibration_run_button;
static lv_obj_t *calibration_weight_value;
static lv_obj_t *calibration_keypad;
static lv_obj_t *calibration_number_input;
static volatile bool standby_requested;
static volatile bool standby_active;
static volatile bool tare_requested = true;
static bool boot_scale_ready;
static bool brew_running;
static int64_t brew_started_us;
static int64_t brew_elapsed_us;
static bool main_brew_metrics_visible;
static int32_t tare_offset;
static float calibration_factor;
static volatile bool calibration_tare_requested;
static volatile bool calibration_run_requested;
static volatile int32_t calibration_known_grams = 100;
static pourbot_recipe_t recipe_edit;
static uint8_t recipe_edit_index;
static int32_t stable_weight_tenths = INT32_MIN;
static int32_t pending_weight_tenths = INT32_MIN;
static int64_t pending_weight_since_us;
static volatile bool scale_is_moving;
static volatile float scale_flow_gps;
static portMUX_TYPE hx711_mux = portMUX_INITIALIZER_UNLOCKED;
static lv_timer_t *overlay_timer;
static lv_obj_t *wifi_status_label, *wifi_network_list, *wifi_connect_button;
static lv_obj_t *wifi_password_area;
static char wifi_selected_ssid[33];
static uint32_t wifi_scan_generation;
static pourbot_wifi_status_t wifi_ui_status;
static lv_obj_t *analytics_body, *analytics_status_label;
static lv_obj_t *ota_status_label, *ota_progress_bar, *ota_install_button;
static lv_obj_t *ota_check_button, *ota_version_table;
static pour_archive_result_t *analytics_result;
static uint32_t analytics_generation;
static bool analytics_detail_open;
static lv_obj_t *analytics_delete_popup;
static char analytics_delete_filename[POUR_ARCHIVE_NAME_SIZE];
static bool analytics_long_press_handled;
static bool archive_ready;
static time_t brew_epoch;
static pourbot_recipe_t brew_recipe;
static uint16_t last_saved_sample_count;
static uint32_t last_saved_sample_time;
static int32_t last_saved_sample_weight;
static bool current_pour_saved;
static void wifi_event_cb(lv_event_t *event);
static void analytics_event_cb(lv_event_t *event);
static void settings_event_cb(lv_event_t *event);
static void archive_current_pour(void);

/* All battery UI state belongs to the LVGL mutex, including rebuilt recipe widgets. */
static lv_obj_t *battery_labels[2];
static int battery_percent = -1;

static void render_battery_label(lv_obj_t *label)
{
    if (!label) return;
    if (battery_percent < 0) {
        lv_label_set_text(label, LV_SYMBOL_BATTERY_EMPTY " --%");
        lv_obj_set_style_text_color(label, lv_color_hex(0x94A3B8), 0);
        return;
    }
    const char *symbol = battery_percent >= 88 ? LV_SYMBOL_BATTERY_FULL :
        battery_percent >= 63 ? LV_SYMBOL_BATTERY_3 :
        battery_percent >= 38 ? LV_SYMBOL_BATTERY_2 :
        battery_percent >= 13 ? LV_SYMBOL_BATTERY_1 : LV_SYMBOL_BATTERY_EMPTY;
    lv_label_set_text_fmt(label, "%s %d%%", symbol, battery_percent);
    lv_obj_set_style_text_color(label, lv_color_hex(battery_percent <= 10
        ? 0xEF4444 : battery_percent <= 20 ? 0xF2B94F : 0x4ADE80), 0);
}

static void add_battery_label(lv_obj_t *screen, unsigned index)
{
    battery_labels[index] = lv_label_create(screen);
    lv_obj_set_style_text_font(battery_labels[index], &lv_font_montserrat_14, 0);
    lv_obj_align(battery_labels[index], LV_ALIGN_TOP_RIGHT, -12, 10);
    render_battery_label(battery_labels[index]);
}

static void battery_task(void *argument)
{
    (void)argument;
    if (battery_gauge_init() != ESP_OK) {
        ESP_LOGW(TAG, "Battery I2C initialization failed; gauge unavailable");
        vTaskDelete(NULL);
        return;
    }
    bool warned = false;
    while (!standby_active) {
        uint8_t percent;
        uint16_t millivolts;
        /* I2C never runs in the scale task or while holding the LVGL mutex. */
        esp_err_t result = battery_gauge_read(&percent, &millivolts);
        if (result != ESP_OK && !warned) {
            ESP_LOGW(TAG, "MAX17048 unavailable on GPIO7/15: %s", esp_err_to_name(result));
            warned = true;
        } else if (result == ESP_OK) {
            warned = false;
        }
        if (!standby_active && bsp_display_lock(100)) {
            const int next = result == ESP_OK ? percent : -1;
            if (next != battery_percent) {
                battery_percent = next;
                for (unsigned i = 0; i < 2; ++i) render_battery_label(battery_labels[i]);
            }
            bsp_display_unlock();
        }
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
    vTaskDelete(NULL);
}

static void set_brew_labels(const char *text)
{
    if (brew_button_label) lv_label_set_text(brew_button_label, text);
    if (dashboard_brew_button_label) lv_label_set_text(dashboard_brew_button_label, text);
}

#define CHART_HISTORY_MAX 1200
static int32_t chart_weight_history[CHART_HISTORY_MAX];
static int32_t chart_flow_history[CHART_HISTORY_MAX];
static uint32_t chart_time_history[CHART_HISTORY_MAX];
static int64_t chart_sample_interval_us = 500000;
static int32_t chart_weight_max = 500;
static int32_t chart_flow_max = 100;
static uint16_t chart_history_count;
static bool chart_recording;
static int64_t chart_last_sample_us;
static int64_t chart_start_elapsed_us;
static int64_t last_ui_draw_us;

/* Caller holds LVGL mutex: copy chart data into a PSRAM job, never write SD here. */
static void archive_current_pour(void)
{
    if (!chart_history_count) {
        if (analytics_status_label) lv_label_set_text(analytics_status_label, "No recorded pour to save");
        return;
    }
    /* The periodic chart sample can trail a pause/reset by up to 500 ms.
     * Capture the actual endpoint before copying the session to the SD job so
     * FINAL and the recorded duration include the complete pour. */
    int64_t elapsed = brew_elapsed_us;
    if (brew_running) elapsed += esp_timer_get_time() - brew_started_us;
    uint32_t terminal_seconds = elapsed > chart_start_elapsed_us
        ? (uint32_t)((elapsed - chart_start_elapsed_us) / 1000000) : 0;
    int32_t terminal_weight = stable_weight_tenths == INT32_MIN ? 0
        : (stable_weight_tenths >= 0 ? stable_weight_tenths + 5 : stable_weight_tenths - 5) / 10;
    int32_t terminal_flow = (int32_t)(scale_flow_gps * 10.0f + 0.5f);
    if (terminal_weight < 0) terminal_weight = 0;
    if (terminal_flow < 0) terminal_flow = 0;
    unsigned endpoint = chart_history_count;
    if (endpoint >= CHART_HISTORY_MAX ||
        chart_time_history[chart_history_count - 1] == terminal_seconds) {
        endpoint = chart_history_count - 1;
    } else {
        chart_history_count++;
    }
    chart_time_history[endpoint] = terminal_seconds;
    chart_weight_history[endpoint] = terminal_weight;
    chart_flow_history[endpoint] = terminal_flow;

    unsigned last = chart_history_count - 1;
    if (current_pour_saved && last_saved_sample_count == chart_history_count &&
        last_saved_sample_time == chart_time_history[last] &&
        last_saved_sample_weight == chart_weight_history[last]) {
        if (analytics_status_label) lv_label_set_text(analytics_status_label, "Current pour is already saved");
        return;
    }
    time_t epoch = brew_epoch;
    if (!epoch && pourbot_time_valid()) {
        epoch = time(NULL) - elapsed / 1000000;
    }
    bool queued = archive_ready && pour_archive_save(&brew_recipe, epoch, chart_history_count,
        chart_time_history, chart_weight_history, chart_flow_history);
    if (queued) {
        current_pour_saved = true;
        last_saved_sample_count = chart_history_count;
        last_saved_sample_time = chart_time_history[last];
        last_saved_sample_weight = chart_weight_history[last];
    }
    if (analytics_status_label) lv_label_set_text(analytics_status_label,
        queued ? "Saving to SD..." : "Could not queue save - try again");
    if (!queued) ESP_LOGW(TAG, "Pour was not queued for SD storage");
}

static void format_fixed(char *buffer, size_t size, float value, uint32_t decimals)
{
    const int32_t scale = decimals == 2 ? 100 : 10;
    const float scaled_value = value * (float)scale;
    const int32_t scaled = (int32_t)(scaled_value >= 0.0f ? scaled_value + 0.5f
                                                          : scaled_value - 0.5f);
    const uint32_t magnitude = scaled < 0 ? (uint32_t)(-(int64_t)scaled)
                                          : (uint32_t)scaled;
    snprintf(buffer, size, "%s%lu.%0*lu", scaled < 0 ? "-" : "",
             (unsigned long)(magnitude / scale), (int)decimals,
             (unsigned long)(magnitude % scale));
}

static lv_obj_t *make_panel(lv_obj_t *parent, lv_coord_t width, lv_coord_t height,
                            uint32_t color);

static bool hx711_wait_ready(uint32_t timeout_ms)
{
    TickType_t start = xTaskGetTickCount();
    TickType_t timeout = pdMS_TO_TICKS(timeout_ms);

    while (gpio_get_level(HX711_DOUT)) {
        if ((xTaskGetTickCount() - start) >= timeout) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    return true;
}

static bool hx711_read(int32_t *value)
{
    if (!hx711_wait_ready(250)) {
        return false;
    }

    uint32_t data = 0;
    portENTER_CRITICAL(&hx711_mux);
    for (int bit = 0; bit < 24; ++bit) {
        gpio_set_level(HX711_SCK, 1);
        esp_rom_delay_us(1);
        data = (data << 1) | (uint32_t)gpio_get_level(HX711_DOUT);
        gpio_set_level(HX711_SCK, 0);
        esp_rom_delay_us(1);
    }

    // 25th pulse selects channel A, gain 128, for the next conversion.
    gpio_set_level(HX711_SCK, 1);
    esp_rom_delay_us(1);
    gpio_set_level(HX711_SCK, 0);
    esp_rom_delay_us(1);
    portEXIT_CRITICAL(&hx711_mux);

    if (data & 0x00800000U) {
        data |= 0xFF000000U;
    }
    *value = (int32_t)data;
    return true;
}

static bool hx711_average(int32_t *average, int samples)
{
    int32_t readings[32];
    int valid = 0;

    /* Flush conversions captured before the user requested tare/calibration.
     * Only subsequent fresh readings establish the new reference. */
    for (int i = 0; i < 2; ++i) {
        int32_t discarded;
        if (!hx711_read(&discarded)) return false;
    }

    if (samples > (int)(sizeof(readings) / sizeof(readings[0]))) {
        samples = sizeof(readings) / sizeof(readings[0]);
    }

    for (int i = 0; i < samples; ++i) {
        int32_t reading;
        if (hx711_read(&reading)) {
            readings[valid++] = reading;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }

    /* A timeout-heavy acquisition is not a reliable zero reference. */
    if (valid < (samples * 3 + 3) / 4) {
        return false;
    }

    /* A trimmed mean prevents one noisy HX711 conversion from shifting the
     * zero point. With enough samples, discard the outer quarter at each end. */
    for (int i = 1; i < valid; ++i) {
        int32_t value = readings[i];
        int j = i - 1;
        while (j >= 0 && readings[j] > value) {
            readings[j + 1] = readings[j];
            --j;
        }
        readings[j + 1] = value;
    }
    int trim = valid >= 8 ? valid / 4 : 0;
    int64_t total = 0;
    for (int i = trim; i < valid - trim; ++i) total += readings[i];
    *average = (int32_t)(total / (valid - trim * 2));
    return true;
}

static int32_t median3(int32_t a, int32_t b, int32_t c)
{
    if (a > b) { int32_t t = a; a = b; b = t; }
    if (b > c) { int32_t t = b; b = c; c = t; }
    if (a > b) { int32_t t = a; a = b; b = t; }
    return b;
}

static void save_calibration_factor(void)
{
    nvs_handle_t handle;
    if (nvs_open("pourbot", NVS_READWRITE, &handle) != ESP_OK) return;
    if (nvs_set_blob(handle, "cal_factor", &calibration_factor,
                     sizeof(calibration_factor)) == ESP_OK) {
        nvs_commit(handle);
    }
    nvs_close(handle);
}

static void load_calibration_factor(void)
{
    nvs_handle_t handle;
    size_t size = sizeof(calibration_factor);
    if (nvs_open("pourbot", NVS_READONLY, &handle) != ESP_OK) return;
    if (nvs_get_blob(handle, "cal_factor", &calibration_factor, &size) != ESP_OK ||
        size != sizeof(calibration_factor)) {
        calibration_factor = 0.0f;
    }
    nvs_close(handle);
}

static void tare_event_cb(lv_event_t *event)
{
    (void)event;
    stable_weight_tenths = INT32_MIN;
    tare_requested = true;
    lv_label_set_text(tare_button_label, "TARING...");
    lv_obj_add_state(tare_button, LV_STATE_DISABLED);
}

static void request_brew_tare(void)
{
    stable_weight_tenths = INT32_MIN;
    tare_requested = true;
    /* Starting/resetting a brew still tares the scale, but automatic tare is
     * intentionally silent so the manual TARE control does not flash or look
     * pressed when the user touches START. */
}

static void format_chart_time(char *buffer, size_t size, uint32_t seconds)
{
    snprintf(buffer, size, "%lu:%02lu", (unsigned long)(seconds / 60),
             (unsigned long)(seconds % 60));
}

static void chart_session_reset(void)
{
    chart_history_count = 0;
    chart_sample_interval_us = 500000;
    chart_weight_max = 500;
    chart_flow_max = 100;
    chart_recording = false;
    chart_last_sample_us = 0;
    chart_start_elapsed_us = 0;
    if (dashboard_chart && dashboard_weight_series && dashboard_flow_series) {
        lv_chart_set_range(dashboard_chart, LV_CHART_AXIS_PRIMARY_Y, 0, chart_weight_max);
        lv_chart_set_range(dashboard_chart, LV_CHART_AXIS_SECONDARY_Y, 0, chart_flow_max);
        lv_chart_set_all_value(dashboard_chart, dashboard_weight_series, LV_CHART_POINT_NONE);
        lv_chart_set_all_value(dashboard_chart, dashboard_flow_series, LV_CHART_POINT_NONE);
        lv_chart_refresh(dashboard_chart);
    }
    for (int i = 0; i < 4; ++i) {
        if (dashboard_time_labels[i]) lv_label_set_text(dashboard_time_labels[i], "0:00");
    }
    for (int i = 0; i < 5; ++i) {
        if (dashboard_weight_labels[i]) lv_label_set_text_fmt(dashboard_weight_labels[i], "%ld",
            (long)(chart_weight_max * (4 - i) / 4));
    }
}

static void chart_render_one(lv_obj_t *chart, lv_chart_series_t *weight_series,
                             lv_chart_series_t *flow_series, lv_obj_t **time_labels,
                             lv_obj_t **weight_labels, uint32_t elapsed_seconds)
{
    if (!chart || !weight_series || !flow_series || chart_history_count == 0) return;
    lv_chart_set_range(chart, LV_CHART_AXIS_PRIMARY_Y, 0, chart_weight_max);
    lv_chart_set_range(chart, LV_CHART_AXIS_SECONDARY_Y, 0, chart_flow_max);
    for (int i = 0; i < 5; ++i) {
        if (weight_labels[i]) lv_label_set_text_fmt(weight_labels[i], "%ld",
            (long)(chart_weight_max * (4 - i) / 4));
    }
    uint16_t history_index = 0;
    for (uint16_t point = 0; point < 60; ++point) {
        const uint32_t point_time = (uint32_t)((uint64_t)elapsed_seconds * point / 59);
        while (history_index + 1 < chart_history_count &&
               chart_time_history[history_index + 1] <= point_time) history_index++;
        lv_chart_set_value_by_id(chart, weight_series, point,
                                 chart_weight_history[history_index]);
        lv_chart_set_value_by_id(chart, flow_series, point,
                                 chart_flow_history[history_index]);
    }
    char time_text[12];
    for (int i = 0; i < 4; ++i) {
        format_chart_time(time_text, sizeof(time_text),
                          (uint32_t)(((uint64_t)elapsed_seconds * i) / 3));
        if (time_labels[i]) lv_label_set_text(time_labels[i], time_text);
    }
    lv_chart_refresh(chart);
}

static void chart_render(uint32_t elapsed_seconds)
{
    chart_render_one(dashboard_chart, dashboard_weight_series, dashboard_flow_series,
                     dashboard_time_labels, dashboard_weight_labels, elapsed_seconds);
}

static void set_main_brew_metrics_visible(bool visible)
{
    if (!timer_label || !flow_label || !flow_bar || !progress_panel ||
        main_brew_metrics_visible == visible) {
        return;
    }
    main_brew_metrics_visible = visible;
    if (main_weight_group && main_dose_card && main_target_card) {
        lv_obj_t *details[] = {recipe_title_label,
                              main_dose_card, main_target_card};
        for (unsigned i = 0; i < sizeof(details) / sizeof(details[0]); ++i) {
            if (visible) lv_obj_clear_flag(details[i], LV_OBJ_FLAG_HIDDEN);
            else lv_obj_add_flag(details[i], LV_OBJ_FLAG_HIDDEN);
        }
        lv_obj_set_size(progress_panel, visible ? 200 : 360, visible ? 200 : 360);
        lv_obj_align(progress_panel, LV_ALIGN_TOP_LEFT, visible ? 205 : 60,
                     visible ? 48 : 50);
        lv_obj_set_width(main_weight_group, visible ? 250 : 360);
        lv_obj_set_height(main_weight_group, visible ? 66 : 96);
        lv_obj_set_style_text_font(weight_label, visible ? &lv_font_montserrat_48 : &font_weight_72, 0);
        lv_obj_align(main_weight_group, LV_ALIGN_TOP_LEFT, visible ? 180 : 60,
                     visible ? 82 : 108);
        if (main_brew_button && tare_button) {
            lv_obj_align(tare_button, LV_ALIGN_BOTTOM_LEFT, visible ? 150 : 77, -9);
            lv_obj_align(main_brew_button, LV_ALIGN_BOTTOM_RIGHT, visible ? -12 : -77, -9);
        }
    }
    if (visible) {
        lv_obj_clear_flag(progress_panel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(timer_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(flow_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(flow_bar, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_arc_opa(progress_panel, LV_OPA_COVER, LV_PART_INDICATOR);
    } else {
        lv_obj_add_flag(progress_panel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(timer_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(flow_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(flow_bar, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_arc_opa(progress_panel, LV_OPA_TRANSP, LV_PART_INDICATOR);
    }
}

static void brew_timer_display_cb(lv_timer_t *timer)
{
    (void)timer;
    if (!timer_label) return;

    set_main_brew_metrics_visible(brew_running || brew_elapsed_us > 0);

    int64_t elapsed = brew_elapsed_us;
    if (brew_running) elapsed += esp_timer_get_time() - brew_started_us;
    const uint32_t elapsed_ms = (uint32_t)(elapsed / 1000);
    lv_label_set_text_fmt(timer_label, "%02lu:%02lu.%lu",
                          (unsigned long)(elapsed_ms / 60000),
                          (unsigned long)((elapsed_ms / 1000) % 60),
                          (unsigned long)((elapsed_ms / 100) % 10));
    if (dashboard_timer_label) {
        lv_label_set_text_fmt(dashboard_timer_label, "%02lu:%02lu.%lu",
                              (unsigned long)(elapsed_ms / 60000),
                              (unsigned long)((elapsed_ms / 1000) % 60),
                              (unsigned long)((elapsed_ms / 100) % 10));
    }
}

static void brew_event_cb(lv_event_t *event)
{
    const lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_LONG_PRESSED) {
        archive_current_pour();
        brew_running = false;
        brew_elapsed_us = 0;
        brew_started_us = 0;
        request_brew_tare();
        chart_session_reset();
        set_brew_labels(LV_SYMBOL_PLAY "  START");
        brew_timer_display_cb(NULL);
        return;
    }
    if (code != LV_EVENT_SHORT_CLICKED) return;

    const int64_t now = esp_timer_get_time();
    if (brew_running) {
        brew_elapsed_us += now - brew_started_us;
        brew_running = false;
        set_brew_labels(LV_SYMBOL_PLAY "  RESUME");
    } else {
        if (brew_elapsed_us == 0) {
            brew_epoch = pourbot_time_valid() ? time(NULL) : 0;
            brew_recipe = *pourbot_recipe_active();
            current_pour_saved = false;
            request_brew_tare();
        }
        brew_started_us = now;
        brew_running = true;
        set_brew_labels(LV_SYMBOL_PAUSE "  PAUSE");
    }
}

static void close_overlay(void)
{
    if (overlay_timer) { lv_timer_del(overlay_timer); overlay_timer = NULL; }
    wifi_status_label = NULL;
    wifi_network_list = NULL;
    wifi_password_area = NULL;
    wifi_connect_button = NULL;
    analytics_body = NULL;
    analytics_status_label = NULL;
    ota_status_label = NULL;
    ota_progress_bar = NULL;
    ota_install_button = NULL;
    ota_check_button = NULL;
    ota_version_table = NULL;
    if (menu_overlay) {
        calibration_status_label = NULL;
        calibration_weight_button = NULL;
        calibration_tare_button = NULL;
        calibration_run_button = NULL;
        calibration_weight_value = NULL;
        calibration_keypad = NULL;
        calibration_number_input = NULL;
        lv_obj_del(menu_overlay);
        menu_overlay = NULL;
    }
}

static void close_overlay_event_cb(lv_event_t *event)
{
    (void)event;
    close_overlay();
}

static lv_obj_t *overlay_button(lv_obj_t *parent, const char *text,
                                lv_coord_t x, lv_coord_t y,
                                lv_event_cb_t callback, void *user_data)
{
    lv_obj_t *button = lv_btn_create(parent);
    lv_obj_set_size(button, 218, 64);
    lv_obj_align(button, LV_ALIGN_TOP_LEFT, x, y);
    lv_obj_set_style_radius(button, 14, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x111827), 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x1F2937), LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(button, 0, 0);
    lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, user_data);
    lv_obj_t *label = lv_label_create(button);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(0xF8FAFC), 0);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 4, 0);
    return button;
}

static lv_obj_t *small_action_button(lv_obj_t *parent, const char *text,
                                     lv_coord_t x, lv_coord_t y, lv_coord_t width,
                                     uint32_t color, lv_event_cb_t callback)
{
    lv_obj_t *button = lv_btn_create(parent);
    lv_obj_set_size(button, width, 44);
    lv_obj_align(button, LV_ALIGN_TOP_LEFT, x, y);
    lv_obj_set_style_radius(button, 13, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(color), 0);
    lv_obj_set_style_shadow_width(button, 0, 0);
    lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, NULL);
    lv_obj_t *label = lv_label_create(button);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(label);
    return button;
}

static void quick_reboot_event_cb(lv_event_t *event)
{
    (void)event;
    esp_restart();
}

static void quick_standby_event_cb(lv_event_t *event)
{
    (void)event;
    close_overlay();
    standby_requested = true;
}

static void standby_task(void *argument)
{
    (void)argument;
    while (true) {
        if (!standby_requested) {
            vTaskDelay(pdMS_TO_TICKS(25));
            continue;
        }
        standby_requested = false;
        if (bsp_display_lock(250)) {
            archive_current_pour();
            bsp_display_unlock();
        }
        if (!pour_archive_wait_idle(5000)) {
            /* Do not cut power during an outstanding SD write. */
            ESP_LOGW(TAG, "Standby postponed: SD operation still running");
            continue;
        }
        pourbot_wifi_stop();
        standby_active = true;

        /* Disable all user-visible and high-draw peripherals before entering
         * deep sleep. GPIO6 remains an active-low RTC wake source. */
        lv_indev_t *touch = bsp_display_get_input_dev();
        if (touch && bsp_display_lock(250)) {
            lv_indev_reset(touch, NULL);
            lv_indev_enable(touch, false);
            bsp_display_unlock();
        }
        bsp_display_backlight_off();
        bsp_display_panel_off();

        /* HX711 power-down mode begins after PD_SCK stays high for >60 us. */
        gpio_set_level(HX711_SCK, 1);
        vTaskDelay(pdMS_TO_TICKS(20));

        /* Do not enter sleep while the wake key is already held, otherwise
         * the level-triggered wake source would restart immediately. */
        while (gpio_get_level(STANDBY_WAKE_GPIO) == 0) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        rtc_gpio_pullup_en(STANDBY_WAKE_GPIO);
        rtc_gpio_pulldown_dis(STANDBY_WAKE_GPIO);
        ESP_ERROR_CHECK(esp_sleep_enable_ext1_wakeup(
            1ULL << STANDBY_WAKE_GPIO, ESP_EXT1_WAKEUP_ANY_LOW));

        /* Deep-sleep wake performs a clean boot, restoring the last recipe
         * and calibration from NVS. */
        esp_deep_sleep_start();
    }
}

static void calibration_tare_event_cb(lv_event_t *event)
{
    (void)event;
    if (calibration_tare_requested || calibration_run_requested) return;
    lv_obj_set_style_bg_color(calibration_tare_button, lv_color_hex(0x475569), 0);
    lv_obj_set_style_bg_color(calibration_run_button, lv_color_hex(0x475569), 0);
    lv_obj_add_state(calibration_tare_button, LV_STATE_DISABLED);
    lv_obj_add_state(calibration_run_button, LV_STATE_DISABLED);
    lv_obj_add_state(calibration_weight_button, LV_STATE_DISABLED);
    calibration_tare_requested = true;
    lv_label_set_text(calibration_status_label, "TARING EMPTY SCALE...");
    lv_obj_set_style_text_color(calibration_status_label, lv_color_hex(0xF97316), 0);
}

static void calibration_run_event_cb(lv_event_t *event)
{
    (void)event;
    if (calibration_tare_requested || calibration_run_requested) return;
    lv_obj_set_style_bg_color(calibration_run_button, lv_color_hex(0x475569), 0);
    lv_obj_add_state(calibration_tare_button, LV_STATE_DISABLED);
    lv_obj_add_state(calibration_run_button, LV_STATE_DISABLED);
    lv_obj_add_state(calibration_weight_button, LV_STATE_DISABLED);
    calibration_run_requested = true;
    lv_label_set_text_fmt(calibration_status_label, "MEASURING %ld g REFERENCE...",
                          (long)calibration_known_grams);
    lv_obj_set_style_text_color(calibration_status_label, lv_color_hex(0xF97316), 0);
}

static void calibration_keypad_event_cb(lv_event_t *event)
{
    const lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_READY) {
        const char *text = lv_textarea_get_text(calibration_number_input);
        char *end;
        const long grams = strtol(text, &end, 10);
        if (!*text || *end || grams < 1 || grams > 5000) {
            lv_label_set_text(calibration_status_label, "ENTER 1 TO 5000 GRAMS");
            return;
        }
        calibration_known_grams = (int32_t)grams;
        lv_label_set_text_fmt(calibration_weight_value, "%ld g", grams);
        lv_label_set_text(calibration_status_label, "");
        lv_obj_set_style_bg_color(calibration_run_button, lv_color_hex(0x475569), 0);
    } else if (code != LV_EVENT_CANCEL) return;
    lv_obj_del(calibration_keypad);
    calibration_keypad = NULL;
    calibration_number_input = NULL;
}

static void calibration_weight_event_cb(lv_event_t *event)
{
    (void)event;
    if (calibration_keypad || calibration_tare_requested || calibration_run_requested) return;
    calibration_keypad = lv_obj_create(menu_overlay);
    lv_obj_set_size(calibration_keypad, 460, 280);
    lv_obj_center(calibration_keypad);
    lv_obj_set_style_bg_color(calibration_keypad, lv_color_hex(0x111827), 0);
    lv_obj_set_style_bg_opa(calibration_keypad, LV_OPA_COVER, 0);
    lv_obj_clear_flag(calibration_keypad, LV_OBJ_FLAG_SCROLLABLE);
    calibration_number_input = lv_textarea_create(calibration_keypad);
    lv_obj_set_size(calibration_number_input, 420, 48);
    lv_obj_align(calibration_number_input, LV_ALIGN_TOP_MID, 0, 0);
    lv_textarea_set_one_line(calibration_number_input, true);
    lv_textarea_set_accepted_chars(calibration_number_input, "0123456789");
    lv_textarea_set_max_length(calibration_number_input, 4);
    lv_textarea_set_placeholder_text(calibration_number_input, "Known weight in grams (1-5000)");
    lv_obj_set_style_text_font(calibration_number_input, &lv_font_montserrat_20, 0);
    lv_obj_t *keyboard = lv_keyboard_create(calibration_keypad);
    lv_obj_set_size(keyboard, 420, 180);
    lv_obj_align(keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_mode(keyboard, LV_KEYBOARD_MODE_NUMBER);
    lv_keyboard_set_textarea(keyboard, calibration_number_input);
    lv_obj_add_event_cb(keyboard, calibration_keypad_event_cb, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(keyboard, calibration_keypad_event_cb, LV_EVENT_CANCEL, NULL);
}

static void calibration_event_cb(lv_event_t *event)
{
    (void)event;
    close_overlay();
    menu_overlay = lv_obj_create(lv_layer_top());
    lv_obj_set_size(menu_overlay, 480, 320);
    lv_obj_set_style_bg_color(menu_overlay, lv_color_hex(0x020305), 0);
    lv_obj_set_style_bg_opa(menu_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(menu_overlay, 0, 0);
    lv_obj_clear_flag(menu_overlay, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(menu_overlay);
    lv_label_set_text(title, "SCALE CALIBRATION");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xF8FAFC), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 18, 16);

    lv_obj_t *back = small_action_button(menu_overlay, "BACK", 376, 10, 78,
                                         0x1F2937, close_overlay_event_cb);
    (void)back;

    lv_obj_t *instructions = lv_label_create(menu_overlay);
    lv_label_set_text(instructions,
        "1: Remove all weight and press TARE EMPTY. Wait for the button to turn green.\n\n"
        "2: Place a known weight. Set its weight in grams.\n\n"
        "3: Tap Calibrate. Wait for the button to turn green.");
    lv_obj_set_width(instructions, 230);
    lv_obj_set_style_text_font(instructions, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(instructions, lv_color_hex(0xCBD5E1), 0);
    lv_obj_set_style_text_line_space(instructions, 3, 0);
    lv_obj_align(instructions, LV_ALIGN_TOP_LEFT, 20, 62);

    calibration_tare_button = small_action_button(menu_overlay, "TARE EMPTY", 266, 157, 194, 0x475569,
                        calibration_tare_event_cb);

    lv_obj_t *known = lv_label_create(menu_overlay);
    lv_label_set_text(known, "KNOWN WEIGHT (g)");
    lv_obj_set_style_text_color(known, lv_color_hex(0x94A3B8), 0);
    lv_obj_align(known, LV_ALIGN_TOP_LEFT, 266, 65);
    calibration_weight_button = small_action_button(menu_overlay, "", 266, 91, 194, 0x1F2937,
                                                     calibration_weight_event_cb);
    calibration_weight_value = lv_obj_get_child(calibration_weight_button, 0);
    lv_label_set_text(calibration_weight_value, "TOUCH TO ENTER");
    lv_obj_set_style_text_font(calibration_weight_value, &lv_font_montserrat_16, 0);
    calibration_run_button = small_action_button(menu_overlay, "CALIBRATE", 266, 231, 194, 0x475569,
                        calibration_run_event_cb);

    calibration_status_label = lv_label_create(menu_overlay);
    lv_label_set_text(calibration_status_label, "");
    lv_obj_set_width(calibration_status_label, 230);
    lv_obj_set_style_text_font(calibration_status_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_align(calibration_status_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(calibration_status_label, lv_color_hex(0x94A3B8), 0);
    lv_obj_align(calibration_status_label, LV_ALIGN_TOP_LEFT, 20, 231);
}

static void ota_ui_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (!ota_status_label || !ota_progress_bar) return;
    pourbot_ota_status_t status;
    pourbot_ota_status(&status);
    lv_bar_set_value(ota_progress_bar, status.progress, LV_ANIM_ON);
    lv_table_set_cell_value(ota_version_table, 1, 0, status.current_version);
    lv_table_set_cell_value(ota_version_table, 1, 1,
        status.available_version[0] ? status.available_version : "--");
    lv_label_set_text_fmt(ota_status_label, "%s  %u%%", status.message, status.progress);
    bool installed = status.finished && status.ok && status.progress == 100;
    lv_obj_set_style_bg_color(ota_install_button,
        lv_color_hex(installed ? 0x16A34A : 0x2563EB), 0);
    if (status.running || !status.update_available)
        lv_obj_add_state(ota_install_button, LV_STATE_DISABLED);
    else lv_obj_clear_state(ota_install_button, LV_STATE_DISABLED);
    if (installed) {
        lv_obj_clear_state(ota_install_button, LV_STATE_DISABLED);
        lv_obj_clear_flag(ota_install_button, LV_OBJ_FLAG_CLICKABLE);
    }
    if (status.running || installed) lv_obj_add_state(ota_check_button, LV_STATE_DISABLED);
    else lv_obj_clear_state(ota_check_button, LV_STATE_DISABLED);
}

static void ota_install_event_cb(lv_event_t *event)
{
    (void)event;
    if (brew_running || brew_elapsed_us > 0) {
        lv_label_set_text(ota_status_label, "Reset the current brew before updating");
        return;
    }
    bool install = lv_event_get_target(event) == ota_install_button;
    bool started = install ? pourbot_ota_install() : pourbot_ota_check();
    if (!started) {
        lv_label_set_text(ota_status_label, "Update is already running or could not start");
        return;
    }
    lv_obj_add_state(ota_install_button, LV_STATE_DISABLED);
    ota_ui_timer_cb(NULL);
}

static void ota_event_cb(lv_event_t *event)
{
    (void)event;
    close_overlay();
    menu_overlay = lv_obj_create(lv_layer_top());
    lv_obj_set_size(menu_overlay, 480, 320);
    lv_obj_set_style_bg_color(menu_overlay, lv_color_hex(0x020305), 0);
    lv_obj_set_style_bg_opa(menu_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(menu_overlay, 0, 0);
    lv_obj_set_style_pad_all(menu_overlay, 0, 0);
    lv_obj_clear_flag(menu_overlay, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(menu_overlay);
    lv_label_set_text(title, "OTA UPDATE");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xF8FAFC), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 20, 18);
    small_action_button(menu_overlay, "BACK", 386, 10, 78, 0x1F2937,
                        settings_event_cb);

    lv_obj_t *card = make_panel(menu_overlay, 440, 240, 0x05070B);
    lv_obj_align(card, LV_ALIGN_TOP_MID, 0, 66);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(0x1F2937), 0);
    ota_version_table = lv_table_create(card);
    lv_obj_set_pos(ota_version_table, 18, 6);
    lv_table_set_col_cnt(ota_version_table, 2);
    lv_table_set_row_cnt(ota_version_table, 2);
    lv_table_set_col_width(ota_version_table, 0, 201);
    lv_table_set_col_width(ota_version_table, 1, 201);
    lv_obj_set_style_bg_color(ota_version_table, lv_color_hex(0x05070B), LV_PART_ITEMS);
    lv_obj_set_style_text_color(ota_version_table, lv_color_hex(0xF8FAFC), LV_PART_ITEMS);
    lv_obj_set_style_text_font(ota_version_table, &lv_font_montserrat_18, LV_PART_ITEMS);
    lv_obj_set_style_text_align(ota_version_table, LV_TEXT_ALIGN_CENTER, LV_PART_ITEMS);
    lv_obj_set_style_pad_ver(ota_version_table, 4, LV_PART_ITEMS);
    lv_obj_set_style_border_color(ota_version_table, lv_color_hex(0x1F2937), LV_PART_ITEMS);
    lv_obj_set_style_border_width(ota_version_table, 1, LV_PART_ITEMS);
    lv_obj_set_style_pad_all(ota_version_table, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(ota_version_table, 0, LV_PART_MAIN);
    lv_obj_clear_flag(ota_version_table, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_table_set_cell_value(ota_version_table, 0, 0, "Current version");
    lv_table_set_cell_value(ota_version_table, 0, 1, "Check");

    ota_progress_bar = lv_bar_create(card);
    lv_obj_set_size(ota_progress_bar, 402, 10);
    lv_obj_align(ota_progress_bar, LV_ALIGN_TOP_LEFT, 18, 73);
    lv_bar_set_range(ota_progress_bar, 0, 100);
    lv_obj_set_style_bg_color(ota_progress_bar, lv_color_hex(0x1F2937), LV_PART_MAIN);
    lv_obj_set_style_bg_color(ota_progress_bar, lv_color_hex(0x22C55E), LV_PART_INDICATOR);

    ota_status_label = lv_label_create(card);
    lv_obj_set_width(ota_status_label, 402);
    lv_obj_set_style_text_font(ota_status_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(ota_status_label, lv_color_hex(0xF8FAFC), 0);
    lv_obj_set_style_text_align(ota_status_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(ota_status_label, LV_ALIGN_TOP_LEFT, 18, 89);
    ota_check_button = small_action_button(card, "CHECK FOR UPDATES", 18, 119, 402,
                                           0x2563EB, ota_install_event_cb);
    ota_install_button = small_action_button(card, "DOWNLOAD AND INSTALL UPDATE", 18, 178, 402,
                                             0x2563EB, ota_install_event_cb);
    lv_obj_set_height(ota_install_button, 50);
    /* A 422 x 70 touch target, contained in the card and separated from Check. */
    lv_obj_set_ext_click_area(ota_install_button, 10);
    overlay_timer = lv_timer_create(ota_ui_timer_cb, 250, NULL);
    ota_ui_timer_cb(NULL);
}

static void settings_event_cb(lv_event_t *event)
{
    (void)event;
    close_overlay();
    menu_overlay = lv_obj_create(lv_layer_top());
    lv_obj_set_size(menu_overlay, 480, 320);
    lv_obj_set_style_bg_color(menu_overlay, lv_color_hex(0x020305), 0);
    lv_obj_set_style_bg_opa(menu_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(menu_overlay, 0, 0);
    lv_obj_clear_flag(menu_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *title = lv_label_create(menu_overlay);
    lv_label_set_text(title, "SETTINGS");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xF8FAFC), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 20, 18);
    small_action_button(menu_overlay, "BACK", 386, 10, 78, 0x1F2937,
                        close_overlay_event_cb);
    lv_obj_t *card = make_panel(menu_overlay, 440, 190, 0x05070B);
    lv_obj_align(card, LV_ALIGN_TOP_MID, 0, 74);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(0x1F2937), 0);
    small_action_button(card, "OPEN CALIBRATION", 18, 18, 210, 0x2563EB,
                        calibration_event_cb);
    small_action_button(card, "WI-FI SETUP", 18, 80, 210, 0x2563EB,
                        wifi_event_cb);
    small_action_button(card, "OTA UPDATE", 18, 142, 210, 0xB7791F,
                        ota_event_cb);
}

static void wifi_back_event_cb(lv_event_t *event)
{
    (void)event;
    if (wifi_selected_ssid[0]) wifi_event_cb(NULL);
    else settings_event_cb(NULL);
}

static void wifi_scan_event_cb(lv_event_t *event)
{
    (void)event;
    if (!pourbot_wifi_scan()) lv_label_set_text(wifi_status_label,
        "Wi-Fi busy or starting - try SCAN again");
    else lv_label_set_text(wifi_status_label, "Scanning...");
}

static void wifi_connect_event_cb(lv_event_t *event)
{
    (void)event;
    if (pourbot_wifi_connect(wifi_selected_ssid, lv_textarea_get_text(wifi_password_area)))
        lv_label_set_text(wifi_status_label, "Connecting...");
    else lv_label_set_text(wifi_status_label, "Cannot connect yet - wait for scan/startup");
}

static void wifi_network_event_cb(lv_event_t *event)
{
    unsigned index = (uintptr_t)lv_event_get_user_data(event);
    if (index >= wifi_ui_status.count) return;
    strlcpy(wifi_selected_ssid, wifi_ui_status.networks[index].ssid, sizeof(wifi_selected_ssid));
    lv_obj_add_flag(wifi_network_list, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(wifi_connect_button, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *selected = lv_label_create(menu_overlay);
    lv_label_set_text(selected, wifi_selected_ssid);
    lv_obj_set_width(selected, 444);
    lv_label_set_long_mode(selected, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(selected, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(selected, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(selected, LV_ALIGN_TOP_LEFT, 18, 80);
    wifi_password_area = lv_textarea_create(menu_overlay);
    lv_obj_set_size(wifi_password_area, 444, 43);
    lv_obj_align(wifi_password_area, LV_ALIGN_TOP_LEFT, 18, 104);
    lv_textarea_set_one_line(wifi_password_area, true);
    lv_textarea_set_password_mode(wifi_password_area, true);
    lv_textarea_set_max_length(wifi_password_area, 64);
    lv_textarea_set_placeholder_text(wifi_password_area, "Password (leave blank for open Wi-Fi)");
    lv_obj_set_style_bg_color(wifi_password_area, lv_color_hex(0x111827), 0);
    lv_obj_set_style_bg_opa(wifi_password_area, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(wifi_password_area, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_color(wifi_password_area, lv_color_hex(0xE2E8F0), LV_PART_TEXTAREA_PLACEHOLDER);
    lv_obj_set_style_text_opa(wifi_password_area, LV_OPA_COVER, LV_PART_TEXTAREA_PLACEHOLDER);
    lv_obj_t *keyboard = lv_keyboard_create(menu_overlay);
    lv_obj_set_size(keyboard, 480, 160);
    lv_obj_align(keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(keyboard, wifi_password_area);
}

static void wifi_ui_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (!wifi_status_label) return;
    pourbot_wifi_status(&wifi_ui_status);
    if (wifi_ui_status.connected) {
        lv_label_set_text_fmt(wifi_status_label, "%s / %s\n%s", wifi_ui_status.ssid,
            wifi_ui_status.ip, pourbot_time_valid() ? "Local date/time synchronized" : "Waiting for internet time...");
    } else lv_label_set_text(wifi_status_label, wifi_ui_status.message[0]
        ? wifi_ui_status.message : "Wi-Fi starting...");
    if (wifi_ui_status.scan_generation != wifi_scan_generation && !wifi_selected_ssid[0]) {
        wifi_scan_generation = wifi_ui_status.scan_generation;
        lv_obj_clean(wifi_network_list);
        for (unsigned i = 0; i < wifi_ui_status.count; ++i) {
            lv_obj_t *row = lv_btn_create(wifi_network_list);
            lv_obj_set_size(row, 418, 50);
            lv_obj_set_style_bg_color(row, lv_color_hex(0x263442), 0);
            lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
            lv_obj_align(row, LV_ALIGN_TOP_LEFT, 0, i * 58);
            lv_obj_add_event_cb(row, wifi_network_event_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)i);
            lv_obj_t *label = lv_label_create(row);
            lv_label_set_text_fmt(label, "%s\n%d dBm  %s", wifi_ui_status.networks[i].ssid,
                wifi_ui_status.networks[i].rssi, wifi_ui_status.networks[i].secure ? "LOCKED" : "OPEN");
            lv_obj_set_width(label, 390);
            lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
            lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);
            lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), 0);
            lv_obj_set_style_text_opa(label, LV_OPA_COVER, 0);
            lv_obj_center(label);
        }
    }
}

static void wifi_event_cb(lv_event_t *event)
{
    (void)event;
    close_overlay();
    wifi_selected_ssid[0] = 0;
    wifi_scan_generation = UINT32_MAX;
    menu_overlay = make_panel(lv_layer_top(), 480, 320, 0x020305);
    lv_obj_center(menu_overlay);
    lv_obj_set_style_text_color(menu_overlay, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_opa(menu_overlay, LV_OPA_COVER, 0);
    lv_obj_t *title = lv_label_create(menu_overlay);
    lv_label_set_text(title, "WI-FI");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 18, 18);
    small_action_button(menu_overlay, "SCAN", 165, 10, 96, 0x2563EB, wifi_scan_event_cb);
    wifi_connect_button = small_action_button(menu_overlay, "CONNECT", 269, 10, 112,
        0xB7791F, wifi_connect_event_cb);
    lv_obj_add_flag(wifi_connect_button, LV_OBJ_FLAG_HIDDEN);
    small_action_button(menu_overlay, "BACK", 389, 10, 78, 0x1F2937, wifi_back_event_cb);
    wifi_status_label = lv_label_create(menu_overlay);
    lv_obj_set_size(wifi_status_label, 444, 32);
    lv_obj_set_style_text_font(wifi_status_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(wifi_status_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_opa(wifi_status_label, LV_OPA_COVER, 0);
    lv_obj_align(wifi_status_label, LV_ALIGN_TOP_LEFT, 18, 56);
    wifi_network_list = lv_obj_create(menu_overlay);
    lv_obj_set_size(wifi_network_list, 444, 218);
    lv_obj_align(wifi_network_list, LV_ALIGN_TOP_LEFT, 18, 96);
    lv_obj_set_style_bg_color(wifi_network_list, lv_color_hex(0x091016), 0);
    lv_obj_set_style_pad_all(wifi_network_list, 8, 0);
    overlay_timer = lv_timer_create(wifi_ui_timer_cb, 250, NULL);
    wifi_ui_timer_cb(NULL);
}

static void analytics_list_render(void);

static void analytics_refresh_event_cb(lv_event_t *event)
{
    (void)event;
    if (!archive_ready || !pour_archive_list()) lv_label_set_text(analytics_status_label, "Storage busy/unavailable - try again");
    else lv_label_set_text(analytics_status_label, "Reading SD archive...");
}

static void analytics_back_event_cb(lv_event_t *event)
{
    (void)event;
    if (analytics_detail_open) {
        analytics_detail_open = false;
        analytics_refresh_event_cb(NULL);
    } else {
        close_overlay_event_cb(NULL);
    }
}

static void analytics_detail_render(void)
{
    analytics_detail_open = true;
    lv_obj_clean(analytics_body);
    const pour_entry_t *e = &analytics_result->selected;
    lv_obj_t *info = lv_label_create(analytics_body);
    lv_label_set_text_fmt(info, "%s  |  %ug dose  |  %ug target", e->recipe, e->dose, e->target);
    lv_obj_set_style_text_font(info, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(info, lv_color_hex(0xF8FAFC), 0);
    lv_obj_align(info, LV_ALIGN_TOP_LEFT, 10, 0);
    lv_obj_t *legend = lv_label_create(analytics_body);
    lv_label_set_recolor(legend, true);
    lv_label_set_text(legend, "#F2B94F ● Weight (g)#     #22AEEF ● Flow (g/s)#");
    lv_obj_set_style_text_font(legend, &lv_font_montserrat_12, 0);
    lv_obj_align(legend, LV_ALIGN_TOP_LEFT, 45, 23);
    int32_t max_weight = 500, max_flow = 100;
    int32_t final_weight_tenths = 0;
    for (unsigned i = 0; i < e->count; ++i) {
        int32_t w = analytics_result->samples[i].weight_tenths / 10;
        int32_t f = analytics_result->samples[i].flow_tenths;
        if (analytics_result->samples[i].weight_tenths > final_weight_tenths)
            final_weight_tenths = analytics_result->samples[i].weight_tenths;
        if (w >= max_weight) max_weight = ((w + 99) / 100 + 1) * 100;
        if (f >= max_flow) max_flow = ((f + 49) / 50 + 1) * 50;
    }
    /* LVGL 8 uses 16-bit chart coordinates; keep malformed/extreme values bounded. */
    if (max_weight > 32000) max_weight = 32000;
    if (max_flow > 32000) max_flow = 32000;
    lv_obj_t *chart = lv_chart_create(analytics_body);
    lv_obj_set_size(chart, 334, 139);
    lv_obj_align(chart, LV_ALIGN_TOP_LEFT, 43, 48);
    lv_chart_set_type(chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(chart, 60);
    lv_chart_set_range(chart, LV_CHART_AXIS_PRIMARY_Y, 0, max_weight);
    lv_chart_set_range(chart, LV_CHART_AXIS_SECONDARY_Y, 0, max_flow);
    lv_chart_set_div_line_count(chart, 5, 7);
    lv_obj_set_style_bg_color(chart, lv_color_hex(0x04080B), 0);
    lv_obj_set_style_bg_opa(chart, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(chart, 0, 0);
    lv_obj_set_style_line_color(chart, lv_color_hex(0x17252E), LV_PART_MAIN);
    lv_obj_set_style_size(chart, 0, LV_PART_INDICATOR);
    lv_chart_series_t *weight = lv_chart_add_series(chart, lv_color_hex(0xF2B94F), LV_CHART_AXIS_PRIMARY_Y);
    lv_chart_series_t *flow = lv_chart_add_series(chart, lv_color_hex(0x22AEEF), LV_CHART_AXIS_SECONDARY_Y);
    uint32_t duration = analytics_result->samples[e->count - 1].seconds;
    unsigned index = 0;
    for (unsigned point = 0; point < 60; ++point) {
        uint32_t t = (uint64_t)duration * point / 59;
        while (index + 1 < e->count && analytics_result->samples[index + 1].seconds <= t) index++;
        lv_chart_set_value_by_id(chart, weight, point, analytics_result->samples[index].weight_tenths / 10);
        lv_chart_set_value_by_id(chart, flow, point, analytics_result->samples[index].flow_tenths);
    }
    for (unsigned i = 0; i < 5; ++i) {
        lv_obj_t *tick = lv_label_create(analytics_body);
        lv_label_set_text_fmt(tick, "%ld", (long)(max_weight * (4 - i) / 4));
        lv_obj_set_width(tick, 36);
        lv_obj_set_style_text_align(tick, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_style_text_font(tick, &lv_font_montserrat_10, 0);
        lv_obj_set_style_text_color(tick, lv_color_hex(0xDCE5EA), 0);
        lv_obj_align(tick, LV_ALIGN_TOP_LEFT, 0, 45 + i * 34);
    }
    for (unsigned i = 0; i < 4; ++i) {
        char text[16]; format_chart_time(text, sizeof(text), (uint64_t)duration * i / 3);
        lv_obj_t *tick = lv_label_create(analytics_body);
        lv_label_set_text(tick, text);
        lv_obj_set_width(tick, 42);
        lv_obj_set_style_text_align(tick, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_font(tick, &lv_font_montserrat_10, 0);
        lv_obj_set_style_text_color(tick, lv_color_hex(0xDCE5EA), 0);
        lv_obj_align(tick, LV_ALIGN_TOP_LEFT, 22 + i * 111, 191);
    }
    lv_obj_t *axis_title = lv_label_create(analytics_body);
    lv_label_set_text(axis_title, "TIME (min:sec)");
    lv_obj_set_style_text_font(axis_title, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(axis_title, lv_color_hex(0xDCE5EA), 0);
    lv_obj_align(axis_title, LV_ALIGN_TOP_LEFT, 177, 211);
    lv_obj_t *stats = lv_label_create(analytics_body);
    char final[20], average[20], elapsed[16];
    /* The brewer may remove the dripper before ending/resetting the session.
     * Treat the greatest weight reached as the completed pour weight instead
     * of using the last sample, which can legitimately be back at zero. */
    format_fixed(final, sizeof(final), final_weight_tenths / 10.0f, 1);
    const float average_flow = duration > 0
        ? final_weight_tenths / 10.0f / duration : 0.0f;
    format_fixed(average, sizeof(average), average_flow, 1);
    format_chart_time(elapsed, sizeof(elapsed), duration);
    lv_label_set_text_fmt(stats, "FINAL\n%s g\n\nAVG FLOW\n%s g/s\n\nTIME\n%s", final, average, elapsed);
    lv_obj_set_style_text_font(stats, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(stats, lv_color_hex(0xF8FAFC), 0);
    lv_obj_align(stats, LV_ALIGN_TOP_LEFT, 385, 48);
    lv_label_set_text(analytics_status_label, e->filename);
}

static void analytics_load_event_cb(lv_event_t *event)
{
    if (analytics_long_press_handled) {
        analytics_long_press_handled = false;
        return;
    }
    unsigned index = (uintptr_t)lv_event_get_user_data(event);
    if (index >= analytics_result->entry_count) return;
    if (!pour_archive_load(analytics_result->entries[index].filename))
        lv_label_set_text(analytics_status_label, "Storage busy - try again");
    else lv_label_set_text(analytics_status_label, "Loading pour...");
}

static void analytics_delete_popup_close(void)
{
    if (analytics_delete_popup) lv_obj_del(analytics_delete_popup);
    analytics_delete_popup = NULL;
    analytics_delete_filename[0] = 0;
}

static void analytics_delete_cancel_cb(lv_event_t *event)
{
    (void)event;
    analytics_delete_popup_close();
}

static void analytics_delete_confirm_cb(lv_event_t *event)
{
    (void)event;
    if (!pour_archive_delete(analytics_delete_filename)) {
        lv_label_set_text(analytics_status_label, "Storage busy - could not delete");
    } else {
        lv_label_set_text(analytics_status_label, "Deleting pour...");
    }
    analytics_delete_popup_close();
}

static void analytics_long_press_cb(lv_event_t *event)
{
    unsigned index = (uintptr_t)lv_event_get_user_data(event);
    if (index >= analytics_result->entry_count || analytics_delete_popup) return;
    analytics_long_press_handled = true;
    strlcpy(analytics_delete_filename, analytics_result->entries[index].filename,
            sizeof(analytics_delete_filename));

    analytics_delete_popup = lv_obj_create(menu_overlay);
    lv_obj_set_size(analytics_delete_popup, 480, 320);
    lv_obj_center(analytics_delete_popup);
    lv_obj_set_style_bg_color(analytics_delete_popup, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(analytics_delete_popup, LV_OPA_70, 0);
    lv_obj_set_style_border_width(analytics_delete_popup, 0, 0);
    lv_obj_set_style_pad_all(analytics_delete_popup, 0, 0);
    lv_obj_clear_flag(analytics_delete_popup, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *panel = make_panel(analytics_delete_popup, 370, 164, 0x101820);
    lv_obj_center(panel);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(0x475569), 0);
    lv_obj_t *title = lv_label_create(panel);
    lv_label_set_text(title, "DELETE SAVED POUR?");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xF8FAFC), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 18);
    lv_obj_t *name = lv_label_create(panel);
    lv_label_set_text(name, analytics_delete_filename);
    lv_obj_set_width(name, 330);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(name, lv_color_hex(0xCBD5E1), 0);
    lv_obj_align(name, LV_ALIGN_TOP_MID, 0, 50);
    small_action_button(panel, "CANCEL", 24, 100, 148, 0x475569, analytics_delete_cancel_cb);
    small_action_button(panel, "DELETE", 198, 100, 148, 0xB91C1C, analytics_delete_confirm_cb);
    lv_obj_move_foreground(analytics_delete_popup);
}

static void analytics_list_render(void)
{
    analytics_detail_open = false;
    lv_obj_clean(analytics_body);
    lv_obj_t *list = lv_obj_create(analytics_body);
    lv_obj_set_size(list, 464, 252);
    lv_obj_set_style_bg_color(list, lv_color_hex(0x091016), 0);
    lv_obj_set_style_pad_all(list, 8, 0);
    for (unsigned i = 0; i < analytics_result->entry_count; ++i) {
        lv_obj_t *row = lv_btn_create(list);
        lv_obj_set_size(row, 430, 53);
        lv_obj_align(row, LV_ALIGN_TOP_LEFT, 0, i * 61);
        lv_obj_set_style_bg_color(row, lv_color_hex(0x1F2937), 0);
        lv_obj_add_event_cb(row, analytics_load_event_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)i);
        lv_obj_add_event_cb(row, analytics_long_press_cb, LV_EVENT_LONG_PRESSED, (void *)(uintptr_t)i);
        lv_obj_t *label = lv_label_create(row);
        lv_label_set_text_fmt(label, "%s\n%s", analytics_result->entries[i].filename, analytics_result->entries[i].recipe);
        lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);
        lv_obj_align(label, LV_ALIGN_TOP_LEFT, 4, 0);
    }
    if (!analytics_result->entry_count) {
        lv_obj_t *empty = lv_label_create(list);
        lv_label_set_text(empty, analytics_result->ok ? "No saved pours yet.\nSave a brew or hold START to reset." : analytics_result->message);
        lv_obj_set_width(empty, 420);
        lv_obj_set_style_text_font(empty, &lv_font_montserrat_16, 0);
    }
}

static void analytics_ui_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (!analytics_body || !analytics_result) return;
    if (!pour_archive_result(analytics_generation, analytics_result)) return;
    analytics_generation = analytics_result->generation;
    lv_label_set_text(analytics_status_label, analytics_result->message);
    if (analytics_result->operation == POUR_SAVE && !analytics_result->ok) current_pour_saved = false;
    if (analytics_result->operation == POUR_LOAD && analytics_result->ok) analytics_detail_render();
    else analytics_list_render();
}

static void analytics_event_cb(lv_event_t *event)
{
    (void)event;
    close_overlay();
    analytics_detail_open = false;
    analytics_delete_popup = NULL;
    analytics_delete_filename[0] = 0;
    analytics_long_press_handled = false;
    if (!analytics_result) analytics_result = heap_caps_calloc(1, sizeof(*analytics_result), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    menu_overlay = make_panel(lv_layer_top(), 480, 320, 0x020305);
    lv_obj_center(menu_overlay);
    lv_obj_t *title = lv_label_create(menu_overlay);
    lv_label_set_text(title, "POURS");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_18, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 12, 16);
    small_action_button(menu_overlay, "BACK", 386, 7, 78, 0x1F2937, analytics_back_event_cb);
    analytics_status_label = lv_label_create(menu_overlay);
    lv_obj_set_width(analytics_status_label, 456);
    lv_obj_set_style_text_font(analytics_status_label, &lv_font_montserrat_10, 0);
    lv_obj_align(analytics_status_label, LV_ALIGN_TOP_LEFT, 12, 54);
    analytics_body = make_panel(menu_overlay, 464, 252, 0x05090C);
    lv_obj_align(analytics_body, LV_ALIGN_TOP_LEFT, 8, 68);
    if (!analytics_result) { lv_label_set_text(analytics_status_label, "Not enough memory for archive viewer"); return; }
    overlay_timer = lv_timer_create(analytics_ui_timer_cb, 100, NULL);
    analytics_refresh_event_cb(NULL);
}

static void recipes_event_cb(lv_event_t *event);

static void recipe_refresh_values(void)
{
    if (!recipe_edit_values[0]) return;
    lv_label_set_text_fmt(recipe_edit_values[0], "%u g", recipe_edit.dose_g);
    lv_label_set_text_fmt(recipe_edit_values[1], "1:%u.%u",
                          recipe_edit.ratio_x10 / 10, recipe_edit.ratio_x10 % 10);
    lv_label_set_text_fmt(recipe_edit_values[2], "%u sec", recipe_edit.bloom_hold_s);
    lv_label_set_text_fmt(recipe_edit_values[3], "%u sec", recipe_edit.pour_hold_s);
}

static void recipe_adjust(uint8_t field, int direction)
{
    if (field == 0) {
        int value = recipe_edit.dose_g + direction;
        recipe_edit.dose_g = value < 5 ? 5 : (value > 100 ? 100 : value);
    } else if (field == 1) {
        int value = recipe_edit.ratio_x10 + direction;
        recipe_edit.ratio_x10 = value < 100 ? 100 : (value > 250 ? 250 : value);
    } else if (field == 2) {
        int value = recipe_edit.bloom_hold_s + direction * 5;
        recipe_edit.bloom_hold_s = value < 0 ? 0 : (value > 180 ? 180 : value);
    } else {
        int value = recipe_edit.pour_hold_s + direction * 5;
        recipe_edit.pour_hold_s = value < 0 ? 0 : (value > 180 ? 180 : value);
    }
    recipe_refresh_values();
}

static void recipe_minus_event_cb(lv_event_t *event)
{
    recipe_adjust((uint8_t)(uintptr_t)lv_event_get_user_data(event), -1);
}

static void recipe_plus_event_cb(lv_event_t *event)
{
    recipe_adjust((uint8_t)(uintptr_t)lv_event_get_user_data(event), 1);
}

static void recipe_save_event_cb(lv_event_t *event)
{
    (void)event;
    pourbot_recipe_save_and_select(recipe_edit_index, &recipe_edit);
    lv_label_set_text(recipe_title_label, recipe_edit.name);
    if (dashboard_recipe_label) lv_label_set_text(dashboard_recipe_label, recipe_edit.name);
    if (recipe_dose_value_label) {
        lv_label_set_text_fmt(recipe_dose_value_label, "%u.0 g", recipe_edit.dose_g);
    }
    if (recipe_target_value_label) {
        lv_label_set_text_fmt(recipe_target_value_label, "%u g\n1:%u.%u",
                              pourbot_recipe_target_g(&recipe_edit),
                              recipe_edit.ratio_x10 / 10, recipe_edit.ratio_x10 % 10);
    }
    /* Saving is the terminal action for the recipe flow.  Dismiss the editor
     * and reveal the tile that was active when the menu was opened. */
    close_overlay();
    memset(recipe_edit_values, 0, sizeof(recipe_edit_values));
}

static void recipe_editor_event_cb(lv_event_t *event)
{
    recipe_edit_index = (uint8_t)(uintptr_t)lv_event_get_user_data(event);
    recipe_edit = *pourbot_recipe_get(recipe_edit_index);
    close_overlay();
    menu_overlay = lv_obj_create(lv_layer_top());
    lv_obj_set_size(menu_overlay, 480, 320);
    lv_obj_set_style_bg_color(menu_overlay, lv_color_hex(0x020305), 0);
    lv_obj_set_style_bg_opa(menu_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(menu_overlay, 0, 0);
    lv_obj_clear_flag(menu_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *title = lv_label_create(menu_overlay);
    lv_label_set_text(title, recipe_edit.name);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xF8FAFC), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 16, 15);
    small_action_button(menu_overlay, "BACK", 386, 8, 78, 0x1F2937,
                        recipes_event_cb);

    const char *names[] = {"DOSE", "RATIO", "BLOOM HOLD", "BETWEEN POURS"};
    for (uint8_t i = 0; i < 4; ++i) {
        lv_coord_t x = (i & 1) ? 246 : 16;
        lv_coord_t y = (i < 2) ? 62 : 142;
        lv_obj_t *panel = make_panel(menu_overlay, 218, 66, 0x080B10);
        lv_obj_align(panel, LV_ALIGN_TOP_LEFT, x, y);
        lv_obj_set_style_border_width(panel, 1, 0);
        lv_obj_set_style_border_color(panel, lv_color_hex(0x1F2937), 0);
        lv_obj_t *label = lv_label_create(panel);
        lv_label_set_text(label, names[i]);
        lv_obj_set_style_text_font(label, &lv_font_montserrat_10, 0);
        lv_obj_set_style_text_color(label, lv_color_hex(0x94A3B8), 0);
        lv_obj_align(label, LV_ALIGN_TOP_LEFT, 10, 6);
        lv_obj_t *minus = lv_btn_create(panel);
        lv_obj_set_size(minus, 42, 34); lv_obj_align(minus, LV_ALIGN_BOTTOM_LEFT, 7, -5);
        lv_obj_set_style_bg_color(minus, lv_color_hex(0x1F2937), 0);
        lv_obj_add_event_cb(minus, recipe_minus_event_cb, LV_EVENT_PRESSED,
                            (void *)(uintptr_t)i);
        lv_obj_add_event_cb(minus, recipe_minus_event_cb, LV_EVENT_LONG_PRESSED_REPEAT,
                            (void *)(uintptr_t)i);
        lv_obj_t *ml = lv_label_create(minus); lv_label_set_text(ml, "-"); lv_obj_center(ml);
        recipe_edit_values[i] = lv_label_create(panel);
        lv_obj_set_width(recipe_edit_values[i], 105);
        lv_obj_set_style_text_align(recipe_edit_values[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_font(recipe_edit_values[i], &lv_font_montserrat_18, 0);
        lv_obj_set_style_text_color(recipe_edit_values[i], lv_color_hex(0xFFFFFF), 0);
        lv_obj_align(recipe_edit_values[i], LV_ALIGN_BOTTOM_MID, 0, -12);
        lv_obj_t *plus = lv_btn_create(panel);
        lv_obj_set_size(plus, 42, 34); lv_obj_align(plus, LV_ALIGN_BOTTOM_RIGHT, -7, -5);
        lv_obj_set_style_bg_color(plus, lv_color_hex(0x1F2937), 0);
        lv_obj_add_event_cb(plus, recipe_plus_event_cb, LV_EVENT_PRESSED,
                            (void *)(uintptr_t)i);
        lv_obj_add_event_cb(plus, recipe_plus_event_cb, LV_EVENT_LONG_PRESSED_REPEAT,
                            (void *)(uintptr_t)i);
        lv_obj_t *pl = lv_label_create(plus); lv_label_set_text(pl, "+"); lv_obj_center(pl);
    }
    recipe_refresh_values();
    lv_obj_t *hint = lv_label_create(menu_overlay);
    lv_label_set_text_fmt(hint, "%u pours  |  New target: %u g",
                          recipe_edit.pour_count, pourbot_recipe_target_g(&recipe_edit));
    lv_obj_set_style_text_color(hint, lv_color_hex(0x94A3B8), 0);
    lv_obj_align(hint, LV_ALIGN_TOP_LEFT, 18, 229);
    lv_obj_t *save_button = small_action_button(menu_overlay, "SAVE & USE",
                                                278, 245, 186, 0xB7791F,
                                                recipe_save_event_cb);
    /* Keep the visual layout compact, but make edge touches much more forgiving. */
    lv_obj_set_ext_click_area(save_button, 18);
}

static void recipes_event_cb(lv_event_t *event)
{
    (void)event;
    close_overlay();
    memset(recipe_edit_values, 0, sizeof(recipe_edit_values));
    menu_overlay = lv_obj_create(lv_layer_top());
    lv_obj_set_size(menu_overlay, 480, 320);
    lv_obj_set_style_bg_color(menu_overlay, lv_color_hex(0x020305), 0);
    lv_obj_set_style_bg_opa(menu_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(menu_overlay, 0, 0);
    lv_obj_clear_flag(menu_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *title = lv_label_create(menu_overlay);
    lv_label_set_text(title, "POUR-OVER RECIPES");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xF8FAFC), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 16, 14);
    small_action_button(menu_overlay, "BACK", 386, 8, 78, 0x1F2937,
                        close_overlay_event_cb);
    for (uint8_t i = 0; i < POURBOT_RECIPE_COUNT; ++i) {
        const pourbot_recipe_t *recipe = pourbot_recipe_get(i);
        lv_obj_t *row = lv_btn_create(menu_overlay);
        lv_obj_set_size(row, 448, 53);
        lv_obj_align(row, LV_ALIGN_TOP_LEFT, 16, 57 + i * 60);
        lv_obj_set_style_radius(row, 14, 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(i == pourbot_recipe_active_index() ? 0x2B2317 : 0x0B1017), 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_border_color(row, lv_color_hex(i == pourbot_recipe_active_index() ? 0xB7791F : 0x1F2937), 0);
        lv_obj_add_event_cb(row, recipe_editor_event_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)i);
        lv_obj_t *name = lv_label_create(row);
        lv_label_set_text(name, recipe->name);
        lv_obj_set_style_text_font(name, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(name, lv_color_hex(0xF8FAFC), 0);
        lv_obj_align(name, LV_ALIGN_TOP_LEFT, 4, -2);
        lv_obj_t *detail = lv_label_create(row);
        lv_label_set_text_fmt(detail, "%ug  1:%u.%u  %ug target  Bloom %us  Hold %us",
            recipe->dose_g, recipe->ratio_x10 / 10, recipe->ratio_x10 % 10,
            pourbot_recipe_target_g(recipe), recipe->bloom_hold_s, recipe->pour_hold_s);
        lv_obj_set_style_text_font(detail, &lv_font_montserrat_10, 0);
        lv_obj_set_style_text_color(detail, lv_color_hex(0xF8FAFC), 0);
        lv_obj_align(detail, LV_ALIGN_BOTTOM_LEFT, 4, 2);
    }
}

static void menu_event_cb(lv_event_t *event)
{
    (void)event;
    close_overlay();
    menu_overlay = lv_obj_create(lv_layer_top());
    lv_obj_set_size(menu_overlay, 480, 320);
    lv_obj_align(menu_overlay, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(menu_overlay, lv_color_hex(0x05070B), 0);
    lv_obj_set_style_bg_opa(menu_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(menu_overlay, 0, 0);
    lv_obj_set_style_radius(menu_overlay, 0, 0);
    lv_obj_set_style_pad_all(menu_overlay, 0, 0);
    lv_obj_clear_flag(menu_overlay, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(menu_overlay);
    lv_label_set_text(title, "POURBOT MENU");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_letter_space(title, 2, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0x94A3B8), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 24, 18);

    small_action_button(menu_overlay, "CLOSE", 386, 8, 78, 0x1F2937,
                        close_overlay_event_cb);
    overlay_button(menu_overlay, "SETTINGS", 16, 72,
                   settings_event_cb, NULL);
    overlay_button(menu_overlay, "RECIPES", 16, 146, recipes_event_cb, NULL);
    overlay_button(menu_overlay, "ANALYTICS", 16, 220,
                   analytics_event_cb, NULL);
}

static lv_obj_t *make_panel(lv_obj_t *parent, lv_coord_t width, lv_coord_t height,
                            uint32_t color)
{
    lv_obj_t *panel = lv_obj_create(parent);
    lv_obj_set_size(panel, width, height);
    lv_obj_set_style_radius(panel, 18, 0);
    lv_obj_set_style_bg_color(panel, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_pad_all(panel, 0, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    return panel;
}

static void add_page_dots(lv_obj_t *screen, uint8_t active_page)
{
    for (uint8_t i = 0; i < 2; ++i) {
        lv_obj_t *dot = lv_obj_create(screen);
        lv_obj_set_size(dot, i == active_page ? 7 : 6, i == active_page ? 7 : 6);
        lv_obj_align(dot, LV_ALIGN_TOP_MID, i == 0 ? -6 : 6, 29);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(dot,
            lv_color_hex(i == active_page ? 0xF2B94F : 0x42515A), 0);
        lv_obj_set_style_border_width(dot, 0, 0);
        lv_obj_set_style_pad_all(dot, 0, 0);
        lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    }
}

/* Main scale and brew gauge screen. */
static void create_ui(void)
{
    lv_obj_t *root = lv_scr_act();
    page_tileview = lv_tileview_create(root);
    lv_obj_set_size(page_tileview, 480, 320);
    lv_obj_set_pos(page_tileview, 0, 0);
    lv_obj_set_style_bg_color(page_tileview, lv_color_hex(0x030608), 0);
    lv_obj_set_style_bg_opa(page_tileview, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(page_tileview, 0, 0);
    lv_obj_set_style_pad_all(page_tileview, 0, 0);
    lv_obj_set_scrollbar_mode(page_tileview, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_snap_x(page_tileview, LV_SCROLL_SNAP_CENTER);
    lv_obj_set_style_anim_time(page_tileview, 280, 0);

    lv_obj_t *quick_screen = lv_tileview_add_tile(page_tileview, 0, 0,
                                                   LV_DIR_RIGHT);
    lv_obj_set_style_bg_color(quick_screen, lv_color_hex(0x030608), 0);
    lv_obj_set_style_bg_opa(quick_screen, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(quick_screen, 2, 0);
    lv_obj_set_style_border_color(quick_screen, lv_color_hex(0x314753), 0);
    lv_obj_set_style_radius(quick_screen, 16, 0);
    lv_obj_clear_flag(quick_screen, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *quick_title = lv_label_create(quick_screen);
    lv_label_set_recolor(quick_title, true);
    lv_label_set_text(quick_title, "Pour#F2B94F Bot#  SYSTEM");
    lv_obj_set_style_text_font(quick_title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(quick_title, lv_color_hex(0xF8FAFC), 0);
    lv_obj_align(quick_title, LV_ALIGN_TOP_MID, 0, 28);

    lv_obj_t *reboot = small_action_button(quick_screen,
                                            LV_SYMBOL_REFRESH "  REBOOT",
                                            24, 96, 204, 0x334155,
                                            quick_reboot_event_cb);
    lv_obj_t *standby = small_action_button(quick_screen,
                                             LV_SYMBOL_POWER "  STANDBY",
                                             252, 96, 204, 0xB7791F,
                                             quick_standby_event_cb);
    lv_obj_set_height(reboot, 112);
    lv_obj_set_height(standby, 112);

    lv_obj_t *return_hint = lv_label_create(quick_screen);
    lv_label_set_text(return_hint, LV_SYMBOL_LEFT "  SWIPE LEFT TO RETURN");
    lv_obj_set_style_text_font(return_hint, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(return_hint, lv_color_hex(0x64748B), 0);
    lv_obj_align(return_hint, LV_ALIGN_BOTTOM_MID, 0, -30);

    lv_obj_t *screen = lv_tileview_add_tile(page_tileview, 1, 0,
                                             LV_DIR_LEFT | LV_DIR_RIGHT);
    main_screen = screen;
    lv_obj_set_tile(page_tileview, main_screen, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x030608), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(screen, 2, 0);
    lv_obj_set_style_border_color(screen, lv_color_hex(0x314753), 0);
    lv_obj_set_style_radius(screen, 16, 0);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    const pourbot_recipe_t *recipe = pourbot_recipe_active();
    const uint16_t target_g = pourbot_recipe_target_g(recipe);

    lv_obj_t *menu_button = lv_btn_create(screen);
    lv_obj_set_size(menu_button, 138, 34);
    lv_obj_align(menu_button, LV_ALIGN_TOP_LEFT, 5, 2);
    lv_obj_set_style_bg_opa(menu_button, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(menu_button, lv_color_hex(0x18232B), LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(menu_button, 0, 0);
    lv_obj_set_style_radius(menu_button, 10, 0);
    lv_obj_add_event_cb(menu_button, menu_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *menu_label = lv_label_create(menu_button);
    lv_label_set_recolor(menu_label, true);
    lv_label_set_text(menu_label, LV_SYMBOL_LIST "  Pour#F2B94F Bot#");
    lv_obj_set_style_text_font(menu_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(menu_label, lv_color_hex(0xE8EEF2), 0);
    lv_obj_center(menu_label);

    recipe_title_label = lv_label_create(screen);
    lv_label_set_text(recipe_title_label, recipe->name);
    lv_obj_set_width(recipe_title_label, 210);
    lv_obj_set_style_text_align(recipe_title_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(recipe_title_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(recipe_title_label, lv_color_hex(0xDCE5EA), 0);
    lv_obj_align(recipe_title_label, LV_ALIGN_TOP_MID, 0, 8);
    add_page_dots(screen, 0);
    add_battery_label(screen, 0);


    status_label = lv_label_create(screen);
    lv_label_set_text(status_label, "");
    lv_obj_set_style_text_font(status_label, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(status_label, lv_color_hex(0x8FA0AA), 0);
    lv_obj_align(status_label, LV_ALIGN_TOP_MID, 0, 27);

    lv_obj_t *dose_card = make_panel(screen, 126, 112, 0x0B1217);
    main_dose_card = dose_card;
    lv_obj_align(dose_card, LV_ALIGN_TOP_LEFT, 14, 45);
    lv_obj_set_style_border_width(dose_card, 1, 0);
    lv_obj_set_style_border_color(dose_card, lv_color_hex(0x1B2A33), 0);
    /* Coffee bean icon: draw it locally so it does not depend on emoji/font support. */
    lv_obj_t *dose_mark = lv_obj_create(dose_card);
    lv_obj_remove_style_all(dose_mark);
    lv_obj_set_size(dose_mark, 19, 24);
    lv_obj_set_style_bg_opa(dose_mark, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(dose_mark, lv_color_hex(0xF2B94F), 0);
    lv_obj_set_style_radius(dose_mark, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_transform_angle(dose_mark, 280, 0);
    lv_obj_set_style_transform_pivot_x(dose_mark, 9, 0);
    lv_obj_set_style_transform_pivot_y(dose_mark, 12, 0);
    lv_obj_align(dose_mark, LV_ALIGN_LEFT_MID, 17, 0);

    lv_obj_t *bean_seam = lv_obj_create(dose_mark);
    lv_obj_remove_style_all(bean_seam);
    lv_obj_set_size(bean_seam, 3, 19);
    lv_obj_set_style_bg_opa(bean_seam, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(bean_seam, lv_color_hex(0x0B1217), 0);
    lv_obj_set_style_radius(bean_seam, LV_RADIUS_CIRCLE, 0);
    lv_obj_align(bean_seam, LV_ALIGN_CENTER, 0, 0);
    lv_obj_t *dose_caption = lv_label_create(dose_card);
    lv_label_set_text(dose_caption, "Dose");
    lv_obj_set_style_text_font(dose_caption, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(dose_caption, lv_color_hex(0x98A8B2), 0);
    lv_obj_align(dose_caption, LV_ALIGN_TOP_LEFT, 52, 24);
    recipe_dose_value_label = lv_label_create(dose_card);
    lv_label_set_text_fmt(recipe_dose_value_label, "%u.0 g", recipe->dose_g);
    lv_obj_set_style_text_font(recipe_dose_value_label, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(recipe_dose_value_label, lv_color_hex(0xF5F7F8), 0);
    lv_obj_align(recipe_dose_value_label, LV_ALIGN_TOP_LEFT, 52, 49);

    lv_obj_t *target_card = make_panel(screen, 126, 142, 0x0B1217);
    main_target_card = target_card;
    lv_obj_align(target_card, LV_ALIGN_TOP_LEFT, 14, 165);
    lv_obj_set_style_border_width(target_card, 1, 0);
    lv_obj_set_style_border_color(target_card, lv_color_hex(0x1B2A33), 0);
    lv_obj_t *drop = lv_label_create(target_card);
    lv_label_set_text(drop, LV_SYMBOL_TINT);
    lv_obj_set_style_text_font(drop, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(drop, lv_color_hex(0xF2B94F), 0);
    lv_obj_align(drop, LV_ALIGN_LEFT_MID, 13, -4);
    lv_obj_t *target_caption = lv_label_create(target_card);
    lv_label_set_text(target_caption, "Target");
    lv_obj_set_style_text_font(target_caption, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(target_caption, lv_color_hex(0x98A8B2), 0);
    lv_obj_align(target_caption, LV_ALIGN_TOP_LEFT, 52, 25);
    recipe_target_value_label = lv_label_create(target_card);
    lv_label_set_text_fmt(recipe_target_value_label, "%u g\n1:%u.%u", target_g,
                          recipe->ratio_x10 / 10, recipe->ratio_x10 % 10);
    lv_obj_set_style_text_font(recipe_target_value_label, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(recipe_target_value_label, lv_color_hex(0xF5F7F8), 0);
    lv_obj_set_style_text_line_space(recipe_target_value_label, 4, 0);
    lv_obj_align(recipe_target_value_label, LV_ALIGN_TOP_LEFT, 52, 50);

    progress_panel = lv_arc_create(screen);
    lv_obj_set_size(progress_panel, 200, 200);
    lv_obj_align(progress_panel, LV_ALIGN_TOP_LEFT, 205, 48);
    lv_arc_set_bg_angles(progress_panel, 180, 360);
    lv_arc_set_range(progress_panel, 0, target_g);
    lv_arc_set_value(progress_panel, 0);
    lv_obj_set_style_arc_width(progress_panel, 12, LV_PART_MAIN);
    lv_obj_set_style_arc_color(progress_panel, lv_color_hex(0x1C303B), LV_PART_MAIN);
    lv_obj_set_style_arc_width(progress_panel, 12, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(progress_panel, lv_color_hex(0xF2B94F), LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(progress_panel, LV_OPA_TRANSP, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(progress_panel, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_clear_flag(progress_panel, LV_OBJ_FLAG_CLICKABLE);

    /* Center the complete weight reading (number + unit) as one dynamic group.
     * The label width follows its text, so additional digits grow equally
     * around the same center point instead of only extending left. */
    lv_obj_t *weight_group = lv_obj_create(screen);
    main_weight_group = weight_group;
    lv_obj_remove_style_all(weight_group);
    lv_obj_set_size(weight_group, 250, 66);
    lv_obj_align(weight_group, LV_ALIGN_TOP_LEFT, 180, 82);
    lv_obj_set_flex_flow(weight_group, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(weight_group, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(weight_group, 7, 0);

    weight_label = lv_label_create(weight_group);
    lv_label_set_text(weight_label, boot_scale_ready ? "0.0" : "--");
    lv_obj_set_style_text_font(weight_label, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(weight_label, lv_color_hex(0xFFFFFF), 0);

    weight_unit_label = lv_label_create(weight_group);
    lv_label_set_text(weight_unit_label, "g");
    lv_obj_set_style_text_font(weight_unit_label, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(weight_unit_label, lv_color_hex(0xE4E9EC), 0);
    lv_obj_set_style_translate_y(weight_unit_label, 10, 0);

    timer_label = lv_label_create(screen);
    lv_label_set_text(timer_label, "00:00.0");
    lv_obj_set_width(timer_label, 200);
    lv_obj_set_style_text_align(timer_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(timer_label, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(timer_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(timer_label, LV_ALIGN_TOP_LEFT, 205, 145);
    lv_obj_add_flag(timer_label, LV_OBJ_FLAG_HIDDEN);
    lv_timer_create(brew_timer_display_cb, 50, NULL);

    flow_label = lv_label_create(screen);
    lv_label_set_text(flow_label, "FLOW  0.0 g/s");
    lv_obj_set_width(flow_label, 180);
    lv_obj_set_style_text_align(flow_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(flow_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(flow_label, lv_color_hex(0xAEBBC3), 0);
    lv_obj_align(flow_label, LV_ALIGN_TOP_LEFT, 215, 193);
    lv_obj_add_flag(flow_label, LV_OBJ_FLAG_HIDDEN);

    flow_bar = lv_arc_create(screen);
    lv_obj_set_size(flow_bar, 200, 200);
    lv_obj_align(flow_bar, LV_ALIGN_TOP_LEFT, 205, 48);
    lv_arc_set_bg_angles(flow_bar, 0, 180);
    lv_arc_set_mode(flow_bar, LV_ARC_MODE_REVERSE);
    lv_arc_set_range(flow_bar, 0, 1000);
    lv_arc_set_value(flow_bar, 0);
    lv_obj_set_style_arc_color(flow_bar, lv_color_hex(0x1C303B), LV_PART_MAIN);
    lv_obj_set_style_arc_width(flow_bar, 12, LV_PART_MAIN);
    lv_obj_set_style_arc_color(flow_bar, lv_color_hex(0xF2B94F), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(flow_bar, 12, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(flow_bar, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_clear_flag(flow_bar, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(flow_bar, LV_OBJ_FLAG_HIDDEN);

    weight_chart = NULL;
    weight_chart_series = NULL;
    flow_chart_series = NULL;
    for (int i = 0; i < 4; ++i) chart_time_labels[i] = NULL;

    tare_button = lv_btn_create(screen);
    lv_obj_set_size(tare_button, 155, 58);
    lv_obj_align(tare_button, LV_ALIGN_BOTTOM_LEFT, 150, -9);
    lv_obj_set_style_radius(tare_button, 12, 0);
    lv_obj_set_style_bg_color(tare_button, lv_color_hex(0x34414A), 0);
    lv_obj_set_style_bg_color(tare_button, lv_color_hex(0x465660), LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(tare_button, lv_color_hex(0x7C2D12), LV_STATE_DISABLED);
    lv_obj_set_style_shadow_width(tare_button, 0, 0);
    lv_obj_add_event_cb(tare_button, tare_event_cb, LV_EVENT_CLICKED, NULL);
    tare_button_label = lv_label_create(tare_button);
    lv_label_set_text(tare_button_label, LV_SYMBOL_REFRESH "  TARE");
    lv_obj_set_style_text_font(tare_button_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(tare_button_label, lv_color_hex(0xEAF0F3), 0);
    lv_obj_center(tare_button_label);

    lv_obj_t *brew_button = lv_btn_create(screen);
    lv_obj_set_size(brew_button, 155, 58);
    lv_obj_align(brew_button, LV_ALIGN_BOTTOM_RIGHT, -12, -9);
    main_brew_button = brew_button;
    lv_obj_set_style_radius(brew_button, 12, 0);
    lv_obj_set_style_bg_color(brew_button, lv_color_hex(0xF2B94F), 0);
    lv_obj_set_style_bg_color(brew_button, lv_color_hex(0xD99A31), LV_STATE_PRESSED);
    lv_obj_set_style_border_width(brew_button, 0, 0);
    lv_obj_set_style_shadow_width(brew_button, 0, 0);
    lv_obj_add_event_cb(brew_button, brew_event_cb, LV_EVENT_SHORT_CLICKED, NULL);
    lv_obj_add_event_cb(brew_button, brew_event_cb, LV_EVENT_LONG_PRESSED, NULL);
    brew_button_label = lv_label_create(brew_button);
    lv_label_set_text(brew_button_label, LV_SYMBOL_PLAY "  START");
    lv_obj_set_style_text_font(brew_button_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(brew_button_label, lv_color_hex(0x111619), 0);
    lv_obj_center(brew_button_label);
}

static void create_graph_screen(void)
{
    graph_screen = lv_tileview_add_tile(page_tileview, 2, 0,
                                         LV_DIR_LEFT | LV_DIR_RIGHT);
    lv_obj_set_style_bg_color(graph_screen, lv_color_hex(0x030608), 0);
    lv_obj_set_style_bg_opa(graph_screen, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(graph_screen, 2, 0);
    lv_obj_set_style_border_color(graph_screen, lv_color_hex(0x314753), 0);
    lv_obj_set_style_radius(graph_screen, 16, 0);
    lv_obj_clear_flag(graph_screen, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *menu_button = lv_btn_create(graph_screen);
    lv_obj_set_size(menu_button, 138, 34);
    lv_obj_align(menu_button, LV_ALIGN_TOP_LEFT, 5, 2);
    lv_obj_set_style_bg_opa(menu_button, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(menu_button, lv_color_hex(0x18232B), LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(menu_button, 0, 0);
    lv_obj_set_style_radius(menu_button, 10, 0);
    lv_obj_add_event_cb(menu_button, menu_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *menu_label = lv_label_create(menu_button);
    lv_label_set_recolor(menu_label, true);
    lv_label_set_text(menu_label, LV_SYMBOL_LIST "  Pour#F2B94F Bot#");
    lv_obj_set_style_text_font(menu_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(menu_label, lv_color_hex(0xE8EEF2), 0);
    lv_obj_center(menu_label);

    graph_recipe_label = lv_label_create(graph_screen);
    lv_label_set_text(graph_recipe_label, pourbot_recipe_active()->name);
    lv_obj_set_width(graph_recipe_label, 190);
    lv_obj_set_style_text_align(graph_recipe_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(graph_recipe_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(graph_recipe_label, lv_color_hex(0xDCE5EA), 0);
    lv_obj_align(graph_recipe_label, LV_ALIGN_TOP_MID, 0, 7);
    add_page_dots(graph_screen, 1);
    add_battery_label(graph_screen, 1);

    lv_obj_t *rule = lv_obj_create(graph_screen);
    lv_obj_set_size(rule, 452, 1);
    lv_obj_align(rule, LV_ALIGN_TOP_MID, 0, 37);
    lv_obj_set_style_bg_color(rule, lv_color_hex(0x18232B), 0);
    lv_obj_set_style_border_width(rule, 0, 0);
    lv_obj_set_style_pad_all(rule, 0, 0);

    lv_obj_t *legend_weight = lv_label_create(graph_screen);
    lv_label_set_text(legend_weight, "● Weight");
    lv_obj_set_style_text_font(legend_weight, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(legend_weight, lv_color_hex(0xF2B94F), 0);
    lv_obj_align(legend_weight, LV_ALIGN_TOP_LEFT, 62, 43);
    lv_obj_t *legend_flow = lv_label_create(graph_screen);
    lv_label_set_text(legend_flow, "● Flow (g/s)");
    lv_obj_set_style_text_font(legend_flow, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(legend_flow, lv_color_hex(0x22AEEF), 0);
    lv_obj_align(legend_flow, LV_ALIGN_TOP_LEFT, 142, 43);

    lv_obj_t *chart_card = make_panel(graph_screen, 460, 185, 0x05090C);
    lv_obj_align(chart_card, LV_ALIGN_TOP_LEFT, 8, 58);
    lv_obj_set_style_border_width(chart_card, 1, 0);
    lv_obj_set_style_border_color(chart_card, lv_color_hex(0x162630), 0);

    const char *y_ticks[] = {"500", "375", "250", "125", "0"};
    const lv_coord_t y_pos[] = {5, 32, 59, 86, 113};
    for (uint8_t i = 0; i < 5; ++i) {
        lv_obj_t *tick = lv_label_create(chart_card);
        chart_weight_labels[i] = tick;
        lv_label_set_text(tick, y_ticks[i]);
        lv_obj_set_width(tick, 32);
        lv_obj_set_style_text_align(tick, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_style_text_font(tick, &lv_font_montserrat_10, 0);
        lv_obj_set_style_text_color(tick, lv_color_hex(0x91A1AA), 0);
        lv_obj_align(tick, LV_ALIGN_TOP_LEFT, 0, y_pos[i]);
    }

    weight_chart = lv_chart_create(chart_card);
    lv_obj_set_size(weight_chart, 315, 128);
    lv_obj_align(weight_chart, LV_ALIGN_TOP_LEFT, 37, 10);
    lv_chart_set_type(weight_chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(weight_chart, 60);
    lv_chart_set_range(weight_chart, LV_CHART_AXIS_PRIMARY_Y, 0, 600);
    lv_chart_set_range(weight_chart, LV_CHART_AXIS_SECONDARY_Y, 0, 100);
    lv_chart_set_div_line_count(weight_chart, 5, 7);
    lv_obj_set_style_bg_color(weight_chart, lv_color_hex(0x04080B), 0);
    lv_obj_set_style_border_width(weight_chart, 0, 0);
    lv_obj_set_style_line_color(weight_chart, lv_color_hex(0x17252E), LV_PART_MAIN);
    lv_obj_set_style_size(weight_chart, 0, LV_PART_INDICATOR);
    weight_chart_series = lv_chart_add_series(weight_chart, lv_color_hex(0xF2B94F),
                                               LV_CHART_AXIS_PRIMARY_Y);
    flow_chart_series = lv_chart_add_series(weight_chart, lv_color_hex(0x22AEEF),
                                            LV_CHART_AXIS_SECONDARY_Y);

    const lv_coord_t x_pos[] = {35, 128, 221, 314};
    for (uint8_t i = 0; i < 4; ++i) {
        chart_time_labels[i] = lv_label_create(chart_card);
        lv_label_set_text(chart_time_labels[i], "0:00");
        lv_obj_set_style_text_font(chart_time_labels[i], &lv_font_montserrat_10, 0);
        lv_obj_set_style_text_color(chart_time_labels[i], lv_color_hex(0x91A1AA), 0);
        lv_obj_align(chart_time_labels[i], LV_ALIGN_TOP_LEFT, x_pos[i], 143);
    }
    lv_obj_t *axis_title = lv_label_create(chart_card);
    lv_label_set_text(axis_title, "TIME");
    lv_obj_set_style_text_font(axis_title, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(axis_title, lv_color_hex(0x647681), 0);
    lv_obj_align(axis_title, LV_ALIGN_BOTTOM_MID, -42, -4);
    chart_session_reset();

    lv_obj_t *readout = make_panel(graph_screen, 94, 185, 0x091016);
    lv_obj_align(readout, LV_ALIGN_TOP_RIGHT, -12, 58);
    lv_obj_set_style_border_width(readout, 1, 0);
    lv_obj_set_style_border_color(readout, lv_color_hex(0x1B2A33), 0);
    graph_weight_label = lv_label_create(readout);
    lv_label_set_text(graph_weight_label, "0.0 g");
    lv_obj_set_width(graph_weight_label, 90);
    lv_obj_set_style_text_align(graph_weight_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(graph_weight_label, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_color(graph_weight_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(graph_weight_label, LV_ALIGN_TOP_MID, 0, 14);
    graph_flow_label = lv_label_create(readout);
    lv_label_set_text(graph_flow_label, "0.0 g/s");
    lv_obj_set_width(graph_flow_label, 90);
    lv_obj_set_style_text_align(graph_flow_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(graph_flow_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(graph_flow_label, lv_color_hex(0x22AEEF), 0);
    lv_obj_align(graph_flow_label, LV_ALIGN_TOP_MID, 0, 72);
    graph_timer_label = lv_label_create(readout);
    lv_label_set_text(graph_timer_label, "00:00.0");
    lv_obj_set_width(graph_timer_label, 90);
    lv_obj_set_style_text_align(graph_timer_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(graph_timer_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(graph_timer_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(graph_timer_label, LV_ALIGN_TOP_MID, 0, 132);

    lv_obj_t *tare = lv_btn_create(graph_screen);
    lv_obj_set_size(tare, 221, 58);
    lv_obj_align(tare, LV_ALIGN_BOTTOM_LEFT, 8, -9);
    lv_obj_set_style_radius(tare, 12, 0);
    lv_obj_set_style_bg_color(tare, lv_color_hex(0x34414A), 0);
    lv_obj_set_style_bg_color(tare, lv_color_hex(0x465660), LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(tare, 0, 0);
    lv_obj_add_event_cb(tare, tare_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *tare_label = lv_label_create(tare);
    lv_label_set_text(tare_label, LV_SYMBOL_REFRESH "  TARE");
    lv_obj_set_style_text_font(tare_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(tare_label, lv_color_hex(0xEAF0F3), 0);
    lv_obj_center(tare_label);

    lv_obj_t *brew = lv_btn_create(graph_screen);
    lv_obj_set_size(brew, 221, 58);
    lv_obj_align(brew, LV_ALIGN_BOTTOM_RIGHT, -8, -9);
    lv_obj_set_style_radius(brew, 12, 0);
    lv_obj_set_style_bg_color(brew, lv_color_hex(0xF2B94F), 0);
    lv_obj_set_style_bg_color(brew, lv_color_hex(0xD99A31), LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(brew, 0, 0);
    lv_obj_add_event_cb(brew, brew_event_cb, LV_EVENT_SHORT_CLICKED, NULL);
    lv_obj_add_event_cb(brew, brew_event_cb, LV_EVENT_LONG_PRESSED, NULL);
    graph_brew_button_label = lv_label_create(brew);
    lv_label_set_text(graph_brew_button_label, LV_SYMBOL_PLAY "  START");
    lv_obj_set_style_text_font(graph_brew_button_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(graph_brew_button_label, lv_color_hex(0x111619), 0);
    lv_obj_center(graph_brew_button_label);
}

static lv_obj_t *dashboard_stat_label(lv_obj_t *parent, const char *heading,
                                      lv_coord_t x, lv_coord_t width)
{
    lv_obj_t *card = make_panel(parent, width, 57, 0x091117);
    lv_obj_align(card, LV_ALIGN_TOP_LEFT, x, 43);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(0x203641), 0);
    lv_obj_t *title = lv_label_create(card);
    lv_label_set_text(title, heading);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0x9EB0BA), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 8, 4);
    lv_obj_t *value = lv_label_create(card);
    lv_obj_set_width(value, width - 16);
    lv_obj_set_style_text_align(value, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(value, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(value, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(value, LV_ALIGN_BOTTOM_MID, 0, -4);
    return value;
}

static void create_dashboard_screen(void)
{
    dashboard_screen = lv_tileview_add_tile(page_tileview, 2, 0, LV_DIR_LEFT);
    lv_obj_set_style_bg_color(dashboard_screen, lv_color_hex(0x030608), 0);
    lv_obj_set_style_bg_opa(dashboard_screen, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(dashboard_screen, 2, 0);
    lv_obj_set_style_border_color(dashboard_screen, lv_color_hex(0x314753), 0);
    lv_obj_set_style_radius(dashboard_screen, 16, 0);
    lv_obj_clear_flag(dashboard_screen, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *menu = lv_btn_create(dashboard_screen);
    lv_obj_set_size(menu, 42, 34);
    lv_obj_align(menu, LV_ALIGN_TOP_LEFT, 5, 2);
    lv_obj_set_style_bg_opa(menu, LV_OPA_TRANSP, 0);
    lv_obj_set_style_shadow_width(menu, 0, 0);
    lv_obj_add_event_cb(menu, menu_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *menu_icon = lv_label_create(menu);
    lv_label_set_text(menu_icon, LV_SYMBOL_LIST);
    lv_obj_set_style_text_font(menu_icon, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(menu_icon, lv_color_hex(0xC8D4DA), 0);
    lv_obj_center(menu_icon);

    dashboard_recipe_label = lv_label_create(dashboard_screen);
    lv_label_set_text(dashboard_recipe_label, pourbot_recipe_active()->name);
    lv_obj_set_width(dashboard_recipe_label, 175);
    lv_obj_set_style_text_font(dashboard_recipe_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(dashboard_recipe_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(dashboard_recipe_label, LV_ALIGN_TOP_LEFT, 52, 7);
    dashboard_recipe_detail_label = lv_label_create(dashboard_screen);
    lv_obj_set_width(dashboard_recipe_detail_label, 225);
    lv_obj_set_style_text_align(dashboard_recipe_detail_label, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_font(dashboard_recipe_detail_label, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(dashboard_recipe_detail_label, lv_color_hex(0xD5E0E5), 0);
    lv_obj_align(dashboard_recipe_detail_label, LV_ALIGN_TOP_RIGHT, -65, 9);
    add_page_dots(dashboard_screen, 1);
    add_battery_label(dashboard_screen, 1);

    dashboard_weight_label = dashboard_stat_label(dashboard_screen, "WEIGHT", 12, 145);
    dashboard_flow_label = dashboard_stat_label(dashboard_screen, "FLOW", 164, 145);
    dashboard_timer_label = dashboard_stat_label(dashboard_screen, "ELAPSED TIME", 316, 152);
    lv_label_set_text(dashboard_weight_label, "0.0 g");
    lv_label_set_text(dashboard_flow_label, "0.0 g/s");
    lv_label_set_text(dashboard_timer_label, "00:00.0");

    lv_obj_t *chart_card = make_panel(dashboard_screen, 294, 139, 0x05090C);
    lv_obj_align(chart_card, LV_ALIGN_TOP_LEFT, 12, 107);
    lv_obj_set_style_border_width(chart_card, 1, 0);
    lv_obj_set_style_border_color(chart_card, lv_color_hex(0x203641), 0);
    lv_obj_t *legend = lv_label_create(chart_card);
    lv_label_set_recolor(legend, true);
    lv_label_set_text(legend, "#F2B94F ● Weight#   #22AEEF ● Flow#");
    lv_obj_set_style_text_font(legend, &lv_font_montserrat_10, 0);
    lv_obj_align(legend, LV_ALIGN_TOP_LEFT, 36, 2);
    const lv_coord_t y_pos[] = {18, 38, 58, 78, 98};
    for (uint8_t i = 0; i < 5; ++i) {
        dashboard_weight_labels[i] = lv_label_create(chart_card);
        lv_obj_set_width(dashboard_weight_labels[i], 29);
        lv_obj_set_style_text_align(dashboard_weight_labels[i], LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_style_text_font(dashboard_weight_labels[i], &lv_font_montserrat_10, 0);
        lv_obj_set_style_text_color(dashboard_weight_labels[i], lv_color_hex(0x91A1AA), 0);
        lv_obj_align(dashboard_weight_labels[i], LV_ALIGN_TOP_LEFT, 0, y_pos[i]);
    }
    dashboard_chart = lv_chart_create(chart_card);
    lv_obj_set_size(dashboard_chart, 244, 90);
    lv_obj_align(dashboard_chart, LV_ALIGN_TOP_LEFT, 35, 22);
    lv_chart_set_type(dashboard_chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(dashboard_chart, 60);
    lv_chart_set_div_line_count(dashboard_chart, 5, 7);
    lv_obj_set_style_bg_color(dashboard_chart, lv_color_hex(0x04080B), 0);
    lv_obj_set_style_border_width(dashboard_chart, 0, 0);
    lv_obj_set_style_line_color(dashboard_chart, lv_color_hex(0x17252E), LV_PART_MAIN);
    lv_obj_set_style_size(dashboard_chart, 0, LV_PART_INDICATOR);
    dashboard_weight_series = lv_chart_add_series(dashboard_chart, lv_color_hex(0xF2B94F),
                                                    LV_CHART_AXIS_PRIMARY_Y);
    dashboard_flow_series = lv_chart_add_series(dashboard_chart, lv_color_hex(0x22AEEF),
                                                 LV_CHART_AXIS_SECONDARY_Y);
    const lv_coord_t x_pos[] = {34, 107, 180, 251};
    for (uint8_t i = 0; i < 4; ++i) {
        dashboard_time_labels[i] = lv_label_create(chart_card);
        lv_label_set_text(dashboard_time_labels[i], "0:00");
        lv_obj_set_style_text_font(dashboard_time_labels[i], &lv_font_montserrat_10, 0);
        lv_obj_set_style_text_color(dashboard_time_labels[i], lv_color_hex(0x91A1AA), 0);
        lv_obj_align(dashboard_time_labels[i], LV_ALIGN_TOP_LEFT, x_pos[i], 115);
    }

    lv_obj_t *summary = make_panel(dashboard_screen, 156, 139, 0x091117);
    lv_obj_align(summary, LV_ALIGN_TOP_RIGHT, -12, 107);
    lv_obj_set_style_border_width(summary, 1, 0);
    lv_obj_set_style_border_color(summary, lv_color_hex(0x203641), 0);
    const char *summary_names[] = {"TARGET", "CURRENT", "REMAINING", "AVG FLOW"};
    lv_obj_t **summary_values[] = {&dashboard_target_label, &dashboard_current_label,
                                   &dashboard_remaining_label, &dashboard_average_label};
    for (uint8_t i = 0; i < 4; ++i) {
        lv_obj_t *name = lv_label_create(summary);
        lv_label_set_text(name, summary_names[i]);
        lv_obj_set_style_text_font(name, &lv_font_montserrat_10, 0);
        lv_obj_set_style_text_color(name, lv_color_hex(0x9EB0BA), 0);
        lv_obj_align(name, LV_ALIGN_TOP_LEFT, 8, 9 + i * 31);
        *summary_values[i] = lv_label_create(summary);
        lv_obj_set_width(*summary_values[i], 76);
        lv_obj_set_style_text_align(*summary_values[i], LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_style_text_font(*summary_values[i], &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(*summary_values[i], lv_color_hex(0xFFFFFF), 0);
        lv_obj_align(*summary_values[i], LV_ALIGN_TOP_RIGHT, -8, 8 + i * 31);
    }

    lv_obj_t *tare = lv_btn_create(dashboard_screen);
    lv_obj_set_size(tare, 155, 58);
    lv_obj_align(tare, LV_ALIGN_BOTTOM_LEFT, 77, -9);
    lv_obj_set_style_radius(tare, 12, 0);
    lv_obj_set_style_bg_color(tare, lv_color_hex(0x34414A), 0);
    lv_obj_set_style_shadow_width(tare, 0, 0);
    lv_obj_add_event_cb(tare, tare_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *tare_text = lv_label_create(tare);
    lv_label_set_text(tare_text, "TARE");
    lv_obj_set_style_text_font(tare_text, &lv_font_montserrat_14, 0);
    lv_obj_center(tare_text);
    lv_obj_t *brew = lv_btn_create(dashboard_screen);
    lv_obj_set_size(brew, 155, 58);
    lv_obj_align(brew, LV_ALIGN_BOTTOM_RIGHT, -77, -9);
    lv_obj_set_style_radius(brew, 12, 0);
    lv_obj_set_style_bg_color(brew, lv_color_hex(0xF2B94F), 0);
    lv_obj_set_style_bg_color(brew, lv_color_hex(0xD99A31), LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(brew, 0, 0);
    lv_obj_add_event_cb(brew, brew_event_cb, LV_EVENT_SHORT_CLICKED, NULL);
    lv_obj_add_event_cb(brew, brew_event_cb, LV_EVENT_LONG_PRESSED, NULL);
    dashboard_brew_button_label = lv_label_create(brew);
    lv_label_set_text(dashboard_brew_button_label, LV_SYMBOL_PLAY "  START");
    lv_obj_set_style_text_font(dashboard_brew_button_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(dashboard_brew_button_label, lv_color_hex(0x111619), 0);
    lv_obj_center(dashboard_brew_button_label);
    chart_session_reset();
}

static void update_ui(bool connected, int32_t relative)
{
    if (standby_active) return;
    /* The HX711 task remains free to sample/filter at full speed. LVGL only
     * needs a new frame about every 33 ms; limiting redraw pressure leaves
     * enough CPU/DMA time for finger-tracked Tileview scrolling. */
    const int64_t draw_now_us = esp_timer_get_time();
    if (draw_now_us - last_ui_draw_us < 33333) return;
    last_ui_draw_us = draw_now_us;
    if (!bsp_display_lock(1000)) {
        return;
    }

    if (connected) {
        const pourbot_recipe_t *recipe = pourbot_recipe_active();
        const uint16_t target_g = pourbot_recipe_target_g(recipe);
        lv_label_set_text(recipe_title_label, recipe->name);
        if (graph_recipe_label) lv_label_set_text(graph_recipe_label, recipe->name);
        if (dashboard_recipe_label) lv_label_set_text(dashboard_recipe_label, recipe->name);
        if (dashboard_recipe_detail_label) {
            lv_label_set_text_fmt(dashboard_recipe_detail_label, "DOSE %u g   1:%u.%u   TARGET %u g",
                                  recipe->dose_g, recipe->ratio_x10 / 10,
                                  recipe->ratio_x10 % 10, target_g);
        }
        if (dashboard_target_label) lv_label_set_text_fmt(dashboard_target_label, "%u g", target_g);
        if (recipe_dose_value_label) {
            lv_label_set_text_fmt(recipe_dose_value_label, "%u.0 g", recipe->dose_g);
        }
        if (recipe_target_value_label) {
            lv_label_set_text_fmt(recipe_target_value_label, "%u g\n1:%u.%u", target_g,
                                  recipe->ratio_x10 / 10, recipe->ratio_x10 % 10);
        }
        if (calibration_factor != 0.0f) {
            const float grams = (float)relative / calibration_factor;
            const int32_t measured_tenths = (int32_t)(grams >= 0.0f
                ? grams * 10.0f + 0.5f : grams * 10.0f - 0.5f);
            /* The stationary display hold must not latch a residual reading
             * after unloading. Round stationary readings below 0.25 g to
             * zero; release at 0.30 g to prevent zero chatter.
             * This changes display rounding only, never the tare offset. */
            const float abs_grams = grams < 0.0f ? -grams : grams;
            const int64_t output_now_us = esp_timer_get_time();
            if (stable_weight_tenths == INT32_MIN || scale_is_moving) {
                pending_weight_tenths = INT32_MIN;
            }
            if ((!scale_is_moving && abs_grams < 0.25f) ||
                (stable_weight_tenths == 0 && !scale_is_moving && abs_grams < 0.30f)) {
                stable_weight_tenths = 0;
                pending_weight_tenths = INT32_MIN;
            } else if (stable_weight_tenths == INT32_MIN || scale_is_moving ||
                grams > ((float)stable_weight_tenths / 10.0f) + 0.50f ||
                grams < ((float)stable_weight_tenths / 10.0f) - 0.50f) {
                stable_weight_tenths = measured_tenths;
                pending_weight_tenths = INT32_MIN;
            } else if (grams > ((float)stable_weight_tenths / 10.0f) + 0.10f ||
                       grams < ((float)stable_weight_tenths / 10.0f) - 0.10f) {
                /* Small stationary changes must persist, not be hidden by a
                 * half-gram display latch. Movement still updates immediately. */
                if (pending_weight_tenths != measured_tenths) {
                    pending_weight_tenths = measured_tenths;
                    pending_weight_since_us = output_now_us;
                } else if (output_now_us - pending_weight_since_us >= 250000) {
                    stable_weight_tenths = measured_tenths;
                    pending_weight_tenths = INT32_MIN;
                }
            } else {
                pending_weight_tenths = INT32_MIN;
            }
            const float shown_grams = (float)stable_weight_tenths / 10.0f;
            char weight_text[20];
            format_fixed(weight_text, sizeof(weight_text), shown_grams, 1);
            lv_label_set_text(weight_label, weight_text);
            lv_label_set_text(weight_unit_label, "g");
            if (graph_weight_label) {
                lv_label_set_text_fmt(graph_weight_label, "%s g", weight_text);
            }
            if (dashboard_weight_label) lv_label_set_text_fmt(dashboard_weight_label, "%s g", weight_text);
            if (dashboard_current_label) lv_label_set_text_fmt(dashboard_current_label, "%s g", weight_text);
        } else {
            lv_label_set_text_fmt(weight_label, "%" PRId32, relative);
            lv_label_set_text(weight_unit_label, "ct");
            if (graph_weight_label) {
                lv_label_set_text_fmt(graph_weight_label, "%" PRId32 " ct", relative);
            }
            if (dashboard_weight_label) lv_label_set_text_fmt(dashboard_weight_label, "%" PRId32 " ct", relative);
        }
        lv_label_set_text(status_label, "");
        const float grams_flow = scale_flow_gps;
        if (calibration_factor != 0.0f) {
            float grams = (float)relative / calibration_factor;
            const float shown_grams = stable_weight_tenths == INT32_MIN
                ? grams : (float)stable_weight_tenths / 10.0f;
            int64_t elapsed = brew_elapsed_us;
            if (brew_running) elapsed += esp_timer_get_time() - brew_started_us;
            char flow_text[20];
            format_fixed(flow_text, sizeof(flow_text), grams_flow, 1);
            lv_label_set_text_fmt(flow_label, "FLOW  %s g/s", flow_text);
            if (graph_flow_label) {
                lv_label_set_text_fmt(graph_flow_label, "%s g/s", flow_text);
            }
            if (dashboard_flow_label) lv_label_set_text_fmt(dashboard_flow_label, "%s g/s", flow_text);
            if (dashboard_remaining_label) {
                float remaining = target_g - shown_grams;
                if (remaining < 0.0f) remaining = 0.0f;
                char remaining_text[20];
                format_fixed(remaining_text, sizeof(remaining_text), remaining, 1);
                lv_label_set_text_fmt(dashboard_remaining_label, "%s g", remaining_text);
            }
            if (dashboard_average_label) {
                const float average_flow = elapsed > 0 && shown_grams > 0.0f
                    ? shown_grams / ((float)elapsed / 1000000.0f) : 0.0f;
                char average_text[20];
                format_fixed(average_text, sizeof(average_text), average_flow, 1);
                lv_label_set_text_fmt(dashboard_average_label, "%s g/s", average_text);
            }
            lv_arc_set_range(progress_panel, 0, target_g);
            lv_arc_set_value(progress_panel, (int)(shown_grams < 0 ? 0 :
                (shown_grams > target_g ? target_g : shown_grams)));
            lv_arc_set_value(flow_bar, (int)(grams_flow * 100.0f) > 1000 ? 1000 : (int)(grams_flow * 100.0f));
            uint32_t flow_bar_color = grams_flow < FLOW_OPTIMAL_MIN_GPS
                ? 0xF2B94F
                : (grams_flow <= FLOW_OPTIMAL_MAX_GPS ? 0x4ADE80 : 0xEF4444);
            lv_obj_set_style_arc_color(flow_bar, lv_color_hex(flow_bar_color),
                                      LV_PART_INDICATOR);
            const int64_t chart_now_us = esp_timer_get_time();
            if (brew_running) {
                if (!chart_recording && grams_flow >= 0.35f) {
                    chart_recording = true;
                    chart_start_elapsed_us = elapsed;
                    chart_last_sample_us = 0;
                }
                if (chart_recording &&
                    (chart_last_sample_us == 0 ||
                     chart_now_us - chart_last_sample_us >= chart_sample_interval_us)) {
                    /* Keep the whole session in bounded memory. Coarsen older
                     * samples rather than halting after ten minutes. */
                    if (chart_history_count >= CHART_HISTORY_MAX) {
                        for (uint16_t i = 0; i < CHART_HISTORY_MAX / 2; ++i) {
                            chart_weight_history[i] = chart_weight_history[i * 2];
                            chart_flow_history[i] = chart_flow_history[i * 2];
                            chart_time_history[i] = chart_time_history[i * 2];
                        }
                        chart_history_count = CHART_HISTORY_MAX / 2;
                        if (chart_sample_interval_us < 60000000) chart_sample_interval_us *= 2;
                    }
                    int chart_value = (int)(shown_grams + 0.5f);
                    if (chart_value < 0) chart_value = 0;
                    int flow_chart_value = (int)(grams_flow * 10.0f + 0.5f);
                    if (flow_chart_value < 0) flow_chart_value = 0;
                    if (chart_value >= chart_weight_max)
                        chart_weight_max = ((chart_value + chart_value / 10 + 99) / 100) * 100;
                    if (flow_chart_value >= chart_flow_max)
                        chart_flow_max = ((flow_chart_value + flow_chart_value / 10 + 49) / 50) * 50;
                    chart_weight_history[chart_history_count] = chart_value;
                    chart_flow_history[chart_history_count] = flow_chart_value;
                    chart_time_history[chart_history_count] =
                        (uint32_t)((elapsed - chart_start_elapsed_us) / 1000000);
                    chart_history_count++;
                    chart_last_sample_us = chart_now_us;
                    uint32_t graph_seconds = (uint32_t)((elapsed - chart_start_elapsed_us) /
                                                        1000000);
                    chart_render(graph_seconds);
                }
            }
        } else {
            lv_label_set_text(flow_label, "FLOW  0 ct/s");
            if (graph_flow_label) lv_label_set_text(graph_flow_label, "0 ct/s");
            if (dashboard_flow_label) lv_label_set_text(dashboard_flow_label, "0 ct/s");
            lv_arc_set_value(flow_bar, 0);
        }
        if (!tare_requested) {
            lv_label_set_text(tare_button_label, LV_SYMBOL_REFRESH "  TARE");
            lv_obj_clear_state(tare_button, LV_STATE_DISABLED);
        }
    } else {
        lv_label_set_text(status_label, "NO SCALE");
        if (graph_weight_label) lv_label_set_text(graph_weight_label, "-- g");
        if (graph_flow_label) lv_label_set_text(graph_flow_label, "0.0 g/s");
        if (dashboard_weight_label) lv_label_set_text(dashboard_weight_label, "-- g");
        if (dashboard_flow_label) lv_label_set_text(dashboard_flow_label, "0.0 g/s");
    }
    bsp_display_unlock();
}

static void scale_task(void *argument)
{
    (void)argument;
    /* Discard initial conversions after cold boot or deep-sleep wake before
     * establishing zero. Keep this separate from the fast live-reading path. */
    for (int i = 0; !boot_scale_ready && i < 10; ++i) {
        int32_t discarded;
        hx711_read(&discarded);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    int32_t last_reading = 0;
    int32_t samples[3] = {0};
    int sample_index = 0;
    int32_t filtered_relative = 0;
    bool filter_initialized = false;
    int64_t last_sample_us = 0;
    int64_t moving_until_us = 0;
    int movement_votes = 0;
    int movement_direction = 0;
    float flow_samples[3] = {0};
    int flow_count = 0;
    int flow_index = 0;

    while (true) {
        /* Never clock the HX711 after the standby task powers it down. */
        if (standby_active) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (calibration_tare_requested) {
            int32_t new_offset;
            bool ok = hx711_average(&new_offset, 24);
            if (ok) {
                tare_offset = new_offset;
                stable_weight_tenths = 0;
                filter_initialized = false;
                pending_weight_tenths = INT32_MIN;
                sample_index = 0;
                flow_count = 0;
                flow_index = 0;
                scale_flow_gps = 0.0f;
                scale_is_moving = false;
                movement_votes = 0;
                movement_direction = 0;
                moving_until_us = 0;
            }
            calibration_tare_requested = false;
            if (bsp_display_lock(1000)) {
                if (calibration_status_label) {
                    lv_label_set_text(calibration_status_label,
                        ok ? "TARED - PLACE KNOWN WEIGHT" : "TARE FAILED - TRY AGAIN");
                    lv_obj_clear_state(calibration_tare_button, LV_STATE_DISABLED);
                    lv_obj_clear_state(calibration_run_button, LV_STATE_DISABLED);
                    lv_obj_clear_state(calibration_weight_button, LV_STATE_DISABLED);
                    lv_obj_set_style_bg_color(calibration_tare_button,
                        lv_color_hex(ok ? 0x16A34A : 0x475569), 0);
                    lv_obj_set_style_text_color(calibration_status_label,
                        lv_color_hex(ok ? 0x4ADE80 : 0xF87171), 0);
                }
                bsp_display_unlock();
            }
        }
        if (calibration_run_requested) {
            int32_t loaded_raw = tare_offset;
            bool ok = hx711_average(&loaded_raw, 12);
            int32_t delta = loaded_raw - tare_offset;
            if (delta < 0) delta = -delta;
            ok = ok && delta > 100 && calibration_known_grams > 0;
            if (ok) {
                calibration_factor = (float)(loaded_raw - tare_offset) /
                                     (float)calibration_known_grams;
                save_calibration_factor();
            }
            calibration_run_requested = false;
            if (bsp_display_lock(1000)) {
                if (calibration_status_label) {
                    lv_label_set_text(calibration_status_label,
                        ok ? "SAVED - SCALE NOW READS GRAMS" : "FAILED - TARE, THEN ADD WEIGHT");
                    lv_obj_clear_state(calibration_tare_button, LV_STATE_DISABLED);
                    lv_obj_clear_state(calibration_run_button, LV_STATE_DISABLED);
                    lv_obj_clear_state(calibration_weight_button, LV_STATE_DISABLED);
                    lv_obj_set_style_bg_color(calibration_run_button,
                        lv_color_hex(ok ? 0x16A34A : 0x475569), 0);
                    lv_obj_set_style_text_color(calibration_status_label,
                        lv_color_hex(ok ? 0x4ADE80 : 0xF87171), 0);
                }
                bsp_display_unlock();
            }
        }
        if (tare_requested) {
            int32_t new_offset;
            /* Shorter routine tare; retain fresh-conversion flushing and the
             * trimmed mean. Boot/calibration keep their longer acquisition. */
            if (hx711_average(&new_offset, 16)) {
                tare_offset = new_offset;
                stable_weight_tenths = 0;
                filter_initialized = false;
                pending_weight_tenths = INT32_MIN;
                sample_index = 0;
                flow_count = 0;
                flow_index = 0;
                scale_flow_gps = 0.0f;
                scale_is_moving = false;
                movement_votes = 0;
                movement_direction = 0;
                moving_until_us = 0;
                tare_requested = false;
                ESP_LOGI(TAG, "Tare offset: %" PRId32, tare_offset);
            } else {
                update_ui(false, last_reading - tare_offset);
                vTaskDelay(pdMS_TO_TICKS(250));
                continue;
            }
        }

        if (hx711_read(&last_reading)) {
            const int32_t raw_relative = last_reading - tare_offset;
            const int64_t now_us = esp_timer_get_time();
            if (!filter_initialized) {
                filtered_relative = raw_relative;
                samples[0] = samples[1] = samples[2] = raw_relative;
                sample_index = 0;
                filter_initialized = true;
                last_sample_us = now_us;
            } else {
                /* Reject isolated ADC/serial outliers BEFORE detecting motion.
                 * A large raw spike must not bypass the median filter. Real
                 * steps pass as soon as two of the three readings agree. */
                samples[sample_index] = raw_relative;
                sample_index = (sample_index + 1) % 3;
                const int32_t candidate = median3(samples[0], samples[1], samples[2]);
                float abs_factor = calibration_factor < 0.0f
                    ? -calibration_factor : calibration_factor;
                if (abs_factor < 1.0f) abs_factor = 1.0f;
                int32_t raw_delta = candidate - filtered_relative;
                float change_g = (float)(raw_delta < 0 ? -raw_delta : raw_delta) / abs_factor;

                const int direction = raw_delta > 0 ? 1 : (raw_delta < 0 ? -1 : 0);
                if (change_g >= 0.25f) {
                    if (direction == movement_direction) {
                        movement_votes++;
                    } else {
                        movement_direction = direction;
                        movement_votes = 1;
                    }
                } else {
                    movement_votes = 0;
                    movement_direction = 0;
                }
                if (change_g > 5.0f || movement_votes >= 3) {
                    moving_until_us = now_us + 450000;
                }
                scale_is_moving = now_us < moving_until_us;

                const int32_t previous_filtered = filtered_relative;
                if (scale_is_moving) {
                    filtered_relative = candidate;
                } else {
                    /* Once stationary, use a short IIR low-pass filter. It
                     * removes conversion noise without delaying real pours,
                     * because movement immediately switches back to median-3. */
                    filtered_relative += (candidate - filtered_relative) / 8;
                }

                const float dt = (float)(now_us - last_sample_us) / 1000000.0f;
                if (dt >= 0.02f && dt < 1.0f && calibration_factor != 0.0f) {
                    float rate = ((float)(filtered_relative - previous_filtered) /
                                  calibration_factor) / dt;
                    if (rate < 0.0f || rate < 0.35f) rate = 0.0f;
                    flow_samples[flow_index] = rate;
                    flow_index = (flow_index + 1) % 3;
                    if (flow_count < 3) flow_count++;
                    float total = 0.0f;
                    for (int i = 0; i < flow_count; ++i) total += flow_samples[i];
                    scale_flow_gps = total / (float)flow_count;
                    if (!scale_is_moving && scale_flow_gps < 0.5f) scale_flow_gps = 0.0f;
                }
                last_sample_us = now_us;
            }
            update_ui(true, filtered_relative);
        } else {
            update_ui(false, filter_initialized
                ? filtered_relative : last_reading - tare_offset);
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
}

void app_main(void)
{
    /* West Warwick local time, including US daylight-saving transitions. */
    setenv("TZ", "EST5EDT,M3.2.0/2,M11.1.0/2", 1);
    tzset();
    /* Hold the backlight pin low before the LCD/LEDC drivers claim it. This
     * prevents uninitialized panel RAM from appearing as startup static. */
    gpio_reset_pin(EXAMPLE_PIN_NUM_QSPI_BL);
    gpio_set_direction(EXAMPLE_PIN_NUM_QSPI_BL, GPIO_MODE_OUTPUT);
    gpio_set_level(EXAMPLE_PIN_NUM_QSPI_BL, 0);

    gpio_config_t input_config = {
        .pin_bit_mask = 1ULL << HX711_DOUT,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&input_config));

    gpio_config_t output_config = {
        .pin_bit_mask = 1ULL << HX711_SCK,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&output_config));
    gpio_set_level(HX711_SCK, 0);

    gpio_config_t wake_button_config = {
        .pin_bit_mask = 1ULL << STANDBY_WAKE_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&wake_button_config));


    /* Load saved recipes and calibration before constructing the first frame. */
    esp_err_t nvs_result = nvs_flash_init();
    if (nvs_result == ESP_OK) {
        pourbot_recipes_load();
        load_calibration_factor();
    } else {
        ESP_LOGE(TAG, "Preference storage startup failed: %s",
                 esp_err_to_name(nvs_result));
    }

    bsp_display_cfg_t display_config = {
        .lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG(),
        .buffer_size = EXAMPLE_LCD_QSPI_H_RES * EXAMPLE_LCD_QSPI_V_RES,
        .rotate = LV_DISP_ROT_90,
    };

    if (!bsp_display_start_with_config(&display_config)) {
        ESP_LOGE(TAG, "Display initialization failed");
        return;
    }
    if (bsp_display_lock(1000)) {
        lv_obj_t *screen = lv_scr_act();
        lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
        lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t *logo = lv_img_create(screen);
        lv_img_set_src(logo, &pourbot_logo);
        lv_obj_center(logo);
        lv_refr_now(NULL);
        bsp_display_unlock();
    }

    /* Let any final panel transfer settle before revealing the first frame. */
    vTaskDelay(pdMS_TO_TICKS(75));
    ESP_ERROR_CHECK(bsp_display_backlight_on());
    /* Initialize and tare while the splash remains visible, not after an
     * apparently ready 0.0 has already been drawn on the main screen. */
    const int64_t splash_started_us = esp_timer_get_time();
    bool conversions_ready = true;
    for (int i = 0; i < 10; ++i) {
        int32_t discarded;
        if (!hx711_read(&discarded)) {
            conversions_ready = false;
            break;
        }
    }
    int32_t boot_offset;
    boot_scale_ready = conversions_ready && hx711_average(&boot_offset, 24);
    if (boot_scale_ready) {
        tare_offset = boot_offset;
        tare_requested = false;
        stable_weight_tenths = 0;
        pending_weight_tenths = INT32_MIN;
        scale_flow_gps = 0.0f;
        scale_is_moving = false;
    } else {
        ESP_LOGW(TAG, "Boot tare failed; showing unavailable weight until scale recovers");
    }
    const int64_t splash_remaining_us = 1925000 -
        (esp_timer_get_time() - splash_started_us);
    if (splash_remaining_us > 0)
        vTaskDelay(pdMS_TO_TICKS((uint32_t)(splash_remaining_us / 1000)));

    if (bsp_display_lock(1000)) {
        lv_obj_clean(lv_scr_act());
        create_ui();
        create_dashboard_screen();
        /* Apply the clean scale layout on boot, even though brewing starts
         * inactive and the visibility flag is initially false. */
        main_brew_metrics_visible = true;
        set_main_brew_metrics_visible(false);
        lv_obj_set_tile(page_tileview, main_screen, LV_ANIM_OFF);
        bsp_display_unlock();
    }


    xTaskCreate(standby_task, "standby_task", 3072, NULL, 3, NULL);
    xTaskCreate(scale_task, "scale_task", 4096, NULL, 5, NULL);
    xTaskCreate(battery_task, "battery_task", 3072, NULL, 2, NULL);
    archive_ready = pour_archive_start();
    if (!pourbot_wifi_start()) ESP_LOGW(TAG, "Wi-Fi task unavailable");
    /* The display, scale UI, workers, and networking have initialized. An OTA
     * image that reached this checkpoint is healthy enough to keep. */
    esp_ota_mark_app_valid_cancel_rollback();
}
