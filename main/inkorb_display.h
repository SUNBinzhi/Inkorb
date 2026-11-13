#pragma once
#include "lvgl.h"
#include "esp_err.h"
#include <stdbool.h>

/* Board sleep hooks for the adapter's auto-sleep (pause) mode. `enter` runs
 * after the LVGL idle timeout and may refuse (non-ESP_OK) to keep rendering;
 * `exit` runs before LVGL resumes. Touch already requests wake; call
 * inkorb_display_request_wake() from key interrupts. */
lv_display_t *inkorb_display_start(esp_err_t (*enter)(void *), esp_err_t (*exit)(void *));
/* AMOLED panel: display off + sleep in, or sleep out (+120 ms) + display on. */
void inkorb_display_panel_power(bool on);
void inkorb_display_request_wake_from_isr(void);
