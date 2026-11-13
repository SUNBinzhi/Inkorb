#include "wifi_sta.h"
#include "wifi_credentials.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "nvs_flash.h"
#include "nvs.h"

/* The ESP32-S3 only sees 2.4GHz networks; never log Wi-Fi passwords. */
static const char *TAG = "wifi_sta";
#define NVS_NS  "inkorb_wifi"
#define NVS_KEY "nets"
#define TRY_TIMEOUT_MS 25000

typedef struct { char ssid[33]; char pass[65]; } net_t;

static wifi_sta_ip_cb_t s_ip_cb;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static net_t s_nets[WIFI_STA_MAX_SAVED];
static int s_count;
static volatile bool s_connected, s_switching, s_picking, s_ap_on;
static volatile bool s_radio_on;   /* on-demand: off until an app holds Wi-Fi */
static int s_fails;
static esp_netif_t *s_ap_netif;

static net_t s_pending;
static volatile wifi_try_state_t s_try;
static int s_try_fails;
static TickType_t s_try_started;
static const char *s_try_why;

void wifi_sta_set_ip_cb(wifi_sta_ip_cb_t cb)
{
    s_ip_cb = cb;
}

static void store_write(void)
{
    net_t copy[WIFI_STA_MAX_SAVED];
    int count;
    taskENTER_CRITICAL(&s_lock);
    count = s_count;
    memcpy(copy, s_nets, sizeof(copy));
    taskEXIT_CRITICAL(&s_lock);
    nvs_handle_t nvs;
    if (nvs_open(NVS_NS, NVS_READWRITE, &nvs) != ESP_OK) return;
    if (nvs_set_blob(nvs, NVS_KEY, copy, sizeof(net_t) * count) == ESP_OK) nvs_commit(nvs);
    nvs_close(nvs);
    memset(copy, 0, sizeof(copy));
}

static void store_load(void)
{
    nvs_handle_t nvs;
    size_t size = sizeof(s_nets);
    if (nvs_open(NVS_NS, NVS_READONLY, &nvs) == ESP_OK) {
        if (nvs_get_blob(nvs, NVS_KEY, s_nets, &size) == ESP_OK) s_count = size / sizeof(net_t);
        nvs_close(nvs);
        if (s_count) return;
    }
    /* First boot: seed with the compiled-in home network. */
    if (strcmp(WIFI_SSID, "YOUR_SSID") != 0 && WIFI_SSID[0]) {
        snprintf(s_nets[0].ssid, sizeof(s_nets[0].ssid), "%s", WIFI_SSID);
        snprintf(s_nets[0].pass, sizeof(s_nets[0].pass), "%s", WIFI_PASSWORD);
        s_count = 1;
        store_write();
    }
}

/* Most recently added first; same name (any casing) replaces the old entry. */
static void store_add(const net_t *net)
{
    taskENTER_CRITICAL(&s_lock);
    int at = s_count < WIFI_STA_MAX_SAVED ? s_count : WIFI_STA_MAX_SAVED - 1;
    for (int i = 0; i < s_count; i++) if (!strcasecmp(s_nets[i].ssid, net->ssid)) { at = i; break; }
    if (at == s_count) s_count++;
    memmove(&s_nets[1], &s_nets[0], sizeof(net_t) * at);
    s_nets[0] = *net;
    taskEXIT_CRITICAL(&s_lock);
    store_write();
}

int wifi_sta_saved(char ssids[][33], int max)
{
    taskENTER_CRITICAL(&s_lock);
    int n = s_count < max ? s_count : max;
    for (int i = 0; i < n; i++) memcpy(ssids[i], s_nets[i].ssid, 33);
    taskEXIT_CRITICAL(&s_lock);
    return n;
}

void wifi_sta_forget(const char *ssid)
{
    bool removed = false;
    taskENTER_CRITICAL(&s_lock);
    for (int i = 0; i < s_count; i++) {
        if (strcmp(s_nets[i].ssid, ssid)) continue;
        memmove(&s_nets[i], &s_nets[i + 1], sizeof(net_t) * (s_count - i - 1));
        memset(&s_nets[--s_count], 0, sizeof(net_t));
        removed = true;
        break;
    }
    taskEXIT_CRITICAL(&s_lock);
    if (removed) { store_write(); ESP_LOGI(TAG, "forgot saved network %s", ssid); }
}

