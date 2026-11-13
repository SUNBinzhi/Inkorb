#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "esp_err.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"
#include "lvgl.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "wifi_sta.h"
#include "inkorb_device.h"
#include "nvs_flash.h"
#include "inkorb_ui.h"
#include "inkorb_mic.h"
#include "driver/gpio.h"
#include "inkorb_battery.h"
#include "inkorb_boot.h"
#include "inkorb_wifi_ui.h"
#include "inkorb_settings_ui.h"
#include "inkorb_usage_ui.h"
#include "inkorb_action.h"
#include "inkorb_usage.h"
#include "inkorb_voice.h"
#include "inkorb_display.h"

#include <time.h>
#include <sys/time.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>

static const char *TAG = "inkorb";

static inkorb_home_t s_ui;
static bool s_sntp_started;
static bool s_time_synced;
static char s_last_time[8];
static char s_last_date[16];
static inkorb_boot_t s_boot;
static bool s_home_visible;
static bool s_mic_finished;
static bool s_battery_checked;
static bool s_offline_requested;
static bool s_wifi_setup_requested;
static bool s_offline_offered;
static int s_battery_percent = -1;
static bool s_battery_plugged, s_battery_charging;
static char s_current_ip[16];
static uint32_t s_boot_started;
static uint32_t s_ready_since;
static bool s_voice_started;

/* Runtime mapping: small BOOT/GPIO0 is screen/off, active-low.
 * Large PWR/SYS_OUT/GPIO3 is talk, active-high; PMIC wake stays hardwired. */
#define INKORB_PWR_GPIO GPIO_NUM_0
#define INKORB_TALK_GPIO GPIO_NUM_3
#define INKORB_PWR_POLL_MS 20
#define INKORB_PWR_DEBOUNCE_SAMPLES 3
#define INKORB_PWR_OFF_HOLD_MS 2000

static int s_pwr_raw;
static int s_pwr_stable;
static unsigned s_pwr_samples;
static bool s_pwr_armed;
static bool s_pwr_short_candidate;
static uint32_t s_pwr_pressed_at;
static bool s_screen_off;

static lv_obj_t *s_wake_shield;

static void screen_set(bool off);

/* Screen-off power saving. While the screen is on we hold the CPU at full
 * speed; once it is off and LVGL has been idle 10 s the adapter pauses LVGL
 * (and its 1 ms tick), we put the panel to sleep, close the mic and drop the
 * lock so the CPU idles at 80 MHz. Touch (adapter ISR) or either key wakes it. */
static esp_pm_lock_handle_t s_awake_lock;
static volatile bool s_board_asleep;

static esp_err_t board_sleep_enter(void *ctx)
{
    (void)ctx;
    if (!s_screen_off) return ESP_FAIL;   /* keep rendering while the screen is on */
    inkorb_display_panel_power(false);
    inkorb_mic_set_active(false);
    s_board_asleep = true;
    if (s_awake_lock) esp_pm_lock_release(s_awake_lock);
    ESP_LOGI(TAG, "board asleep (panel sleep, mic off, CPU may idle at 80 MHz)");
    return ESP_OK;
}

static esp_err_t board_sleep_exit(void *ctx)
{
    (void)ctx;
    if (s_awake_lock) esp_pm_lock_acquire(s_awake_lock);
    s_board_asleep = false;
    inkorb_mic_set_active(true);
    inkorb_display_panel_power(true);
    ESP_LOGI(TAG, "board awake");
    return ESP_OK;
}

static void IRAM_ATTR on_key_edge(void *arg)
{
    (void)arg;
    if (s_board_asleep) inkorb_display_request_wake_from_isr();
}

static void start_power_saving(void)
{
    const esp_pm_config_t pm = { .max_freq_mhz = 240, .min_freq_mhz = 80, .light_sleep_enable = false };
    esp_err_t err = esp_pm_configure(&pm);
    if (err == ESP_OK) err = esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "screen_on", &s_awake_lock);
    if (err == ESP_OK) err = esp_pm_lock_acquire(s_awake_lock);
    if (err != ESP_OK) ESP_LOGW(TAG, "power management unavailable: %s", esp_err_to_name(err));
}

