#pragma once
#include <stdbool.h>
#include <stddef.h>

/* User-adjustable device settings, saved in NVS (namespace inkorb_cfg).
 * The voice assistant changes them through its tools. */
void inkorb_device_load(void);          /* call once after nvs_flash_init */
int inkorb_device_volume(void);         /* 0-100 */
int inkorb_device_brightness(void);     /* 5-100 */
void inkorb_device_set_volume(int level);
/* Takes the display lock, so call it from outside the LVGL task. */
bool inkorb_device_set_brightness(int level);
/* One-line Chinese status for the system prompt: battery, power, Wi-Fi, volume, brightness. */
void inkorb_device_status(char *out, size_t len);

/* Settings-page live preview while dragging: applied, not saved. */
void inkorb_device_preview_volume(int level);
void inkorb_device_preview_brightness(int level);   /* LVGL task (display lock held) */

/* Auto screen-off after this many seconds without touch or voice; 0 = never. */
int inkorb_device_auto_off(void);
void inkorb_device_set_auto_off(int seconds);

/* Multi-line Chinese "about this device" text. */
void inkorb_device_info(char *out, size_t len);
void inkorb_device_restart(void);