static void connect_to(const char *ssid, const char *pass)
{
    wifi_config_t cfg = {0};
    memcpy(cfg.sta.ssid, ssid, strnlen(ssid, 32));
    memcpy(cfg.sta.password, pass, strnlen(pass, 64));
    cfg.sta.threshold.authmode = pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    cfg.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    esp_wifi_set_config(WIFI_IF_STA, &cfg);
    esp_wifi_connect();
    memset(&cfg, 0, sizeof(cfg));
}

/* Scan and join the strongest saved network; scan results arrive sorted by RSSI. */
static void pick_task(void *arg)
{
    (void)arg;
    wifi_ap_record_t *aps = heap_caps_calloc(24, sizeof(*aps), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    while (aps && s_radio_on && !s_connected && s_try != WIFI_TRY_CONNECTING) {
        wifi_scan_config_t scan = {0};
        uint16_t count = 24;
        bool joined = false;
        if (esp_wifi_scan_start(&scan, true) == ESP_OK && esp_wifi_scan_get_ap_records(&count, aps) == ESP_OK) {
            for (unsigned i = 0; i < count && !joined; i++) {
                net_t net = {0};
                taskENTER_CRITICAL(&s_lock);
                for (int k = 0; k < s_count; k++) {
                    if (strcasecmp((const char *)aps[i].ssid, s_nets[k].ssid)) continue;
                    net = s_nets[k];
                    memcpy(net.ssid, aps[i].ssid, sizeof(net.ssid)); /* exact casing from the air */
                    break;
                }
                taskEXIT_CRITICAL(&s_lock);
                if (!net.ssid[0] || s_try == WIFI_TRY_CONNECTING) continue;
                ESP_LOGI(TAG, "joining saved Wi-Fi %s RSSI=%d channel=%u", net.ssid, aps[i].rssi, aps[i].primary);
                connect_to(net.ssid, net.pass);
                memset(&net, 0, sizeof(net));
                joined = true;
            }
            if (!joined) ESP_LOGW(TAG, "no saved 2.4GHz network in range (%u seen)", count);
        }
        if (joined) break;
        /* Scanning hops channels and disturbs the setup hotspot; back off more then. */
        vTaskDelay(pdMS_TO_TICKS(s_ap_on ? 15000 : 8000));
    }
    free(aps);
    s_picking = false;
    vTaskDeleteWithCaps(NULL);
}

static void start_pick(void)
{
    if (s_picking) return;
    s_picking = true;
    if (xTaskCreateWithCaps(pick_task, "wifi_pick", 4096, NULL, 3, NULL,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        s_picking = false;
        esp_wifi_connect();
    }
}

static const char *reason_text(uint8_t reason)
{
    switch (reason) {
    case WIFI_REASON_NO_AP_FOUND:
    case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
        return "找不到这个网络（设备只支持2.4GHz）";
    case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
    case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD:
        return "网络加密方式不兼容";
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_MIC_FAILURE:
    case WIFI_REASON_AUTH_EXPIRE:
        return "密码可能不正确";
    default:
        return "连接失败，请重试";
    }
}

static void try_failed(const char *why)
{
    s_try_why = why;
    s_try = WIFI_TRY_FAILED;
    ESP_LOGW(TAG, "new network %s failed: %s", s_pending.ssid, why);
    memset(s_pending.pass, 0, sizeof(s_pending.pass));
    esp_wifi_disconnect();
    start_pick();
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        start_pick();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED) {
        s_connected = true;
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *ev = data;
        bool was_connected = s_connected;
        s_connected = false;
        if (was_connected && s_ip_cb) s_ip_cb(NULL);
        if (s_switching) { s_switching = false; connect_to(s_pending.ssid, s_pending.pass); return; }
        if (s_try == WIFI_TRY_CONNECTING) {
            if (strncmp((const char *)ev->ssid, s_pending.ssid, ev->ssid_len)) {
                connect_to(s_pending.ssid, s_pending.pass); /* late event from the old network */
            } else if (++s_try_fails >= 3) {
                try_failed(reason_text(ev->reason));
            } else {
                esp_wifi_connect();
            }
            return;
        }
        if (!s_radio_on) return;   /* our own stop: do not reconnect */
        ESP_LOGW(TAG, "disconnected (reason %u), retrying...", ev->reason);
        if (s_picking) return; /* the scan task chooses the next network */
        if (++s_fails < 3) esp_wifi_connect();
        else { s_fails = 0; start_pick(); }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "GOT_IP " IPSTR, IP2STR(&e->ip_info.ip));
        s_fails = 0;
        if (s_try == WIFI_TRY_CONNECTING) {
            /* Event-loop task: internal stack, safe for the NVS write. */
            store_add(&s_pending);
            memset(s_pending.pass, 0, sizeof(s_pending.pass));
            s_try = WIFI_TRY_OK;
            ESP_LOGI(TAG, "saved new network %s", s_pending.ssid);
        }
        if (s_ip_cb) {
            char buf[16];
            snprintf(buf, sizeof(buf), IPSTR, IP2STR(&e->ip_info.ip));
            s_ip_cb(buf);
        }
    }
}

