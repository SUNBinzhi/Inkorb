#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>

#define WIFI_STA_MAX_SAVED 8

/** Optional: called on GOT_IP with dotted IP, or on disconnect with NULL. */
typedef void (*wifi_sta_ip_cb_t)(const char *ip_or_null);

void wifi_sta_set_ip_cb(wifi_sta_ip_cb_t cb);

/** Initialise Wi-Fi without turning the radio on. Non-blocking.
 *  The network in wifi_credentials.h seeds the saved list on first boot. */
esp_err_t wifi_sta_start(void);

/** On-demand radio: apps that need the network hold it while open. The radio
 *  starts on the first hold (scan + join the strongest saved network) and stops
 *  WIFI_LINGER_S after the last release, so quick re-entries stay connected. */
#define WIFI_LINGER_S 120
void wifi_sta_hold(void);
void wifi_sta_release(void);
/** Stop now if nobody holds Wi-Fi (e.g. give the radio to Bluetooth). */
void wifi_sta_stop_if_idle(void);
bool wifi_sta_radio_on(void);

/** Connected SSID, RSSI and IP (ip may be NULL, needs 16 bytes); false when not connected. */
bool wifi_sta_status(char *ssid, size_t len, int *rssi, char *ip);

/** Copies saved SSIDs, most recently used first; returns the count. */
int wifi_sta_saved(char ssids[][33], int max);
void wifi_sta_forget(const char *ssid);
/** Switch to a saved network (reported through wifi_sta_try_state). */
void wifi_sta_use(const char *ssid);

/** Try a new network; it is saved only once it gets an IP. */
typedef enum { WIFI_TRY_IDLE, WIFI_TRY_CONNECTING, WIFI_TRY_OK, WIFI_TRY_FAILED } wifi_try_state_t;
void wifi_sta_try(const char *ssid, const char *password);
/** State of the last try; ssid/why may be NULL. why is a short Chinese reason on failure. */
wifi_try_state_t wifi_sta_try_state(char *ssid, size_t len, const char **why);

/** Setup hotspot alongside the station (APSTA). */
esp_err_t wifi_sta_set_ap(bool on, const char *ssid, const char *password);
int wifi_sta_ap_clients(void);
