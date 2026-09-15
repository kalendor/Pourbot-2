#include "battery_gauge.h"

#include "driver/i2c.h"
#include "freertos/FreeRTOS.h"

#define GAUGE_BUS I2C_NUM_1
#define GAUGE_ADDRESS 0x36
#define GAUGE_TIMEOUT pdMS_TO_TICKS(20)

esp_err_t battery_gauge_init(void)
{
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
    return i2c_driver_install(GAUGE_BUS, I2C_MODE_MASTER, 0, 0, 0);
}

esp_err_t battery_gauge_read(uint8_t *percent, uint16_t *millivolts)
{
    if (!percent || !millivolts) return ESP_ERR_INVALID_ARG;
    uint8_t reg = 0x08; /* VERSION, MSB first. */
    uint8_t version[2];
    esp_err_t result = i2c_master_write_read_device(GAUGE_BUS, GAUGE_ADDRESS,
        &reg, 1, version, sizeof(version), GAUGE_TIMEOUT);
    if (result != ESP_OK) return result;
    const uint16_t id = ((uint16_t)version[0] << 8) | version[1];
    if ((id & 0xFFF0) != 0x0010) return ESP_ERR_NOT_FOUND;

    reg = 0x02; /* VCELL followed by SOC; preserve gauge learning across boots. */
    uint8_t data[4];
    result = i2c_master_write_read_device(GAUGE_BUS, GAUGE_ADDRESS,
        &reg, 1, data, sizeof(data), GAUGE_TIMEOUT);
    if (result != ESP_OK) return result;
    const uint16_t cell = ((uint16_t)data[0] << 8) | data[1];
    const uint16_t soc = ((uint16_t)data[2] << 8) | data[3];
    /* VCELL LSB = 78.125 uV, SOC LSB = 1/256 percent. */
    const uint32_t mv = ((uint32_t)cell * 625 + 4000) / 8000;
    if (mv < 2500 || mv > 4500 || soc > 110 * 256)
        return ESP_ERR_INVALID_RESPONSE;
    uint16_t rounded = (soc + 128) / 256;
    *percent = rounded > 100 ? 100 : (uint8_t)rounded;
    *millivolts = (uint16_t)mv;
    return ESP_OK;
}