void wifi_sta_try(const char *ssid, const char *password)
{
    memset(&s_pending, 0, sizeof(s_pending));
    snprintf(s_pending.ssid, sizeof(s_pending.ssid), "%s", ssid);
    snprintf(s_pending.pass, sizeof(s_pending.pass), "%s", password ? password : "");
    s_try_fails = 0;
    s_try_why = NULL;
    s_try_started = xTaskGetTickCount();
    s_try = WIFI_TRY_CONNECTING;
    ESP_LOGI(TAG, "trying new network %s", s_pending.ssid);
    if (s_connected) {
        s_switching = true;
        esp_wifi_disconnect();
    } else {
        esp_wifi_disconnect();
        connect_to(s_pending.ssid, s_pending.pass);
    }
}

void wifi_sta_use(const char *ssid)
{
    net_t net = {0};
    taskENTER_CRITICAL(&s_lock);
    for (int i = 0; i < s_count; i++) if (!strcmp(s_nets[i].ssid, ssid)) { net = s_nets[i]; break; }
    taskEXIT_CRITICAL(&s_lock);
    if (net.ssid[0]) wifi_sta_try(net.ssid, net.pass);
    memset(&net, 0, sizeof(net));
}

wifi_try_state_t wifi_sta_try_state(char *ssid, size_t len, const char **why)
{
    if (s_try == WIFI_TRY_CONNECTING &&
        xTaskGetTickCount() - s_try_started > pdMS_TO_TICKS(TRY_TIMEOUT_MS)) {
        s_switching = false;
        try_failed("连接超时，请靠近路由器再试");
    }
    if (ssid) snprintf(ssid, len, "%s", s_pending.ssid);
    if (why) *why = s_try_why;
    return s_try;
}

bool wifi_sta_status(char *ssid, size_t len, int *rssi, char *ip)
{
    wifi_ap_record_t ap;
    if (!s_connected || esp_wifi_sta_get_ap_info(&ap) != ESP_OK) return false;
    if (ssid) snprintf(ssid, len, "%s", (const char *)ap.ssid);
    if (rssi) *rssi = ap.rssi;
    if (ip) {
        esp_netif_ip_info_t info = {0};
        esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        ip[0] = '\0';
        if (sta && esp_netif_get_ip_info(sta, &info) == ESP_OK && info.ip.addr)
            snprintf(ip, 16, IPSTR, IP2STR(&info.ip));
    }
    return true;
}

