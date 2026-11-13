#pragma once
#include "esp_err.h"
#include <stdbool.h>

esp_err_t inkorb_battery_init(void);
/* Returns -1 through percent when no battery is connected. */
esp_err_t inkorb_battery_read(int *percent);
/* plugged: USB power present (REG00 bit5). charging: current flows into the
 * battery (REG01 bits 6:5 == 01); false once full even while plugged. */
esp_err_t inkorb_battery_power(bool *plugged, bool *charging);
/* AXP2101 soft power-off cuts the main rails; PWRON still wakes the PMIC. */
esp_err_t inkorb_battery_shutdown(void);