/* Keys are polled from LVGL timers, which stop while asleep: wake on edges. */
static void start_key_wake(void)
{
    esp_err_t err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) { ESP_LOGW(TAG, "key wake ISR unavailable"); return; }
    gpio_set_intr_type(INKORB_PWR_GPIO, GPIO_INTR_NEGEDGE);   /* small key, active low */
    gpio_set_intr_type(INKORB_TALK_GPIO, GPIO_INTR_POSEDGE);  /* big key, active high */
    gpio_isr_handler_add(INKORB_PWR_GPIO, on_key_edge, NULL);
    gpio_isr_handler_add(INKORB_TALK_GPIO, on_key_edge, NULL);
    gpio_intr_enable(INKORB_PWR_GPIO);
    gpio_intr_enable(INKORB_TALK_GPIO);
}

static void on_wake_shield(lv_event_t *event)
{
    (void)event;
    screen_set(false);
}

/* Screen off = emission 0 first; the board sleep hooks above then stop
 * rendering, the panel and the mic once LVGL idles. While off,
 * a transparent top-layer shield eats the first touch so waking the screen
 * never also presses whatever button sits underneath. LVGL task only. */
static void screen_set(bool off)
{
    if (off == s_screen_off) return;
    if (bsp_display_brightness_set(off ? 0 : inkorb_device_brightness()) != ESP_OK) {
        ESP_LOGE(TAG, "screen %s failed", off ? "off" : "on");
        return;
    }
    s_screen_off = off;
    if (!off) lv_obj_invalidate(lv_screen_active());   /* the panel may have slept */
    if (off && !s_wake_shield) {
        s_wake_shield = lv_obj_create(lv_layer_top());
        lv_obj_remove_style_all(s_wake_shield);
        lv_obj_set_size(s_wake_shield, LV_PCT(100), LV_PCT(100));
        lv_obj_add_flag(s_wake_shield, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(s_wake_shield, on_wake_shield, LV_EVENT_PRESSED, NULL);
    } else if (!off && s_wake_shield) {
        lv_obj_delete_async(s_wake_shield); /* may be inside its own event */
        s_wake_shield = NULL;
        lv_display_trigger_activity(NULL);
    }
    ESP_LOGI(TAG, "screen %s", off ? "off" : "on");
}

/* Auto screen-off: no touch and no voice activity for the chosen time. */
static void on_auto_off_timer(lv_timer_t *timer)
{
    (void)timer;
    int seconds = inkorb_device_auto_off();
    if (!s_home_visible || s_screen_off || seconds <= 0) return;
    if (s_voice_started && inkorb_voice_busy()) { lv_display_trigger_activity(NULL); return; }
    if (lv_display_get_inactive_time(NULL) >= (uint32_t)seconds * 1000U) screen_set(true);
}

static void on_power_button_timer(lv_timer_t *timer)
{
    (void)timer;
    int pressed = !gpio_get_level(INKORB_PWR_GPIO);
    if (pressed && s_pwr_stable && s_pwr_short_candidate &&
        lv_tick_elaps(s_pwr_pressed_at) >= INKORB_PWR_OFF_HOLD_MS) {
        s_pwr_short_candidate = false;
        ESP_LOGI(TAG, "PWR: 2s hold -> PMIC power off");
        esp_err_t err = inkorb_battery_shutdown();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "PWR: power off failed: %s", esp_err_to_name(err));
        }
        return;
    }
    if (pressed != s_pwr_raw) {
        s_pwr_raw = pressed;
        s_pwr_samples = 1;
        return;
    }
    if (s_pwr_samples < INKORB_PWR_DEBOUNCE_SAMPLES) {
        ++s_pwr_samples;
    }
    if (s_pwr_samples < INKORB_PWR_DEBOUNCE_SAMPLES || pressed == s_pwr_stable) {
        return;
    }
    s_pwr_stable = pressed;
    if (pressed) {
        if (s_pwr_armed) {
            s_pwr_short_candidate = true;
            s_pwr_pressed_at = lv_tick_get();
            s_pwr_armed = false;
        }
        return;
    }
    s_pwr_armed = true;
    if (!s_pwr_short_candidate) {
        return;
    }
    s_pwr_short_candidate = false;
    /* Short presses toggle on release; holding requests PMIC power-off at 2s.
     * A key held during power-on is ignored until released. */
    if (lv_tick_elaps(s_pwr_pressed_at) >= 1000) return;

    /* The LVGL timer runs under the display lock, serializing panel commands
     * with rendering. */
    screen_set(!s_screen_off);
}

static void start_power_button(void)
{
    const gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << INKORB_PWR_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));
    s_pwr_raw = s_pwr_stable = !gpio_get_level(INKORB_PWR_GPIO);
    /* A key held during startup must be released before it can toggle. */
    s_pwr_armed = !s_pwr_stable;
    if (!lv_timer_create(on_power_button_timer, INKORB_PWR_POLL_MS, NULL)) {
        ESP_LOGE(TAG, "PWR: button timer allocation failed");
        return;
    }
    ESP_LOGI(TAG, "PWR: small GPIO0 active-low, short release toggles screen (debounced)");
    ESP_LOGI(TAG, "PWR: small hold 2000ms for shutdown; large hardware power-on 2s");
}

