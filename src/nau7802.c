#include "nau7802.h"

#include "battery_gauge.h"
#include "driver/i2c.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define NAU_BUS I2C_NUM_1
#define NAU_ADDRESS 0x2A
#define NAU_TIMEOUT pdMS_TO_TICKS(25)

#define REG_PU_CTRL 0x00
#define REG_CTRL1 0x01
#define REG_CTRL2 0x02
#define REG_ADCO_B2 0x12
#define REG_ADC 0x15
#define REG_PGA 0x1B
#define REG_PGA_PWR 0x1C
#define REG_REVISION 0x1F

#define PU_RR (1U << 0)
#define PU_PUD (1U << 1)
#define PU_PUA (1U << 2)
#define PU_PUR (1U << 3)
#define PU_CR (1U << 5)
#define PU_AVDDS (1U << 7)

static const char *TAG = "nau7802";
static bool initialized;
static bool bus_configured;

static esp_err_t read_reg(uint8_t reg, uint8_t *data, size_t length)
{
    return i2c_master_write_read_device(NAU_BUS, NAU_ADDRESS, &reg, 1,
                                        data, length, NAU_TIMEOUT);
}

static esp_err_t write_reg(uint8_t reg, uint8_t value)
{
    uint8_t data[2] = {reg, value};
    return i2c_master_write_to_device(NAU_BUS, NAU_ADDRESS, data,
                                      sizeof(data), NAU_TIMEOUT);
}

static bool update_reg(uint8_t reg, uint8_t clear_mask, uint8_t set_mask)
{
    uint8_t value;
    if (read_reg(reg, &value, 1) != ESP_OK) return false;
    value = (value & ~clear_mask) | set_mask;
    return write_reg(reg, value) == ESP_OK;
}

static bool wait_for_bits(uint8_t reg, uint8_t mask, bool set,
                          uint32_t timeout_ms)
{
    const int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    do {
        uint8_t value;
        if (read_reg(reg, &value, 1) != ESP_OK) return false;
        if (((value & mask) != 0) == set) return true;
        vTaskDelay(pdMS_TO_TICKS(1));
    } while (esp_timer_get_time() < deadline);
    return false;
}

bool nau7802_init(void)
{
    if (initialized) return true;

    if (!bus_configured) {
        const i2c_config_t config = {
            .mode = I2C_MODE_MASTER,
            .sda_io_num = BATTERY_I2C_SDA,
            .scl_io_num = BATTERY_I2C_SCL,
            /* Breakout pull-ups remain primary; these make loose jumper-wire
             * installations more tolerant without affecting either device. */
            .sda_pullup_en = GPIO_PULLUP_ENABLE,
            .scl_pullup_en = GPIO_PULLUP_ENABLE,
            .master.clk_speed = 100000,
        };
        esp_err_t result = i2c_param_config(NAU_BUS, &config);
        if (result != ESP_OK) return false;
        result = i2c_driver_install(NAU_BUS, I2C_MODE_MASTER, 0, 0, 0);
        if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) return false;
        bus_configured = true;
    }

    uint8_t revision;
    if (read_reg(REG_REVISION, &revision, 1) != ESP_OK ||
        (revision & 0x0F) != 0x0F) {
        ESP_LOGE(TAG, "NAU7802 not detected at address 0x2A");
        return false;
    }

    /* SparkFun's proven initialization sequence: reset, power analog and
     * digital sections, 3.3 V LDO, gain 128, 80 SPS, then calibrate the AFE. */
    if (!update_reg(REG_PU_CTRL, 0, PU_RR) ||
        !update_reg(REG_PU_CTRL, PU_RR, 0) ||
        !update_reg(REG_PU_CTRL, 0, PU_PUD | PU_PUA) ||
        !wait_for_bits(REG_PU_CTRL, PU_PUR, true, 250) ||
        !update_reg(REG_CTRL1, 0x3F, (4U << 3) | 7U) ||
        !update_reg(REG_PU_CTRL, 0, PU_AVDDS) ||
        !update_reg(REG_CTRL2, 0x70, (3U << 4)) ||
        !update_reg(REG_ADC, 0, 0x30) ||
        !update_reg(REG_PGA_PWR, 0, 0x80) ||
        !update_reg(REG_PGA, (1U << 6), 0)) {
        ESP_LOGE(TAG, "NAU7802 setup failed");
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(250));

    /* Internal AFE calibration; CALS clears on completion and CAL_ERROR must
     * remain clear. */
    if (!update_reg(REG_CTRL2, 0x03, (1U << 2)) ||
        !wait_for_bits(REG_CTRL2, (1U << 2), false, 1000)) {
        ESP_LOGE(TAG, "NAU7802 AFE calibration timed out");
        return false;
    }
    uint8_t ctrl2;
    if (read_reg(REG_CTRL2, &ctrl2, 1) != ESP_OK || (ctrl2 & (1U << 3))) {
        ESP_LOGE(TAG, "NAU7802 AFE calibration failed");
        return false;
    }
    initialized = true;
    ESP_LOGI(TAG, "NAU7802 ready at 80 SPS, gain 128");
    return true;
}

bool nau7802_reconnect(void)
{
    /* Keep shared controller 1 installed so the MAX17048 is not interrupted.
     * Reset and initialize only the scale converter at address 0x2A. */
    initialized = false;
    return nau7802_init();
}

bool nau7802_read(int32_t *value)
{
    if (!value || !initialized) return false;
    if (!wait_for_bits(REG_PU_CTRL, PU_CR, true, 100)) return false;
    uint8_t data[3];
    if (read_reg(REG_ADCO_B2, data, sizeof(data)) != ESP_OK) return false;
    uint32_t raw = ((uint32_t)data[0] << 16) |
                   ((uint32_t)data[1] << 8) | data[2];
    if (raw & 0x00800000U) raw |= 0xFF000000U;
    *value = (int32_t)raw;
    return true;
}

bool nau7802_average(int32_t *average, int samples)
{
    int32_t readings[32];
    int valid = 0;
    for (int i = 0; i < 2; ++i) {
        int32_t discarded;
        if (!nau7802_read(&discarded)) return false;
    }
    if (samples > 32) samples = 32;
    for (int i = 0; i < samples; ++i) {
        int32_t reading;
        if (nau7802_read(&reading)) readings[valid++] = reading;
    }
    if (valid < (samples * 3 + 3) / 4) return false;
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

void nau7802_power_down(void)
{
    if (!initialized) return;
    update_reg(REG_PU_CTRL, PU_PUD | PU_PUA, 0);
    initialized = false;
}

