#pragma once
#include <stdbool.h>
#include <stddef.h>

/* Phone setup: the device opens a WPA2 hotspot, answers every DNS name with
 * itself so phones pop up the captive page, and serves a small form that
 * hands the chosen network to wifi_sta_try(). */
bool inkorb_portal_start(void);
void inkorb_portal_stop(void);
bool inkorb_portal_running(void);
/* Hotspot name/password for the screen and QR code. */
void inkorb_portal_credentials(char *ssid, size_t ssid_len, char *pass, size_t pass_len);