esp_err_t wifi_sta_set_ap(bool on, const char *ssid, const char *password)
{
    if (!on) {
        s_ap_on = false;
        if (s_try != WIFI_TRY_CONNECTING) s_try = WIFI_TRY_IDLE;
        esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
        if (!s_connected) start_pick();
        return err;
    }
    if (s_try != WIFI_TRY_CONNECTING) s_try = WIFI_TRY_IDLE; /* fresh result per session */
    if (!s_ap_netif) s_ap_netif = esp_netif_create_default_wifi_ap();
    wifi_config_t ap = {0};
    size_t n = strnlen(ssid, 32);
    memcpy(ap.ap.ssid, ssid, n);
    ap.ap.ssid_len = n;
    memcpy(ap.ap.password, password, strnlen(password, 64));
    ap.ap.channel = 6;
    ap.ap.max_connection = 2;
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err == ESP_OK) err = esp_wifi_set_config(WIFI_IF_AP, &ap);
    memset(&ap, 0, sizeof(ap));
    s_ap_on = err == ESP_OK;
    return err;
}

int wifi_sta_ap_clients(void)
{
    wifi_sta_list_t list;
    if (!s_ap_on || esp_wifi_ap_get_sta_list(&list) != ESP_OK) return 0;
    return list.num;
}

/* ---------- on-demand radio ---------- */

static int s_holders;
static esp_timer_handle_t s_linger;

/* Bring the driver up from scratch (it is fully deinitialised while off). */
static esp_err_t driver_up(void)
{
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_wifi_init(&cfg);
    if (err == ESP_OK) err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (err == ESP_OK) err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err == ESP_OK) err = esp_wifi_start();   /* STA_START event scans and joins */
    return err;
}

static void radio_off(void)
{
    if (!s_radio_on) return;
    s_radio_on = false;
    s_fails = 0;
    s_ap_on = false;
    esp_wifi_stop();
    esp_wifi_deinit();
    bool was_connected = s_connected;
    s_connected = false;
    if (was_connected && s_ip_cb) s_ip_cb(NULL);
    ESP_LOGI(TAG, "radio off (no app needs Wi-Fi)");
}

static void linger_expired(void *arg)
{
    (void)arg;
    taskENTER_CRITICAL(&s_lock);
    bool idle = s_holders == 0;
    taskEXIT_CRITICAL(&s_lock);
    if (idle) radio_off();
}

void wifi_sta_hold(void)
{
    taskENTER_CRITICAL(&s_lock);
    s_holders++;
    taskEXIT_CRITICAL(&s_lock);
    if (s_linger) esp_timer_stop(s_linger);
    if (!s_radio_on) {
        s_radio_on = true;
        esp_err_t err = driver_up();
        ESP_LOGI(TAG, "radio on (%s)", esp_err_to_name(err));
        if (err != ESP_OK) s_radio_on = false;
    }
}

void wifi_sta_release(void)
{
    taskENTER_CRITICAL(&s_lock);
    if (s_holders > 0) s_holders--;
    bool idle = s_holders == 0;
    taskEXIT_CRITICAL(&s_lock);
    if (idle && s_linger) {
        esp_timer_stop(s_linger);
        esp_timer_start_once(s_linger, (uint64_t)WIFI_LINGER_S * 1000000ULL);
    }
}

void wifi_sta_stop_if_idle(void)
{
    taskENTER_CRITICAL(&s_lock);
    bool idle = s_holders == 0;
    taskEXIT_CRITICAL(&s_lock);
    if (!idle) return;
    if (s_linger) esp_timer_stop(s_linger);
    radio_off();
}

bool wifi_sta_radio_on(void) { return s_radio_on; }

esp_err_t wifi_sta_start(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    store_load();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &on_wifi_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &on_wifi_event, NULL, NULL));

    /* The driver itself is only initialised while an app holds Wi-Fi
     * (wifi_sta_hold); when off it is deinitialised so its internal-RAM
     * buffers are free for Bluetooth. */
    const esp_timer_create_args_t linger = { .callback = linger_expired, .name = "wifi_linger" };
    ESP_ERROR_CHECK(esp_timer_create(&linger, &s_linger));
    ESP_LOGI(TAG, "STA started, %d saved network(s)", s_count);
    return ESP_OK;
}
