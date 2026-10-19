#include "battery_gauge.h"

#include "driver/i2c.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

#define GAUGE_BUS I2C_NUM_1
#define GAUGE_ADDRESS 0x36
#define GAUGE_TIMEOUT pdMS_TO_TICKS(20)
#define REG_VERSION 0x08
#define REG_CRATE 0x16

static const char *TAG = "battery_gauge";
static bool gauge_bus_ready;

static esp_err_t read_register(uint8_t reg, uint8_t *data, size_t length)
{
    return i2c_master_write_read_device(GAUGE_BUS, GAUGE_ADDRESS,
        &reg, 1, data, length, GAUGE_TIMEOUT);
}

esp_err_t battery_gauge_init(void)
{
    if (gauge_bus_ready) return ESP_OK;
    /* nau7802_init installs shared controller 1 before worker tasks start.
     * Reconfiguring that live bus here could interrupt a scale conversion. */
    uint8_t version[2];
    esp_err_t result = read_register(REG_VERSION, version, sizeof(version));
    if (result != ESP_OK) return result;
    const uint16_t id = ((uint16_t)version[0] << 8) | version[1];
    if ((id & 0xFFF0) != 0x0010) return ESP_ERR_NOT_FOUND;
    gauge_bus_ready = true;
    return ESP_OK;
}

esp_err_t battery_gauge_reconnect(void)
{
    /* Controller 1 is shared with the NAU7802. Never delete or reconfigure it
     * just because the fuel gauge missed a read. Probe the gauge again while
     * leaving the scale's bus operational. */
    gauge_bus_ready = false;
    return battery_gauge_init();
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
    /* A reset gauge can briefly report 0% for a clearly non-empty cell.  Do
     * not paint that transient as a dead battery; retry until SOC settles. */
    if (mv < 2500 || mv > 4500 || soc > 110 * 256 ||
        (soc == 0 && mv >= 3500)) {
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
    /* CRATE describes the estimated change in state of charge; it is not a
     * hardware charger-present signal.  Values within roughly +/-1%/h can
     * wander around zero while the scale is running from its battery.  Do not
     * turn that estimator noise into a false charging indicator.  The UI task
     * applies an additional sustained-sample filter. */
    sample->charging = sample->rate_tenths_per_hour >= 20;
    return ESP_OK;
}
