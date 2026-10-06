#include "battery_gauge.h"

#include <string.h>

#include "driver/i2c.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

#define GAUGE_BUS I2C_NUM_1
#define GAUGE_ADDRESS 0x36
#define GAUGE_TIMEOUT pdMS_TO_TICKS(20)
#define REG_MODE 0x06
#define REG_VERSION 0x08
#define REG_CONFIG 0x0C
#define REG_CRATE 0x16
#define REG_STATUS 0x1A
#define REG_COMMAND 0xFE

static const char *TAG = "battery_gauge";
static bool gauge_bus_ready;
static bool gauge_prepared;

static esp_err_t read_register(uint8_t reg, uint8_t *data, size_t length)
{
    return i2c_master_write_read_device(GAUGE_BUS, GAUGE_ADDRESS,
        &reg, 1, data, length, GAUGE_TIMEOUT);
}

static esp_err_t write_register(uint8_t reg, const uint8_t *data, size_t length)
{
    uint8_t buffer[3];
    if (length > 2) return ESP_ERR_INVALID_ARG;
    buffer[0] = reg;
    memcpy(&buffer[1], data, length);
    return i2c_master_write_to_device(GAUGE_BUS, GAUGE_ADDRESS,
        buffer, length + 1, GAUGE_TIMEOUT);
}

/* Match Adafruit_MAX1704X::begin(): reset the gauge, clear the reset alert,
 * disable forced sleep, and leave the gauge awake for continuous updates. */
static esp_err_t prepare_gauge(void)
{
    const uint8_t reset_command[2] = {0x54, 0x00};
    /* The MAX17048 can NACK because reset occurs before the final ACK. */
    (void)write_register(REG_COMMAND, reset_command, sizeof(reset_command));
    vTaskDelay(pdMS_TO_TICKS(10));

    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        uint8_t status;
        if (read_register(REG_STATUS, &status, 1) == ESP_OK) {
            status &= (uint8_t)~0x01;
            if (write_register(REG_STATUS, &status, 1) == ESP_OK) break;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }

    uint8_t mode;
    esp_err_t result = read_register(REG_MODE, &mode, 1);
    if (result != ESP_OK) return result;
    mode &= (uint8_t)~(1U << 5); /* SLEEPEN = 0. */
    result = write_register(REG_MODE, &mode, 1);
    if (result != ESP_OK) return result;

    uint8_t config[2];
    result = read_register(REG_CONFIG, config, sizeof(config));
    if (result != ESP_OK) return result;
    config[1] &= (uint8_t)~(1U << 7); /* SLEEP = 0 in 16-bit CONFIG. */
    return write_register(REG_CONFIG, config, sizeof(config));
}

esp_err_t battery_gauge_init(void)
{
    if (gauge_prepared) return ESP_OK;
    if (!gauge_bus_ready) {
        const i2c_config_t config = {
            .mode = I2C_MODE_MASTER,
            .sda_io_num = BATTERY_I2C_SDA,
            .scl_io_num = BATTERY_I2C_SCL,
            /* Adafruit #5580 supplies external pull-ups to its VIN (3.3 V). */
            .sda_pullup_en = GPIO_PULLUP_DISABLE,
            .scl_pullup_en = GPIO_PULLUP_DISABLE,
            .master.clk_speed = 100000,
        };
        esp_err_t result = i2c_param_config(GAUGE_BUS, &config);
        if (result != ESP_OK) return result;
        result = i2c_driver_install(GAUGE_BUS, I2C_MODE_MASTER, 0, 0, 0);
        /* This is a dedicated bus. INVALID_STATE means it was already installed
         * by an earlier attempt, so it is safe to continue using it. */
        if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) return result;
        gauge_bus_ready = true;
    }
    uint8_t version[2];
    esp_err_t result = read_register(REG_VERSION, version, sizeof(version));
    if (result != ESP_OK) return result;
    const uint16_t id = ((uint16_t)version[0] << 8) | version[1];
    if ((id & 0xFFF0) != 0x0010) return ESP_ERR_NOT_FOUND;
    result = prepare_gauge();
    if (result == ESP_OK) gauge_prepared = true;
    return result;
}

esp_err_t battery_gauge_read(battery_gauge_sample_t *sample)
{
    if (!sample) return ESP_ERR_INVALID_ARG;
    uint8_t reg = REG_VERSION; /* VERSION, MSB first. */
    uint8_t version[2];
    esp_err_t result = i2c_master_write_read_device(GAUGE_BUS, GAUGE_ADDRESS,
        &reg, 1, version, sizeof(version), GAUGE_TIMEOUT);
    if (result != ESP_OK) return result;
    const uint16_t id = ((uint16_t)version[0] << 8) | version[1];
    if ((id & 0xFFF0) != 0x0010) return ESP_ERR_NOT_FOUND;

    reg = 0x02; /* VCELL, MSB first. */
    uint8_t voltage_data[2];
    result = i2c_master_write_read_device(GAUGE_BUS, GAUGE_ADDRESS,
        &reg, 1, voltage_data, sizeof(voltage_data), GAUGE_TIMEOUT);
    if (result != ESP_OK) return result;

    reg = 0x04; /* SOC, MSB first; preserve gauge learning across boots. */
    uint8_t soc_data[2];
    result = i2c_master_write_read_device(GAUGE_BUS, GAUGE_ADDRESS,
        &reg, 1, soc_data, sizeof(soc_data), GAUGE_TIMEOUT);
    if (result != ESP_OK) return result;

    reg = REG_CRATE; /* Signed charge/discharge rate, MSB first. */
    uint8_t rate_data[2];
    result = i2c_master_write_read_device(GAUGE_BUS, GAUGE_ADDRESS,
        &reg, 1, rate_data, sizeof(rate_data), GAUGE_TIMEOUT);
    if (result != ESP_OK) return result;

    const uint16_t cell = ((uint16_t)voltage_data[0] << 8) | voltage_data[1];
    const uint16_t soc = ((uint16_t)soc_data[0] << 8) | soc_data[1];
    const int16_t rate = (int16_t)(((uint16_t)rate_data[0] << 8) | rate_data[1]);
    /* VCELL LSB = 78.125 uV, SOC LSB = 1/256 percent. */
    const uint32_t mv = ((uint32_t)cell * 625 + 4000) / 8000;
    if (mv < 2500 || mv > 4500 || soc > 110 * 256) {
        ESP_LOGW(TAG,
            "MAX17048 raw VCELL=%02X%02X (%lu mV), SOC=%02X%02X (%u.%02u%%)",
            voltage_data[0], voltage_data[1], (unsigned long)mv,
            soc_data[0], soc_data[1],
            soc / 256, ((soc & 0xFF) * 100) / 256);
        return ESP_ERR_INVALID_RESPONSE;
    }
    uint16_t rounded = (soc + 128) / 256;
    sample->percent = rounded > 100 ? 100 : (uint8_t)rounded;
    sample->millivolts = (uint16_t)mv;
    sample->rate_tenths_per_hour = (int16_t)(((int32_t)rate * 208) / 100);
    /* Ignore tiny positive CRATE noise; 1.0%/hour is clear charging activity. */
    sample->charging = sample->rate_tenths_per_hour >= 10;
    return ESP_OK;
}