/* Boot holds Wi-Fi only long enough to set the clock, then lets it go. */
static volatile bool s_time_hold;

static void release_time_sync_hold(void)
{
    if (!s_time_hold) return;
    s_time_hold = false;
    wifi_sta_release();
}

static void on_time_sync_timeout(lv_timer_t *timer)
{
    lv_timer_delete(timer);
    if (s_time_hold) ESP_LOGW(TAG, "time sync not done in 60 s; releasing Wi-Fi");
    release_time_sync_hold();
}

static bool time_looks_valid(const struct tm *t)
{
    return (t->tm_year + 1900) >= 2024;
}

static void mark_synced_once(const char *via)
{
    if (s_time_synced) {
        return;
    }
    s_time_synced = true;
    ESP_LOGI(TAG, "SNTP sync OK via %s (TZ=CST-8)", via);
    release_time_sync_hold();
    s_last_time[0] = '\0';
    s_last_date[0] = '\0';
}

static void refresh_clock_labels(void)
{
    if (!s_ui.time_label || !s_ui.date_label) {
        return;
    }

    time_t now = 0;
    struct tm t = {0};
    time(&now);
    localtime_r(&now, &t);

    if (!s_time_synced && s_sntp_started) {
        sntp_sync_status_t st = esp_sntp_get_sync_status();
        if (st == SNTP_SYNC_STATUS_COMPLETED) {
            mark_synced_once("status");
        }
    }

    char time_buf[8];
    char date_buf[16];
    if (s_time_synced && time_looks_valid(&t)) {
        snprintf(time_buf, sizeof(time_buf), "%02d:%02d", t.tm_hour, t.tm_min);
        snprintf(date_buf, sizeof(date_buf), "%04d-%02d-%02d",
                 t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
    } else {
        snprintf(time_buf, sizeof(time_buf), "SYNC..");
        snprintf(date_buf, sizeof(date_buf), "----.--.--");
    }

    if (strcmp(time_buf, s_last_time) == 0 && strcmp(date_buf, s_last_date) == 0) {
        return;
    }
    strncpy(s_last_time, time_buf, sizeof(s_last_time) - 1);
    s_last_time[sizeof(s_last_time) - 1] = '\0';
    strncpy(s_last_date, date_buf, sizeof(s_last_date) - 1);
    s_last_date[sizeof(s_last_date) - 1] = '\0';

    /* All callers hold the display lock (LVGL timer or locked SNTP callback). */
    inkorb_home_set_demo_clock(&s_ui, time_buf, date_buf);
}

static void on_clock_timer(lv_timer_t *timer)
{
    (void)timer;
    refresh_clock_labels();
}

static void on_talk_button(lv_timer_t *timer)
{
    (void)timer;
    static bool initialized;
    static int previous, stable;
    static unsigned samples;
    static bool armed, recording;
    int raw = gpio_get_level(INKORB_TALK_GPIO);
    if (!initialized) {
        previous = stable = raw;
        armed = !raw;
        initialized = true;
        return;
    }
    if (raw != previous) { previous = raw; samples = 1; return; }
    if (samples < 3) ++samples;
    if (samples < 3 || raw == stable) return;
    stable = raw;
    if (stable) {
        if (armed && s_voice_started) {
            screen_set(false);
            lv_display_trigger_activity(NULL);
            inkorb_voice_button(true);
            recording = true;
        }
        armed = false;
    } else {
        if (recording && s_voice_started) inkorb_voice_button(false);
        recording = false;
        armed = true;
    }
}

static void on_sntp_sync(struct timeval *tv)
{
    (void)tv;
    bsp_display_lock(-1);
    mark_synced_once("sync_cb");
    refresh_clock_labels();
    bsp_display_unlock();
}

static void start_sntp_once(void)
{
    if (s_sntp_started) {
        return;
    }
    s_sntp_started = true;

    setenv("TZ", "CST-8", 1);
    tzset();

    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("ntp.aliyun.com");
    cfg.wait_for_sync = false;
    cfg.sync_cb = on_sntp_sync;
    esp_err_t err = esp_netif_sntp_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SNTP init failed: %s", esp_err_to_name(err));
        s_sntp_started = false;
        return;
    }
    ESP_LOGI(TAG, "SNTP started TZ=CST-8");
}

