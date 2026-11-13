#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
bool inkorb_cloud_configured(void);
/* Barge-in: stop the running turn (network, TTS and playback) as soon as possible. */
void inkorb_cloud_cancel(void);
/* Settings page: long-term memory text as saved, and clearing. Clears hit NVS
 * now; the in-RAM copies follow at the start of the next turn. */
void inkorb_cloud_memory_text(char *out, size_t len);
void inkorb_cloud_forget(bool long_term, bool recent);
void inkorb_cloud_turn(const uint8_t *pcm, size_t bytes, TickType_t released,
                       void (*status)(const char *), void (*texts)(const char *, const char *));
