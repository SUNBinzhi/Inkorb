#include "inkorb_battery.h"
#include "bsp/esp-bsp.h"
#include "driver/i2c_master.h"
#include "esp_check.h"

static const char *TAG = "inkorb_battery";
static i2c_master_dev_handle_t s_pmu;

static esp_err_t read_reg(uint8_t reg, uint8_t *value)
{
    return i2c_master_transmit_receive(s_pmu, &reg, 1, value, 1, 100);
}

static esp_err_t update_reg(uint8_t reg, uint8_t mask, uint8_t bits)
{
    uint8_t value;
    ESP_RETURN_ON_ERROR(read_reg(reg, &value), TAG, "PMU read failed");
    uint8_t updated = (value & ~mask) | (bits & mask);
    if (updated == value) return ESP_OK;
    const uint8_t data[] = {reg, updated};
    return i2c_master_transmit(s_pmu, data, sizeof(data), 100);
}

esp_err_t inkorb_battery_init(void)
{
    if (!s_pmu) {
        i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
        if (!bus) return ESP_ERR_INVALID_STATE;
        const i2c_device_config_t cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = 0x34,
            .scl_speed_hz = 100000,
        };
        ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &cfg, &s_pmu),
                            TAG, "PMU attach failed");
    }
    uint8_t chip;
    ESP_RETURN_ON_ERROR(read_reg(0x03, &chip), TAG, "PMU ID read failed");
    if (chip != 0x4A) return ESP_ERR_NOT_SUPPORTED;
    /* XPowers/Waveshare register definitions. Preserve all unrelated bits;
     * do not change power rails, charging current/voltage, or gauge model. */
    ESP_RETURN_ON_ERROR(update_reg(0x68, 1U << 0, 1U << 0), TAG, "Battery detection failed");
    ESP_RETURN_ON_ERROR(update_reg(0x18, 1U << 3, 1U << 3), TAG, "Fuel gauge enable failed");
    /* Large Key2 remains the hardwired 2s PMIC power-on key, but now serves
     * as hold-to-talk while running. Disable its hardware long-press OFF
     * source so recordings longer than four seconds cannot cut the power.
     * Small GPIO0 requests software PMIC shutdown after a two-second hold. */
    ESP_RETURN_ON_ERROR(update_reg(0x27, 0x0F, 0x03), TAG, "Power key timing failed");
    ESP_RETURN_ON_ERROR(update_reg(0x22, 0x03, 0x00), TAG, "Talk key shutdown disable failed");
    uint8_t off_cfg, time_cfg;
    ESP_RETURN_ON_ERROR(read_reg(0x22, &off_cfg), TAG, "Power key readback failed");
    ESP_RETURN_ON_ERROR(read_reg(0x27, &time_cfg), TAG, "Power key timing readback failed");
    if ((off_cfg & 0x03) != 0x00 || (time_cfg & 0x0F) != 0x03) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    ESP_LOGI(TAG, "PWR hardware ready: large 2s ON / long OFF disabled for talk (22=%02x, 27=%02x)",
             off_cfg, time_cfg);
    ESP_LOGI(TAG, "AXP2101 fuel gauge ready");
    return ESP_OK;
}

esp_err_t inkorb_battery_shutdown(void)
{
    if (!s_pmu) return ESP_ERR_INVALID_STATE;
    /* REG10 bit0 is a self-clearing soft PWROFF command, not ESP sleep. */
    return update_reg(0x10, 0x01, 0x01);
}

esp_err_t inkorb_battery_read(int *percent)
{
    if (!percent || !s_pmu) return ESP_ERR_INVALID_ARG;
    *percent = -1;
    uint8_t status, soc;
    ESP_RETURN_ON_ERROR(read_reg(0x00, &status), TAG, "Battery status read failed");
    if (!(status & (1U << 3))) return ESP_OK;
    ESP_RETURN_ON_ERROR(read_reg(0xA4, &soc), TAG, "Battery percentage read failed");
    if (soc > 100) return ESP_ERR_INVALID_RESPONSE;
    *percent = soc;
    return ESP_OK;
}

esp_err_t inkorb_battery_power(bool *plugged, bool *charging)
{
    if (!plugged || !charging || !s_pmu) return ESP_ERR_INVALID_ARG;
    uint8_t status1, status2;
    *plugged = *charging = false;
    ESP_RETURN_ON_ERROR(read_reg(0x00, &status1), TAG, "PMU status read failed");
    ESP_RETURN_ON_ERROR(read_reg(0x01, &status2), TAG, "Charger status read failed");
    *plugged = status1 & (1U << 5);
    *charging = ((status2 >> 5) & 0x03) == 0x01;
    return ESP_OK;
}