static void on_ip(const char *ip_or_null)
{
    bsp_display_lock(-1);
    snprintf(s_current_ip, sizeof(s_current_ip), "%s", ip_or_null ? ip_or_null : "");
    if (s_home_visible) {
        inkorb_home_set_ip(&s_ui, ip_or_null ? ip_or_null : "WiFi...");
    }
    bsp_display_unlock();
    if (ip_or_null) {
        start_sntp_once();
    }
}

static void mic_boot_task(void *arg)
{
    (void)arg;
    /* After LCD + Wi‑Fi settle — avoids I2S/SPI DMA NO_MEM races. */
    vTaskDelay(pdMS_TO_TICKS(1500));   /* after the LCD settles */
    if (inkorb_mic_start()) {
        ESP_LOGI(TAG, "mic reactive bloom enabled");
    } else {
        ESP_LOGW(TAG, "mic unavailable — bloom idle breathe only");
    }
    bsp_display_lock(-1);
    s_mic_finished = true;
    bsp_display_unlock();
    vTaskDelete(NULL);
}

static void on_battery_timer(lv_timer_t *timer)
{
    (void)timer;
    static bool ready;
    static int last_percent = -2;
    if (!ready) ready = inkorb_battery_init() == ESP_OK;
    int percent = -1;
    if (ready && inkorb_battery_read(&percent) != ESP_OK) percent = -1;
    bool plugged = false, charging = false;
    if (ready && inkorb_battery_power(&plugged, &charging) != ESP_OK) plugged = charging = false;
    if (plugged != s_battery_plugged || charging != s_battery_charging)
        ESP_LOGI(TAG, "power: usb=%d charging=%d", plugged, charging);
    s_battery_plugged = plugged;
    s_battery_charging = charging;
    if (s_home_visible) inkorb_home_set_charging(&s_ui, plugged, charging);
    s_battery_checked = true;
    s_battery_percent = percent;
    if (percent != last_percent) {
        /* Already under the LVGL lock. Reuse this task instead of consuming
         * another internal-RAM stack needed by the SPI DMA path. */
        if (s_home_visible) inkorb_home_set_battery(&s_ui, percent);
        if (percent >= 0) ESP_LOGI(TAG, "battery: %d%%", percent);
        else ESP_LOGW(TAG, "battery: unavailable or not connected");
        last_percent = percent;
    }
}

static void on_continue_offline(lv_event_t *event)
{
    (void)event;
    s_offline_requested = true;
}

/* Connected: settings to switch or add networks. Offline: straight to phone setup. */
static void on_wifi_settings(lv_event_t *event)
{
    (void)event;
    inkorb_wifi_ui_open(!wifi_sta_status(NULL, 0, NULL, NULL));
}

/* Home gestures open the usage and camera pages. */
static void on_home_gesture(lv_event_t *event)
{
    lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_active());
    if (s_screen_off || (dir != LV_DIR_LEFT && dir != LV_DIR_BOTTOM)) return;
    for (lv_obj_t *obj = lv_event_get_target(event); obj; obj = lv_obj_get_parent(obj))
        if (lv_obj_has_flag(obj, INKORB_OVERLAY_FLAG)) return;
    if (dir == LV_DIR_LEFT) inkorb_usage_ui_open();
    else inkorb_action_open(&s_ui);
}

static void on_settings(lv_event_t *event)
{
    (void)event;
    inkorb_settings_ui_open();
}

static void on_setup_wifi(lv_event_t *event)
{
    (void)event;
    s_offline_requested = true;
    s_wifi_setup_requested = true;
}

