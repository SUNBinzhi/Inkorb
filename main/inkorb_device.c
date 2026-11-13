#include "inkorb_device.h"
#include "inkorb_battery.h"
#include "inkorb_mic.h"
#include "wifi_sta.h"
#include "bsp/esp-bsp.h"
#include "esp_log.h"
#include "nvs.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_timer.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "inkorb_device";
#define NVS_NS "inkorb_cfg"
static int s_volume = 100, s_brightness = 100, s_auto_off;

static int clamp(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static void save(const char *key, int value)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NS, NVS_READWRITE, &nvs) != ESP_OK) return;
    if (nvs_set_i32(nvs, key, value) == ESP_OK) nvs_commit(nvs);
    nvs_close(nvs);
}

void inkorb_device_load(void)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NS, NVS_READONLY, &nvs) == ESP_OK) {
        int32_t v;
        if (nvs_get_i32(nvs, "volume", &v) == ESP_OK) s_volume = clamp(v, 0, 100);
        if (nvs_get_i32(nvs, "bright", &v) == ESP_OK) s_brightness = clamp(v, 5, 100);
        if (nvs_get_i32(nvs, "auto_off", &v) == ESP_OK) s_auto_off = clamp(v, 0, 3600);
        nvs_close(nvs);
    }
    inkorb_speaker_set_volume(s_volume);
    ESP_LOGI(TAG, "settings: volume=%d brightness=%d auto_off=%ds", s_volume, s_brightness, s_auto_off);
}

int inkorb_device_volume(void) { return s_volume; }
int inkorb_device_brightness(void) { return s_brightness; }

void inkorb_device_set_volume(int level)
{
    s_volume = clamp(level, 0, 100);
    inkorb_speaker_set_volume(s_volume);
    save("volume", s_volume);
    ESP_LOGI(TAG, "volume set to %d", s_volume);
}

bool inkorb_device_set_brightness(int level)
{
    level = clamp(level, 5, 100);
    /* Panel commands share the bus with LVGL flushes. */
    bsp_display_lock(-1);
    bool ok = bsp_display_brightness_set(level) == ESP_OK;
    bsp_display_unlock();
    if (!ok) return false;
    s_brightness = level;
    save("bright", s_brightness);
    ESP_LOGI(TAG, "brightness set to %d", s_brightness);
    return true;
}

/* snprintf may cut a multi-byte character; an invalid UTF-8 tail makes the
 * whole chat request fail with HTTP 400, so drop any partial character. */
static void trim_utf8(char *s)
{
    size_t n = strlen(s), start = n;
    while (start && ((unsigned char)s[start - 1] & 0xC0) == 0x80) start--;
    if (!start) return;
    unsigned char lead = (unsigned char)s[start - 1];
    size_t need = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
    if (n - (start - 1) < need) s[start - 1] = '\0';
}

void inkorb_device_status(char *out, size_t len)
{
    int percent = -1;
    bool plugged = false, charging = false;
    if (inkorb_battery_read(&percent) != ESP_OK) percent = -1;
    if (inkorb_battery_power(&plugged, &charging) != ESP_OK) plugged = charging = false;
    char ssid[33] = "", ip[16] = "";
    int rssi = 0;
    bool online = wifi_sta_status(ssid, sizeof(ssid), &rssi, ip);
    char battery[96];
    if (percent >= 0) snprintf(battery, sizeof(battery), "电量%d%%%s", percent,
                               charging ? "，正在充电" : plugged ? "，已接电源（已充满或未充电）" : "，未接电源");
    else snprintf(battery, sizeof(battery), "电量读取失败");
    char wifi[160];
    if (online) snprintf(wifi, sizeof(wifi), "Wi-Fi「%s」，信号%s（%ddBm），IP %s", ssid,
                         rssi > -60 ? "强" : rssi > -72 ? "中" : "弱", rssi, ip);
    else snprintf(wifi, sizeof(wifi), "Wi-Fi未连接");
    snprintf(out, len, "%s；%s；音量%d（0-100）；屏幕亮度%d（5-100）", battery, wifi, s_volume, s_brightness);
    trim_utf8(out);
}

void inkorb_device_preview_volume(int level)
{
    inkorb_speaker_set_volume(clamp(level, 0, 100));
}

void inkorb_device_preview_brightness(int level)
{
    bsp_display_brightness_set(clamp(level, 5, 100));
}

int inkorb_device_auto_off(void) { return s_auto_off; }

void inkorb_device_set_auto_off(int seconds)
{
    s_auto_off = clamp(seconds, 0, 3600);
    save("auto_off", s_auto_off);
    ESP_LOGI(TAG, "auto screen-off set to %ds", s_auto_off);
}

void inkorb_device_info(char *out, size_t len)
{
    const esp_app_desc_t *app = esp_app_get_description();
    int percent = -1;
    bool plugged = false, charging = false;
    if (inkorb_battery_read(&percent) != ESP_OK) percent = -1;
    if (inkorb_battery_power(&plugged, &charging) != ESP_OK) plugged = charging = false;
    char ssid[33] = "", ip[16] = "";
    int rssi = 0;
    bool online = wifi_sta_status(ssid, sizeof(ssid), &rssi, ip);
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    uint32_t up = (uint32_t)(esp_timer_get_time() / 1000000);
    char battery[64];
    if (percent >= 0) snprintf(battery, sizeof(battery), "%d%%%s", percent, charging ? "  充电中" : plugged ? "  已接电源" : "");
    else snprintf(battery, sizeof(battery), "读取失败");
    snprintf(out, len,
             "固件  %s\n编译  %s %s\n电池  %s\nWi-Fi  %s\nIP  %s\nMAC  %02X:%02X:%02X:%02X:%02X:%02X\n"
             "已运行  %lu小时%lu分\n可用内存  内部%uKB  PSRAM %uKB",
             app->version, app->date, app->time, battery, online ? ssid : "未连接", online && ip[0] ? ip : "--",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
             (unsigned long)(up / 3600), (unsigned long)(up / 60 % 60),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    trim_utf8(out);
}

void inkorb_device_restart(void)
{
    ESP_LOGI(TAG, "restart requested from settings");
    esp_restart();
}