static void show_home(void)
{
    /* Build and populate everything within one LVGL lock. No placeholders
     * can render between constructing the home screen and filling its data. */
    inkorb_home_create(&s_ui); /* Cleans the splash and its spinner animation. */
    s_last_time[0] = s_last_date[0] = '\0';
    refresh_clock_labels();
    inkorb_home_set_ip(&s_ui, s_current_ip[0] ? s_current_ip : "Offline");
    inkorb_home_set_battery(&s_ui, s_battery_percent);
    inkorb_home_set_charging(&s_ui, s_battery_plugged, s_battery_charging);
    s_home_visible = true;
    s_voice_started = inkorb_voice_start(&s_ui);
    inkorb_wifi_ui_init(&s_ui);
    inkorb_settings_ui_init(&s_ui);
    inkorb_usage_ui_init(&s_ui);
    lv_obj_add_event_cb(lv_screen_active(), on_home_gesture, LV_EVENT_GESTURE, NULL);
    lv_obj_add_event_cb(s_ui.settings_button, on_settings, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(s_ui.capsule, on_wifi_settings, LV_EVENT_CLICKED, NULL);
    if (s_wifi_setup_requested) inkorb_wifi_ui_open(true);
    ESP_LOGI(TAG, "VOICE: prototype %s; hold large PWR to talk; tap capsule for Wi-Fi", s_voice_started ? "ready" : "unavailable");
    ESP_LOGI(TAG, "BOOT: home shown (%s, time=%s, battery=%d, mic_init=%d; Wi-Fi on demand)",
             s_current_ip[0] ? "Wi-Fi ready" : "offline",
             s_time_synced ? "synced" : "pending", s_battery_percent, s_mic_finished);
}

static void on_boot_timer(lv_timer_t *timer)
{
    uint32_t elapsed = lv_tick_elaps(s_boot_started);
    time_t now;
    struct tm local;
    time(&now);
    localtime_r(&now, &local);
    bool clock_ready = s_time_synced && time_looks_valid(&local);
    /* Wi-Fi is on demand now: home no longer waits for the network or the
     * clock (it shows "SYNC.." until the background sync lands). */
    (void)clock_ready;
    bool ready = s_battery_checked && s_mic_finished;

    if (s_offline_requested && s_battery_checked && s_mic_finished) {
        show_home();
        lv_timer_delete(timer);
        return;
    }
    if (ready && elapsed >= 1500) {
        if (!s_ready_since) {
            s_ready_since = lv_tick_get();
            inkorb_boot_set_status(&s_boot, "Ready", true);
        } else if (lv_tick_elaps(s_ready_since) >= 600) { /* let the ring close */
            show_home();
            lv_timer_delete(timer);
        }
        return;
    }
    s_ready_since = 0;
    inkorb_boot_set_status(&s_boot, "Getting ready", false);
    if (elapsed >= 20000 && !s_offline_offered) {
        inkorb_boot_allow_offline(&s_boot);
        s_offline_offered = true;
        ESP_LOGW(TAG, "BOOT: waiting for network/time; Wi-Fi setup and offline options available");
    }
}

void app_main(void)
{
    /* NVS first: saved volume and brightness apply from the splash on.
     * wifi_sta_start() calls nvs_flash_init() again, which is a no-op. */
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_err);
    inkorb_action_init();
    inkorb_device_load();
    inkorb_usage_load();
    start_power_saving();
    lv_display_t *display = inkorb_display_start(board_sleep_enter, board_sleep_exit);
    if (!display) { ESP_LOGE(TAG, "display startup failed"); return; }
    bsp_display_lock(-1);
    ESP_ERROR_CHECK(bsp_display_brightness_set(0));
    inkorb_boot_create(&s_boot, on_continue_offline, on_setup_wifi, NULL);
    s_boot_started = lv_tick_get();
    lv_timer_create(on_boot_timer, 100, NULL);
    lv_timer_create(on_clock_timer, 1000, NULL);
    start_power_button();
    const gpio_config_t talk_cfg = {
        .pin_bit_mask = 1ULL << INKORB_TALK_GPIO,
        .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE, .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&talk_cfg));
    lv_timer_create(on_talk_button, 20, NULL);
    start_key_wake();
    lv_timer_create(on_auto_off_timer, 1000, NULL);
    lv_timer_t *battery_timer = lv_timer_create(on_battery_timer, 3000, NULL); /* charger plug-in shows within 3 s */
    if (battery_timer) lv_timer_ready(battery_timer);
    else ESP_LOGE(TAG, "battery timer allocation failed");
    /* Flush the complete splash before making the panel visible. */
    lv_refr_now(display);
    ESP_ERROR_CHECK(bsp_display_brightness_set(inkorb_device_brightness()));
    bsp_display_unlock();
    ESP_LOGI(TAG, "BOOT: splash shown; home deferred until data ready");

    wifi_sta_set_ip_cb(on_ip);
    ESP_ERROR_CHECK(wifi_sta_start());
    /* Background clock sync; the radio goes off again after it (WIFI_LINGER_S). */
    s_time_hold = true;
    wifi_sta_hold();
    bsp_display_lock(-1);
    lv_timer_create(on_time_sync_timeout, 60000, NULL);
    bsp_display_unlock();

    if (xTaskCreate(mic_boot_task, "mic_boot", 3072, NULL, 3, NULL) != pdPASS) {
        ESP_LOGW(TAG, "mic boot task unavailable; continuing without mic");
        bsp_display_lock(-1);
        s_mic_finished = true;
        bsp_display_unlock();
    }
}
