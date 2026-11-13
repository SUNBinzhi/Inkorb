#include "inkorb_action.h"
#include "inkorb_round.h"
#include "esp_log.h"
#include "esp_random.h"
#include "nvs.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "wifi_sta.h"
#include <stdio.h>
#include <string.h>

/* Wire format: DJI R SDK protocol and DATA segment, commands 0019 and 1D03. */
enum { IDLE, SCANNING, FOUND, CONNECTING, PAIRING, READY, SENDING, FAILED, SWITCHING, ASLEEP, WAKING, DETACHED };
enum { SCAN = 1, CLOSE, CONNECT_0, CONNECT_1, CONNECT_2, START, STOP, OPEN, SWITCH, SLEEP, WAKE, CONNECT };
enum { MAX_CAMERAS = 3 };
typedef struct {
    ble_addr_t address;
    char name[24];
    bool asleep;            /* we put it to sleep: wake before connecting */
} camera_t;
/* Camera status push (1D02), subscribed via 1D05 once connected. */
typedef struct {
    bool valid, recording;
    uint8_t mode, resolution, fps;
    uint16_t record_s;
    uint8_t battery;        /* 0-100, 0xff unknown */
    uint8_t heat;           /* temp_over: 0 ok .. 3 shutting down */
    uint32_t local_start;   /* lv_tick when we saw recording start (fallback) */
} camera_status_t;
typedef struct {
    int state, count, pin;
    camera_status_t cam;
    char selected[24];
    const char *detail;
    camera_t cameras[MAX_CAMERAS];
} action_view_t;

static action_view_t view;
static portMUX_TYPE view_lock = portMUX_INITIALIZER_UNLOCKED;
static QueueHandle_t requests;
static struct ble_npl_callout poll;
static struct ble_npl_event kick;   /* runs queued taps at once, not on the 100 ms poll */
static bool started, synced, closing, requested_stop, reopen_pending, rescan_pending;
/* Bluetooth runs only while the page is open: started on open, the whole
 * NimBLE stack (controller included) torn down once a close has finished. */
static volatile bool close_done, stopping;
static uint8_t own_type, own_address[6];
static char pairing_key[15];
static bool known_camera;
static uint16_t link = BLE_HS_CONN_HANDLE_NONE, service_start, service_end;
static uint16_t notify_handle, write_handle, notify_end, cccd, sequence, record_seq, sleep_seq;
static ble_addr_t peer;
/* Cameras we left asleep, so they can be woken after switching away (DJI
 * allows waking for 30 min; a sleeping camera may not show up in a scan). */
static camera_t sleepers[MAX_CAMERAS];
static uint32_t slept_at[MAX_CAMERAS];
static void note_sleep(bool asleep);
/* Sleeping cameras keep their link while we talk to another one: DJI only
 * honours the wake broadcast while that link is up. */
typedef struct {
    uint16_t conn, notify_handle, write_handle;
    ble_addr_t address;
} parked_t;
static parked_t parked[2] = {{BLE_HS_CONN_HANDLE_NONE}, {BLE_HS_CONN_HANDLE_NONE}};

static int find_parked(const ble_addr_t *address)
{
    for (int i = 0; i < 2; ++i)
        if (parked[i].conn != BLE_HS_CONN_HANDLE_NONE && !ble_addr_cmp(&parked[i].address, address)) return i;
    return -1;
}       /* camera of the current session, for wake + reconnect */
static bool adv_on;
static uint32_t remote_id;
static unsigned deadline;
static uint8_t incoming[1023];
static size_t received;
static lv_obj_t *page, *status, *device_name, *switch_button, *scan_arc, *scan_caption, *switch_caption, *shutter_dot, *shutter_hint, *rows[MAX_CAMERAS], *names[MAX_CAMERAS], *tags[MAX_CAMERAS], *icon_action[MAX_CAMERAS], *icon_360[MAX_CAMERAS], *scan_button, *shutter;
static lv_timer_t *timer;
static uint8_t heat_seen;
static lv_obj_t *link_dot, *battery_label, *power_button, *power_caption, *connect_button, *connect_caption, *heat_card, *heat_text, *rec_badge, *rec_label, *spec_label, *res_box, *res_value, *fps_box, *fps_value_label;

/* Big value over a small caption, centred at x offset from screen centre. */
static lv_obj_t *side_stat(lv_obj_t *parent, int32_t x, int32_t y, const char *caption, lv_obj_t **value)
{
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, 100, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(box, LV_OBJ_FLAG_HIDDEN);
    *value = lv_label_create(box);
    lv_obj_set_style_text_font(*value, &lv_font_montserrat_36, 0);
    lv_obj_set_style_text_color(*value, lv_color_hex(ROUND_WHITE), 0);
    lv_label_set_text(*value, "");
    lv_obj_t *cap = lv_label_create(box);
    lv_obj_set_style_text_font(cap, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(cap, lv_color_hex(ROUND_FAINT), 0);
    lv_obj_set_style_text_letter_space(cap, 2, 0);
    lv_label_set_text(cap, caption);
    lv_obj_align(box, LV_ALIGN_CENTER, x, y);
    return box;
}
static inkorb_home_t *home;

static action_view_t snapshot(void)
{
    portENTER_CRITICAL(&view_lock);
    action_view_t copy = view;
    portEXIT_CRITICAL(&view_lock);
    return copy;
}

static void state(int next, const char *detail)
{
    portENTER_CRITICAL(&view_lock);
    view.state = next;
    view.detail = detail;
    portEXIT_CRITICAL(&view_lock);
    ESP_LOGI("inkorb_action", "state=%d %s", next, detail ? detail : "");
}

static void start_wake(void);
static bool manual_connect;
/* Wake retries: up to 3 rounds of (3 s wake broadcast + 8 s connect). While
 * broadcasting we also listen, to tell "asleep but deaf" from "powered off". */
static int wake_tries;
static bool target_seen;   /* direct connect failed -> wake, then connect */
static int connect_ms = 12000;
static bool wake_retry;   /* reconnecting after a wake: failure means "still asleep" */

static void fail(const char *detail)
{
    /* Late errors from an abandoned attempt (e.g. its MTU callback) must not
     * break a wake that is already under way. */
    int now_state = snapshot().state;
    if (now_state == WAKING || now_state == DETACHED) return;
    deadline = 0;
    if (manual_connect) {         /* Connect got no answer: camera is likely off, wake it */
        manual_connect = false;
        if (link != BLE_HS_CONN_HANDLE_NONE) ble_gap_terminate(link, BLE_ERR_REM_USER_CONN_TERM);
        else if (ble_gap_conn_active()) ble_gap_conn_cancel();
        state(DETACHED, NULL);
        wake_tries = 0;
        target_seen = false;
        start_wake();             /* then ADV_COMPLETE connects; failure -> DETACHED */
        return;
    }
    if (wake_retry) {
        wake_retry = false;
        if (link == BLE_HS_CONN_HANDLE_NONE && wake_tries < 2) {
            ++wake_tries;
            if (ble_gap_conn_active()) ble_gap_conn_cancel();
            ESP_LOGI("inkorb_action", "wake retry %d (camera seen: %d)", wake_tries, target_seen);
            start_wake();
            return;
        }
        ESP_LOGI("inkorb_action", "wake gave up (camera seen: %d)", target_seen);
        state(link != BLE_HS_CONN_HANDLE_NONE ? ASLEEP : DETACHED,
              target_seen ? "Camera did not respond" : "Camera seems off\nTurn it on, then Connect");
        if (link != BLE_HS_CONN_HANDLE_NONE) ble_gap_terminate(link, BLE_ERR_REM_USER_CONN_TERM);
        else if (ble_gap_conn_active()) ble_gap_conn_cancel();
        return;
    }
    state(FAILED, detail);
    if (link != BLE_HS_CONN_HANDLE_NONE) ble_gap_terminate(link, BLE_ERR_REM_USER_CONN_TERM);
    else if (ble_gap_conn_active()) ble_gap_conn_cancel();
}

static uint32_t checksum(const uint8_t *bytes, size_t count, uint32_t polynomial)
{
    uint32_t crc = 0x3aa3;
    for (size_t i = 0; i < count; ++i) {
        crc ^= bytes[i];
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((crc & 1) ? polynomial : 0);
    }
    return crc;
}

static void put_le(uint8_t *bytes, uint32_t value, size_t length)
{
    for (size_t i = 0; i < length; ++i) bytes[i] = (uint8_t)(value >> (8 * i));
}

static uint32_t get_le(const uint8_t *bytes, size_t length)
{
    uint32_t value = 0;
    for (size_t i = 0; i < length; ++i) value |= (uint32_t)bytes[i] << (8 * i);
    return value;
}

static int frame_written(uint16_t conn, const struct ble_gatt_error *error,
                         struct ble_gatt_attr *attr, void *arg)
{
    (void)attr; (void)arg;
    if (!closing && conn == link && error->status) {
        if (snapshot().state == PAIRING) fail("Pairing write failed");
        else if (snapshot().state == SENDING) {
            deadline = 0;
            state(READY, "Camera write failed");
        }
    }
    return 0;
}

static int send_frame(uint8_t set, uint8_t cmd, uint8_t type, uint16_t seq,
                      const uint8_t *payload, size_t length)
{
    uint8_t frame[51] = {0xaa};
    size_t total = length + 18;
    put_le(frame + 1, total, 2);
    frame[3] = type;
    put_le(frame + 8, seq, 2);
    put_le(frame + 10, checksum(frame, 10, 0xa001), 2);
    frame[12] = set;
    frame[13] = cmd;
    memcpy(frame + 14, payload, length);
    put_le(frame + total - 4, checksum(frame, total - 4, 0xedb88320), 4);
    uint16_t mtu = ble_att_mtu(link);
    if (mtu < total + 3) return BLE_HS_EMSGSIZE;
    if (type & 0x1f) return ble_gattc_write_flat(link, write_handle, frame, total, frame_written, NULL);
    return ble_gattc_write_no_rsp_flat(link, write_handle, frame, total);
}

/* Keep the camera's application-level approval separate from BLE bonding. */
static bool pairing_saved(void)
{
    nvs_handle_t nvs;
    uint8_t approved = 0;
    if (nvs_open("inkorb_dji", NVS_READONLY, &nvs) == ESP_OK) {
        nvs_get_u8(nvs, pairing_key, &approved);
        nvs_close(nvs);
    }
    return approved == 1;
}

/* Cameras connected before, most recent first, kept in NVS so the list is
 * ready without a scan. */
static camera_t known[MAX_CAMERAS];
static void save_known(void);
static int known_count = -1;

static void load_known(void)
{
    if (known_count >= 0) return;
    known_count = 0;
    nvs_handle_t nvs;
    if (nvs_open("inkorb_dji", NVS_READONLY, &nvs) != ESP_OK) return;
    size_t size = sizeof(known);
    if (nvs_get_blob(nvs, "known", known, &size) == ESP_OK) known_count = size / sizeof(camera_t);
    nvs_close(nvs);
    /* Only the list is remembered; sleep state is live-only (it goes stale
     * as soon as someone presses a button on the camera). */
    for (int i = 0; i < known_count; ++i) known[i].asleep = false;
}

static void remember_camera(const char *name)
{
    load_known();
    camera_t entry = {.address = peer};
    snprintf(entry.name, sizeof(entry.name), "%s", name);
    int at = known_count < MAX_CAMERAS ? known_count : MAX_CAMERAS - 1;
    for (int i = 0; i < known_count; ++i)
        if (!ble_addr_cmp(&known[i].address, &peer)) { at = i; break; }
    if (at == known_count) ++known_count;
    memmove(known + 1, known, at * sizeof(camera_t));
    known[0] = entry;
    save_known();
}

static void save_known(void)
{
    nvs_handle_t nvs;
    if (nvs_open("inkorb_dji", NVS_READWRITE, &nvs) == ESP_OK) {
        if (nvs_set_blob(nvs, "known", known, known_count * sizeof(camera_t)) == ESP_OK) nvs_commit(nvs);
        nvs_close(nvs);
    }
}

/* List = cameras left asleep, then remembered ones; a scan adds the rest. */
static void fill_list(void)
{
    load_known();
    uint32_t now = xTaskGetTickCount() / configTICK_RATE_HZ;
    portENTER_CRITICAL(&view_lock);
    view.count = 0;
    for (int i = 0; i < MAX_CAMERAS; ++i)
        if (sleepers[i].asleep && now - slept_at[i] < 30 * 60) view.cameras[view.count++] = sleepers[i];
    for (int i = 0; i < known_count && view.count < MAX_CAMERAS; ++i) {
        bool dup = false;
        for (int j = 0; j < view.count; ++j) dup |= !ble_addr_cmp(&view.cameras[j].address, &known[i].address);
        if (!dup) view.cameras[view.count++] = known[i];
    }
    portEXIT_CRITICAL(&view_lock);
}

static void show_list(void)
{
    fill_list();
    state(FOUND, snapshot().count ? "Tap a camera or Search" : "Tap Search");
}

static void save_pairing(void)
{
    nvs_handle_t nvs;
    esp_err_t error = nvs_open("inkorb_dji", NVS_READWRITE, &nvs);
    if (error == ESP_OK) {
        error = nvs_set_u8(nvs, pairing_key, 1);
        if (error == ESP_OK) error = nvs_commit(nvs);
        nvs_close(nvs);
    }
    if (error != ESP_OK) ESP_LOGW("inkorb_action", "Pairing save: %s", esp_err_to_name(error));
}

static void consume_frame(const uint8_t *frame, size_t length)
{
    int current = snapshot().state;
    uint16_t seq = get_le(frame + 8, 2);
    bool reply = frame[3] & 0x20;
    const uint8_t *payload = frame + 14;
    size_t size = length - 18;
    if (current == PAIRING && frame[12] == 0 && frame[13] == 0x19) {
        if (reply && seq == sequence && size >= 5 && payload[4] != 0) {
            fail("Camera rejected request");
        } else if (!reply && size >= 29 && payload[26] == 2) {
            if (get_le(payload + 27, 2) != 0) {
                fail("Pairing declined");
                return;
            }
            uint8_t ack[9] = {0};
            put_le(ack, remote_id, 4);
            if (send_frame(0, 0x19, 0x20, seq, ack, sizeof(ack))) fail("Pairing reply failed");
            else {
                deadline = 0;
                wake_retry = false;
                manual_connect = false;
                portENTER_CRITICAL(&view_lock);
                memset(&view.cam, 0, sizeof(view.cam));
                portEXIT_CRITICAL(&view_lock);
                state(READY, "Connected");
                /* Ask for status pushes: on change + periodic, 2 Hz. */
                uint8_t sub[6] = {3, 20, 0, 0, 0, 0};
                send_frame(0x1d, 5, 0, ++sequence, sub, sizeof(sub));
                if (!known_camera) save_pairing();
                remember_camera(snapshot().selected);
            }
        }
    } else if (!reply && frame[12] == 0x1d && frame[13] == 2 && size >= 7) {
        portENTER_CRITICAL(&view_lock);
        bool was = view.cam.recording;
        view.cam.valid = true;
        view.cam.mode = payload[0];
        view.cam.recording = payload[1] == 3 && payload[0] != 5;
        view.cam.resolution = payload[2];
        view.cam.fps = payload[3];
        view.cam.record_s = get_le(payload + 5, 2);
        view.cam.battery = size >= 38 ? payload[37] : 0xff;
        view.cam.heat = size >= 31 ? payload[30] : 0;
        bool asleep = size >= 29 && payload[28] == 3;
        if (view.cam.recording && !was) view.cam.local_start = lv_tick_get();
        portEXIT_CRITICAL(&view_lock);
        /* Slept from the camera's own power button: stop talking to it. */
        if (asleep && current == READY) { note_sleep(true); state(ASLEEP, NULL); }
        /* Woke without dropping the link: just carry on. */
        else if (!asleep && current == WAKING) { deadline = 0; state(READY, NULL); }
        if (!asleep && current == READY) note_sleep(false);
    } else if (reply && seq == sleep_seq && frame[12] == 0 && frame[13] == 0x1a && size >= 1) {
        deadline = 0;
        if (payload[0] == 0) { note_sleep(true); state(ASLEEP, NULL); }
        else state(READY, "Sleep rejected");
    } else if (current == SENDING && reply && seq == record_seq &&
               frame[12] == 0x1d && frame[13] == 3 && size >= 1) {
        deadline = 0;
        bool ok = payload[0] == 0;
        portENTER_CRITICAL(&view_lock);
        if (ok && !view.cam.valid) {      /* no status pushes: time it ourselves */
            view.cam.recording = !requested_stop;
            view.cam.local_start = lv_tick_get();
        }
        portEXIT_CRITICAL(&view_lock);
        state(READY, ok ? (requested_stop ? "Stop confirmed" : "Start confirmed")
                        : (requested_stop ? "Stop rejected" : "Start rejected"));
    }
}

static void notification(struct os_mbuf *om)
{
    size_t count = OS_MBUF_PKTLEN(om);
    if (count > sizeof(incoming) - received) received = 0;
    if (count > sizeof(incoming) || os_mbuf_copydata(om, 0, count, incoming + received)) return;
    received += count;
    size_t offset = 0;
    while (offset < received) {
        const uint8_t *frame = incoming + offset;
        if (frame[0] != 0xaa) { ++offset; continue; }
        if (received - offset < 12) break;
        size_t length = get_le(frame + 1, 2) & 0x3ff;
        if (length < 18 || get_le(frame + 10, 2) != checksum(frame, 10, 0xa001)) {
            ++offset;
            continue;
        }
        if (received - offset < length) break;
        if (get_le(frame + length - 4, 4) == checksum(frame, length - 4, 0xedb88320))
            consume_frame(frame, length);
        offset += length;
    }
    received -= offset;
    memmove(incoming, incoming + offset, received);
}

static int subscribed(uint16_t conn, const struct ble_gatt_error *error,
                      struct ble_gatt_attr *attr, void *arg)
{
    (void)conn; (void)attr; (void)arg;
    if (closing) return 0;
    if (error->status) { fail("Notify setup failed"); return 0; }
    uint8_t request[33] = {0};
    put_le(request, remote_id, 4);
    request[4] = 6;
    for (int i = 0; i < 6; ++i) request[5 + i] = own_address[5 - i];
    known_camera = pairing_saved();
    request[26] = known_camera ? 0 : 1;
    uint16_t pin = esp_random() % 10000;
    put_le(request + 27, pin, 2);
    portENTER_CRITICAL(&view_lock);
    view.pin = pin;
    portEXIT_CRITICAL(&view_lock);
    state(PAIRING, known_camera ? "Reconnecting..." : NULL);
    deadline = manual_connect ? 60 : 400;   /* known camera answers fast */
    if (send_frame(0, 0x19, 2, ++sequence, request, sizeof(request))) fail("Pairing request failed");
    return 0;
}

static int descriptor(uint16_t conn, const struct ble_gatt_error *error, uint16_t value,
                      const struct ble_gatt_dsc *dsc, void *arg)
{
    (void)value; (void)arg;
    if (closing) return 0;
    if (!error->status && ble_uuid_u16(&dsc->uuid.u) == 0x2902) cccd = dsc->handle;
    if (error->status == BLE_HS_EDONE) {
        const uint8_t enable[] = {1, 0};
        if (!cccd || ble_gattc_write_flat(conn, cccd, enable, sizeof(enable), subscribed, NULL))
            fail("Notify setup failed");
    } else if (error->status) fail("Descriptor discovery failed");
    return 0;
}

static int characteristic(uint16_t conn, const struct ble_gatt_error *error,
                          const struct ble_gatt_chr *chr, void *arg)
{
    (void)arg;
    if (closing) return 0;
    if (!error->status) {
        if (notify_handle && notify_end == service_end) notify_end = chr->def_handle - 1;
        uint16_t uuid = ble_uuid_u16(&chr->uuid.u);
        if (uuid == 0xfff4) notify_handle = chr->val_handle;
        if (uuid == 0xfff5) write_handle = chr->val_handle;
    } else if (error->status == BLE_HS_EDONE) {
        if (!notify_handle || !write_handle || notify_end <= notify_handle ||
            ble_gattc_disc_all_dscs(conn, notify_handle, notify_end, descriptor, NULL))
            fail("Action characteristics missing");
    } else fail("Characteristic discovery failed");
    return 0;
}

static int service(uint16_t conn, const struct ble_gatt_error *error,
                   const struct ble_gatt_svc *svc, void *arg)
{
    (void)arg;
    if (closing) return 0;
    if (!error->status) {
        service_start = svc->start_handle;
        notify_end = service_end = svc->end_handle;
    } else if (error->status == BLE_HS_EDONE) {
        if (!service_start || ble_gattc_disc_all_chrs(conn, service_start, service_end, characteristic, NULL))
            fail("Action service missing");
    } else fail("Service discovery failed");
    return 0;
}

static int mtu_ready(uint16_t conn, const struct ble_gatt_error *error, uint16_t mtu, void *arg)
{
    (void)arg;
    if (!closing && (error->status || mtu < 54 ||
        ble_gattc_disc_svc_by_uuid(conn, BLE_UUID16_DECLARE(0xfff0), service, NULL)))
        fail("Bluetooth setup failed");
    return 0;
}

static void discover(const struct ble_gap_disc_desc *disc)
{
    struct ble_hs_adv_fields adv;
    if (ble_hs_adv_parse_fields(&adv, disc->data, disc->length_data)) return;
    bool dji = adv.mfg_data_len >= 5 && adv.mfg_data[0] == 0xaa &&
               adv.mfg_data[1] == 8 && adv.mfg_data[4] == 0xfa;
    bool named = adv.name_len >= 10 && !memcmp(adv.name, "OsmoAction", 10);
    portENTER_CRITICAL(&view_lock);
    int index = 0;
    while (index < view.count && ble_addr_cmp(&view.cameras[index].address, &disc->addr)) ++index;
    if (index < MAX_CAMERAS && (index < view.count || dji || named)) {
        camera_t *camera = &view.cameras[index];
        if (index == view.count) {
            camera->address = disc->addr;
            snprintf(camera->name, sizeof(camera->name), "DJI %02X%02X", disc->addr.val[1], disc->addr.val[0]);
            ++view.count;
        }
        if (adv.name_len) {
            size_t n = adv.name_len < sizeof(camera->name) - 1 ? adv.name_len : sizeof(camera->name) - 1;
            memcpy(camera->name, adv.name, n);
            camera->name[n] = 0;
        }
    }
    portEXIT_CRITICAL(&view_lock);
}

static void process_request(int request);
static void connect_peer(void);

/* CLOSE can finish after the user has reopened the page. */
static void finish_close(void)
{
    if (!closing || link != BLE_HS_CONN_HANDLE_NONE || ble_gap_conn_active()) return;
    close_done = true;
    if (reopen_pending) {
        closing = false;
        reopen_pending = false;
        show_list();
    }
}

static int gap(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_DISC:
        if (snapshot().state == SCANNING) discover(&event->disc);
        else if (snapshot().state == WAKING && !target_seen && !ble_addr_cmp(&event->disc.addr, &peer)) {
            target_seen = true;
            ESP_LOGI("inkorb_action", "camera heard while waking (rssi %d)", event->disc.rssi);
        }
        break;
    case BLE_GAP_EVENT_DISC_COMPLETE:
        if (snapshot().state == SCANNING) state(FOUND, NULL);
        break;
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status) {
            if (closing) finish_close();
            else if (snapshot().state != FAILED) fail("Connection failed");
        } else {
            link = event->connect.conn_handle;
            if (closing || snapshot().state != CONNECTING) ble_gap_terminate(link, BLE_ERR_REM_USER_CONN_TERM);
            else if (ble_gattc_exchange_mtu(link, mtu_ready, NULL)) fail("MTU request failed");
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        for (int i = 0; i < 2; ++i)
            if (parked[i].conn == event->disconnect.conn.conn_handle) {
                parked[i].conn = BLE_HS_CONN_HANDLE_NONE;
                return 0;
            }
        if (event->disconnect.conn.conn_handle != link) return 0;
        link = BLE_HS_CONN_HANDLE_NONE;
        deadline = 0;
        if (closing) finish_close();
        else if (snapshot().state == SWITCHING) rescan_pending = true;
        /* Woken (by us or the camera's buttons): link drops, reconnect. */
        /* Asleep: the camera may drop the link; stay asleep, Wake still works.
         * Waking: the camera drops it as it wakes, so reconnect. */
        else if (snapshot().state == ASLEEP) {}
        else if (snapshot().state == WAKING) {
            if (!adv_on) { wake_retry = true; connect_peer(); }
        }
        else if (!closing && snapshot().state != FAILED) state(FOUND, "Disconnected");
        break;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        adv_on = false;
        if (!closing && snapshot().state == WAKING && link == BLE_HS_CONN_HANDLE_NONE) {
            wake_retry = true;
            connect_ms = 8000;
            connect_peer();
        }
        break;
    case BLE_GAP_EVENT_NOTIFY_RX:
        if (!closing && event->notify_rx.conn_handle == link &&
            event->notify_rx.attr_handle == notify_handle) notification(event->notify_rx.om);
        break;
    }
    return 0;
}

static void note_sleep(bool asleep)
{
    int slot = -1;
    for (int i = 0; i < MAX_CAMERAS; ++i)
        if (sleepers[i].asleep && !ble_addr_cmp(&sleepers[i].address, &peer)) slot = i;
    if (asleep && slot < 0) {
        slot = 0;
        for (int i = 0; i < MAX_CAMERAS; ++i)
            if (!sleepers[i].asleep || slept_at[i] < slept_at[slot]) { slot = i; if (!sleepers[i].asleep) break; }
    }
    portENTER_CRITICAL(&view_lock);
    if (slot >= 0) {
        sleepers[slot].address = peer;
        memcpy(sleepers[slot].name, view.selected, sizeof(sleepers[slot].name));
        sleepers[slot].asleep = asleep;
        slept_at[slot] = xTaskGetTickCount() / configTICK_RATE_HZ;
    }
    for (int i = 0; i < view.count; ++i)
        if (!ble_addr_cmp(&view.cameras[i].address, &peer)) view.cameras[i].asleep = asleep;
    portEXIT_CRITICAL(&view_lock);
}

static void connect_peer(void)
{
    snprintf(pairing_key, sizeof(pairing_key), "%02X%02X%02X%02X%02X%02X%02X",
             peer.type, peer.val[0], peer.val[1], peer.val[2], peer.val[3], peer.val[4], peer.val[5]);
    state(CONNECTING, NULL);
    if (ble_gap_disc_active()) ble_gap_disc_cancel();
    service_start = service_end = notify_handle = write_handle = notify_end = cccd = 0;
    received = 0;
    deadline = 180;
    if (ble_gap_connect(own_type, &peer, connect_ms, NULL, gap, NULL)) fail("Connection request failed");
    connect_ms = 12000;
}

/* DJI wake-up: advertise "WKP" + the camera's MAC (LSB first) for 2 s. The
 * camera drops the old link as it wakes; we then reconnect to it. */
static void start_wake(void)
{
    uint8_t adv[11] = {10, 0xff, 'W', 'K', 'P'};
    for (int i = 0; i < 6; ++i) adv[5 + i] = peer.val[i];
    struct ble_gap_adv_params params = {
        .conn_mode = BLE_GAP_CONN_MODE_UND,
        .disc_mode = BLE_GAP_DISC_MODE_GEN,
        .itvl_min = 0x20,
        .itvl_max = 0x40,
    };
    int rc = ble_gap_adv_set_data(adv, sizeof(adv));
    if (!rc) rc = ble_gap_adv_start(own_type, NULL, 3000, &params, gap, NULL);
    if (rc) {   /* connectable adv may be refused while linked: plain broadcast */
        params.conn_mode = BLE_GAP_CONN_MODE_NON;
        rc = ble_gap_adv_start(own_type, NULL, 3000, &params, gap, NULL);
    }
    if (!rc && link == BLE_HS_CONN_HANDLE_NONE && !ble_gap_disc_active()) {
        struct ble_gap_disc_params listen = {.itvl = 0x50, .window = 0x30, .passive = 1};
        ble_gap_disc(own_type, 3000, &listen, gap, NULL);
    }
    ESP_LOGI("inkorb_action", "wake adv rc=%d", rc);
    if (rc) {
        state(link != BLE_HS_CONN_HANDLE_NONE ? ASLEEP : DETACHED, "Wake failed");
        return;
    }
    adv_on = true;
    state(WAKING, NULL);
    deadline = 250;
}

static void process_request(int request)
{
    action_view_t current = snapshot();
    if (request == OPEN) {
        if (closing) {
            reopen_pending = true;
            state(IDLE, "Disconnecting...");
            finish_close();
        } else if (current.state == IDLE) show_list();
    } else if (request == CLOSE) {
        closing = true;
        reopen_pending = false;
        deadline = 0;
        state(IDLE, NULL);
        if (ble_gap_disc_active()) ble_gap_disc_cancel();
        if (adv_on) { ble_gap_adv_stop(); adv_on = false; }
        if (ble_gap_conn_active()) ble_gap_conn_cancel();
        if (link != BLE_HS_CONN_HANDLE_NONE) ble_gap_terminate(link, BLE_ERR_REM_USER_CONN_TERM);
        for (int i = 0; i < 2; ++i)
            if (parked[i].conn != BLE_HS_CONN_HANDLE_NONE) {
                ble_gap_terminate(parked[i].conn, BLE_ERR_REM_USER_CONN_TERM);
                parked[i].conn = BLE_HS_CONN_HANDLE_NONE;
            }
    } else if (request == SWITCH && (current.state == ASLEEP || current.state == WAKING)) {
        if (adv_on) { ble_gap_adv_stop(); adv_on = false; }
        wake_retry = false;
        if (ble_gap_conn_active()) ble_gap_conn_cancel();
        state(SWITCHING, "Switching camera...");
        int slot = parked[0].conn == BLE_HS_CONN_HANDLE_NONE ? 0 : parked[1].conn == BLE_HS_CONN_HANDLE_NONE ? 1 : -1;
        if (link != BLE_HS_CONN_HANDLE_NONE && current.state == ASLEEP && slot >= 0) {
            parked[slot] = (parked_t){link, notify_handle, write_handle, peer};
            link = BLE_HS_CONN_HANDLE_NONE;
            rescan_pending = true;
        } else if (link != BLE_HS_CONN_HANDLE_NONE) ble_gap_terminate(link, BLE_ERR_REM_USER_CONN_TERM);
        else rescan_pending = true;
        deadline = 50;
    } else if (request == SWITCH && current.state == READY) {
        if (ble_gap_terminate(link, BLE_ERR_REM_USER_CONN_TERM)) {
            state(READY, "Could not disconnect");
        } else {
            state(SWITCHING, "Switching camera...");
            deadline = 50;
        }
    } else if (request == SCAN && link == BLE_HS_CONN_HANDLE_NONE && !ble_gap_conn_active()) {
        closing = false;
        if (ble_gap_disc_active()) ble_gap_disc_cancel();
        fill_list();
        /* Wi-Fi is off while this page is open, so scan 100% of the time. */
        struct ble_gap_disc_params params = {
            .itvl = 0x50,
            .window = 0x50,
            .filter_duplicates = 0,
        };
        state(SCANNING, NULL);
        if (ble_gap_disc(own_type, 7000, &params, gap, NULL)) fail("Scan failed");
    } else if (!closing && request >= CONNECT_0 && request <= CONNECT_2 &&
               (current.state == FOUND || current.state == SCANNING) &&
               request - CONNECT_0 < current.count && link == BLE_HS_CONN_HANDLE_NONE) {
        portENTER_CRITICAL(&view_lock);
        memcpy(view.selected, current.cameras[request - CONNECT_0].name, sizeof(view.selected));
        portEXIT_CRITICAL(&view_lock);
        peer = current.cameras[request - CONNECT_0].address;
        int slot = find_parked(&peer);
        if (slot >= 0) {        /* still linked: resume it asleep, then wake */
            if (ble_gap_disc_active()) ble_gap_disc_cancel();
            link = parked[slot].conn;
            notify_handle = parked[slot].notify_handle;
            write_handle = parked[slot].write_handle;
            parked[slot].conn = BLE_HS_CONN_HANDLE_NONE;
            received = 0;
            snprintf(pairing_key, sizeof(pairing_key), "%02X%02X%02X%02X%02X%02X%02X",
                     peer.type, peer.val[0], peer.val[1], peer.val[2], peer.val[3], peer.val[4], peer.val[5]);
            state(ASLEEP, NULL);
        } else {
            /* Open the control page; the user picks Connect or Wake. */
            if (ble_gap_disc_active()) ble_gap_disc_cancel();
            state(DETACHED, NULL);
        }
    } else if (request == SLEEP && current.state == READY) {
        uint8_t mode = 3;
        sleep_seq = ++sequence;
        state(SENDING, "Sleeping...");
        deadline = 40;
        if (send_frame(0, 0x1a, 1, sleep_seq, &mode, 1)) {
            deadline = 0;
            state(READY, "Send failed");
        }
    } else if (request == CONNECT && current.state == DETACHED) {
        manual_connect = true;
        connect_ms = 5000;
        connect_peer();
        deadline = 50;          /* 5 s, then wake instead */
    } else if (request == WAKE && current.state == DETACHED) {
        wake_tries = 0;
        target_seen = false;
        start_wake();       /* ADV_COMPLETE then connects */
    } else if (request == SWITCH && current.state == DETACHED) {
        show_list();
    } else if (request == WAKE && current.state == ASLEEP) {
        wake_tries = 0;
        target_seen = false;
        start_wake();
    } else if ((request == START || request == STOP) && current.state == READY) {
        uint8_t payload[9] = {0};
        put_le(payload, 0x33ff0000, 4);
        requested_stop = request == STOP;
        payload[4] = requested_stop;
        record_seq = ++sequence;
        state(SENDING, requested_stop ? "Stopping..." : "Starting...");
        deadline = 40;
        if (send_frame(0x1d, 3, 1, record_seq, payload, sizeof(payload))) {
            deadline = 0;
            state(READY, "Send failed");
        }
    }
}

static void poll_requests(struct ble_npl_event *event)
{
    (void)event;
    if (deadline && --deadline == 0) {
        if (snapshot().state == SENDING) state(READY, "No camera reply");
        else if (snapshot().state == WAKING) state(ASLEEP, "Camera did not wake");
        else fail("Connection timed out");
    }
    finish_close();
    int request;
    while (xQueueReceive(requests, &request, 0) == pdTRUE)
        if (synced) process_request(request);
    if (rescan_pending) {
        rescan_pending = false;
        if (!closing && snapshot().state == SWITCHING) {
            if (snapshot().count) state(FOUND, NULL);
            else process_request(SCAN);
        }
    }
    ble_npl_callout_reset(&poll, ble_npl_time_ms_to_ticks32(100));
}

static void run_kick(struct ble_npl_event *event)
{
    (void)event;
    int request;
    while (xQueueReceive(requests, &request, 0) == pdTRUE)
        if (synced) process_request(request);
}

static void sync_host(void)
{
    if (ble_hs_util_ensure_addr(0) || ble_hs_id_infer_auto(0, &own_type) ||
        ble_hs_id_copy_addr(own_type, own_address, NULL)) {
        state(FAILED, "Bluetooth address unavailable");
        return;
    }
    remote_id = get_le(own_address, 4);
    synced = true;
    if (snapshot().state == IDLE) show_list();
}

static void run_host(void *arg)
{
    (void)arg;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static void post(int request)
{
    if (requests) xQueueSend(requests, &request, 0);
    if (started && !stopping) ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &kick);
}

/* Tap feedback before Bluetooth answers: the status shows the action at once
 * and holds until the camera state actually moves on. */
static const char *pending_text;
static int pending_from = -1;

static void click(lv_event_t *event)
{
    int request = (int)(intptr_t)lv_event_get_user_data(event);
    if (request == SCAN || request == SWITCH) {
        pending_text = request == SCAN ? "Searching..." : "Switching camera...";
        pending_from = snapshot().state;
        lv_label_set_text(status, pending_text);
    }
    post(request);
}

static void toggle_power(lv_event_t *event)
{
    (void)event;
    int st = snapshot().state;
    post(st == ASLEEP || st == DETACHED ? WAKE : SLEEP);
}

static void dismiss_heat(lv_event_t *event)
{
    heat_seen = (uint8_t)(uintptr_t)lv_obj_get_user_data(lv_event_get_target(event));
    lv_obj_add_flag(heat_card, LV_OBJ_FLAG_HIDDEN);
}

static void toggle_record(lv_event_t *event)
{
    (void)event;
    post(snapshot().cam.recording ? STOP : START);
}

/* Round button captions are siblings, so hide them with their buttons. */
static void set_control_visible(lv_obj_t *button, lv_obj_t *caption, bool visible)
{
    if (visible) {
        lv_obj_remove_flag(button, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(caption, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(button, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(caption, LV_OBJ_FLAG_HIDDEN);
    }
}

static void update_label(lv_obj_t *label, const char *text)
{
    if (strcmp(lv_label_get_text(label), text)) lv_label_set_text(label, text);
}

static const char *resolution_name(uint8_t r)
{
    switch (r) {
    case 10: case 66: return "1080P";
    case 45: case 67: case 95: return "2.7K";
    case 16: case 103: case 109: return "4K";
    case 4: return "5.7K"; case 7: case 111: return "8K"; case 1: return "720P";
    default: return NULL;
    }
}

static int fps_value(uint8_t f)
{
    static const int table[] = {0, 24, 25, 30, 48, 50, 60, 120, 240, 0, 100};
    if (f < sizeof(table) / sizeof(table[0])) return table[f];
    return f == 19 ? 200 : 0;
}

static const char *mode_name(uint8_t m)
{
    switch (m) {
    case 0: return "Slow-mo"; case 1: return "Video"; case 2: return "Timelapse";
    case 5: return "Photo"; case 10: return "Hyperlapse"; case 0x1a: return "Live";
    case 0x23: return "UVC"; case 0x28: return "Night"; case 0x34: return "Follow";
    case 0x38: return "Panorama"; case 0x3a: return "Hyperlapse"; case 0x3c: return "Selfie";
    case 0x3f: return "Pano Photo"; case 0x41: return "Ultra Wide"; case 0x43: return "Bullet Time";
    case 0x44: case 0x4a: return "Night";
    default: return NULL;
    }
}

static void refresh_camera(const action_view_t *v)
{
    bool connected = v->state == READY || v->state == SENDING;
    const camera_status_t *c = &v->cam;
    if (connected && c->recording) {
        uint32_t secs = c->valid ? c->record_s : lv_tick_elaps(c->local_start) / 1000;
        char t[24];
        if (secs >= 3600) snprintf(t, sizeof(t), LV_SYMBOL_BULLET " %u:%02u:%02u", (unsigned)(secs / 3600), (unsigned)(secs / 60 % 60), (unsigned)(secs % 60));
        else snprintf(t, sizeof(t), LV_SYMBOL_BULLET " %02u:%02u", (unsigned)(secs / 60), (unsigned)(secs % 60));
        update_label(rec_label, t);
        lv_obj_remove_flag(rec_badge, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(status, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(rec_badge, LV_OBJ_FLAG_HIDDEN);
        if (!connected) lv_obj_remove_flag(status, LV_OBJ_FLAG_HIDDEN);
    }
    if (connected && c->recording) {
        lv_obj_add_state(shutter, LV_STATE_CHECKED);
        lv_obj_add_state(shutter_dot, LV_STATE_CHECKED);
        /* slow breathing glow while recording */
        uint32_t ph = lv_tick_get() % 1600;
        uint32_t tri = ph < 800 ? ph : 1600 - ph;
        lv_obj_set_style_shadow_opa(shutter, 40 + tri * 120 / 800, LV_STATE_CHECKED);
    } else {
        lv_obj_remove_state(shutter, LV_STATE_CHECKED);
        lv_obj_remove_state(shutter_dot, LV_STATE_CHECKED);
    }
    /* Spec sits in the round's side bays: resolution left, fps right,
     * mode under the shutter. */
    bool show = connected && c->valid;
    char res[12] = "", fps[8] = "";
    const char *m = show ? mode_name(c->mode) : NULL;
    if (show && c->mode != 5) {
        const char *r = resolution_name(c->resolution);
        if (r) snprintf(res, sizeof(res), "%s", r);
        else snprintf(res, sizeof(res), "#%u", c->resolution);
        int f = fps_value(c->fps);
        if (f) snprintf(fps, sizeof(fps), "%d", f);
    }
    update_label(res_value, res);
    update_label(fps_value_label, fps);
    update_label(spec_label, m ? m : "");
    if (res[0]) lv_obj_remove_flag(res_box, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(res_box, LV_OBJ_FLAG_HIDDEN);
    if (fps[0]) lv_obj_remove_flag(fps_box, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(fps_box, LV_OBJ_FLAG_HIDDEN);
    if (m && connected) lv_obj_remove_flag(spec_label, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(spec_label, LV_OBJ_FLAG_HIDDEN);
}

static void refresh(lv_timer_t *unused)
{
    (void)unused;
    action_view_t current = snapshot();
    static const char *const labels[] = {"Bluetooth starting", "Searching...", "Select camera", "Connecting...",
                                         "Confirm on camera", "Connected", "Waiting for camera", "Connection failed", "Switching camera...", "Camera asleep", "Waking camera...", "Not connected"};
    char message[64];
    const char *detail = current.detail ? current.detail : labels[current.state];
    if (current.state == PAIRING) {
        snprintf(message, sizeof(message), "%s\n%04d",
                 current.detail ? current.detail : "Confirm on camera", current.pin);
        detail = message;
    } else if (current.state == FOUND && !current.count && !current.detail) {
        detail = "No camera found";
    } else if (current.state == SCANNING && current.count) {
        snprintf(message, sizeof(message), "Searching... (%d)", current.count);
        detail = message;
    }
    if (pending_text && current.state == pending_from) detail = pending_text;
    else pending_text = NULL;
    update_label(status, detail);
    bool show_device = current.state == CONNECTING || current.state == PAIRING ||
                       current.state == READY || current.state == SENDING || current.state == SWITCHING ||
                       current.state == ASLEEP || current.state == WAKING;
    update_label(device_name, show_device ? current.selected : "");
    /* Connected: the status word becomes a dot beside the name —
     * green when fine, red when the camera rejected or a write failed. */
    bool awake = current.state == READY || current.state == SENDING;
    bool dozing = current.state == ASLEEP || current.state == WAKING || current.state == DETACHED;
    /* Connecting happens on the control page itself: controls greyed out,
     * a blinking amber dot and a one-line status until it is ready. */
    bool joining = current.state == CONNECTING || current.state == PAIRING;
    bool linked = awake || dozing || joining;
    if (linked) {
        bool bad = detail && (strstr(detail, "reject") || strstr(detail, "fail") || strstr(detail, "Could not") || strstr(detail, "did not"));
        uint32_t col = bad ? 0xFF4D5A : (current.state == ASLEEP || current.state == DETACHED) ? 0x6B7A88 : (current.state == WAKING || joining) ? 0xFFB45E : 0x3EE07F;
        lv_obj_set_style_bg_color(link_dot, lv_color_hex(col), 0);
        lv_obj_set_style_shadow_color(link_dot, lv_color_hex(col), 0);
        bool blink = current.state == SENDING || current.state == WAKING || joining;
        lv_obj_set_style_bg_opa(link_dot, blink && (lv_tick_get() / 250) % 2 ? LV_OPA_40 : LV_OPA_COVER, 0);
        /* name · dot · battery, centred as one group */
        uint8_t bat = current.cam.valid ? current.cam.battery : 0xff;
        bool show_bat = bat <= 100 && current.state != ASLEEP;
        if (show_bat) {
            char b[24];
            const char *sym = bat > 80 ? LV_SYMBOL_BATTERY_FULL : bat > 55 ? LV_SYMBOL_BATTERY_3 :
                              bat > 30 ? LV_SYMBOL_BATTERY_2 : bat > 10 ? LV_SYMBOL_BATTERY_1 : LV_SYMBOL_BATTERY_EMPTY;
            snprintf(b, sizeof(b), "%s %u%%", sym, bat);
            update_label(battery_label, b);
            lv_obj_set_style_text_color(battery_label, lv_color_hex(bat <= 15 ? 0xFF4D5A : 0x8FA3B6), 0);
            lv_obj_remove_flag(battery_label, LV_OBJ_FLAG_HIDDEN);
        } else lv_obj_add_flag(battery_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_update_layout(device_name);
        lv_obj_update_layout(battery_label);
        int32_t w = lv_obj_get_width(device_name) + 18 + (show_bat ? 10 + lv_obj_get_width(battery_label) : 0);
        lv_obj_align(device_name, LV_ALIGN_TOP_MID, -(w - lv_obj_get_width(device_name)) / 2, 74);
        lv_obj_align_to(link_dot, device_name, LV_ALIGN_OUT_RIGHT_MID, 8, 0);
        lv_obj_align_to(battery_label, link_dot, LV_ALIGN_OUT_RIGHT_MID, 10, 0);
        lv_obj_remove_flag(link_dot, LV_OBJ_FLAG_HIDDEN);
        if (awake) lv_obj_add_flag(status, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_remove_flag(status, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_align(device_name, LV_ALIGN_TOP_MID, 0, 74);
        lv_obj_add_flag(link_dot, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(battery_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(status, LV_OBJ_FLAG_HIDDEN);
    }
    /* Overheat pops up only when it happens; tap to dismiss until it worsens. */
    uint8_t heat = awake && current.cam.valid ? current.cam.heat : 0;
    if (!heat) heat_seen = 0;
    if (heat > heat_seen && heat <= 3) {
        static const char *const msg[] = {"", "Camera is getting hot", "Too hot to record", "Overheating\nCamera will shut down"};
        update_label(heat_text, msg[heat]);
        lv_obj_set_style_border_color(heat_card, lv_color_hex(heat >= 2 ? 0xFF4D5A : 0xFFB45E), 0);
        lv_obj_remove_flag(heat_card, LV_OBJ_FLAG_HIDDEN);
    } else lv_obj_add_flag(heat_card, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_user_data(heat_card, (void *)(uintptr_t)heat);

    if (current.state == SCANNING) {
        unsigned angle = (lv_tick_get() % 900) * 360 / 900;
        lv_obj_remove_flag(scan_arc, LV_OBJ_FLAG_HIDDEN);
        lv_arc_set_bg_angles(scan_arc, angle, (angle + 85) % 360);
    } else lv_obj_add_flag(scan_arc, LV_OBJ_FLAG_HIDDEN);
    bool list = current.state == FOUND || current.state == SCANNING;
    for (int i = 0; i < MAX_CAMERAS; ++i) {
        if (list && i < current.count) {
            update_label(names[i], current.cameras[i].name);
            bool is360 = strstr(current.cameras[i].name, "360") != NULL;
            if (is360) { lv_obj_add_flag(icon_action[i], LV_OBJ_FLAG_HIDDEN); lv_obj_remove_flag(icon_360[i], LV_OBJ_FLAG_HIDDEN); }
            else { lv_obj_remove_flag(icon_action[i], LV_OBJ_FLAG_HIDDEN); lv_obj_add_flag(icon_360[i], LV_OBJ_FLAG_HIDDEN); }
            if (current.cameras[i].asleep) lv_obj_remove_flag(tags[i], LV_OBJ_FLAG_HIDDEN);
            else lv_obj_add_flag(tags[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(rows[i], LV_OBJ_FLAG_HIDDEN);
        } else lv_obj_add_flag(rows[i], LV_OBJ_FLAG_HIDDEN);
    }
    /* Search stays tappable while scanning (restarts it); results are
     * selectable as they arrive. */
    if (started && (current.state == FOUND || current.state == FAILED || current.state == SCANNING))
        lv_obj_remove_state(scan_button, LV_STATE_DISABLED);
    else lv_obj_add_state(scan_button, LV_STATE_DISABLED);
    bool connected = awake || dozing || joining;
    set_control_visible(scan_button, scan_caption, !connected);
    set_control_visible(switch_button, switch_caption, connected);
    set_control_visible(shutter, shutter_hint, connected);
    set_control_visible(power_button, power_caption, connected);
    update_label(power_caption, dozing ? "Wake" : "Sleep");
    lv_obj_set_style_text_color(lv_obj_get_child(power_button, 0),
                                lv_color_hex(dozing ? 0x3EE07F : ROUND_BLUE), 0);
    set_control_visible(connect_button, connect_caption, current.state == DETACHED);
    if (current.state == READY || current.state == ASLEEP || current.state == DETACHED)
        lv_obj_remove_state(power_button, LV_STATE_DISABLED);
    else lv_obj_add_state(power_button, LV_STATE_DISABLED);
    refresh_camera(&current);
    if (current.state == READY || dozing) lv_obj_remove_state(switch_button, LV_STATE_DISABLED);
    else lv_obj_add_state(switch_button, LV_STATE_DISABLED);
    if (current.state == READY) {
        lv_obj_remove_state(shutter, LV_STATE_DISABLED);
    } else {
        lv_obj_add_state(shutter, LV_STATE_DISABLED);
    }
}

static void start_ble(void);

/* After the page closes and the camera link is down, stop NimBLE entirely. */
static void ble_off_task(void *arg)
{
    (void)arg;
    for (int i = 0; i < 40 && !close_done; ++i) vTaskDelay(pdMS_TO_TICKS(100));
    ble_npl_callout_stop(&poll);
    nimble_port_stop();
    ble_npl_callout_deinit(&poll);
    ble_npl_event_deinit(&kick);
    nimble_port_deinit();
    started = synced = false;
    xQueueReset(requests);
    state(IDLE, NULL);
    ESP_LOGI("inkorb_action", "Bluetooth off");
    stopping = false;
    if (page) start_ble();      /* reopened while we were shutting down */
    vTaskDelete(NULL);
}

static void close_page(void *unused)
{
    (void)unused;
    if (!page) return;
    post(CLOSE);
    if (started && !stopping) {
        stopping = true;
        close_done = false;
        if (xTaskCreate(ble_off_task, "ble_off", 4096, NULL, 3, NULL) != pdPASS) stopping = false;
    }
    lv_timer_delete(timer);
    lv_obj_delete(page);
    page = NULL;
    if (home->time_bloom) lv_obj_remove_flag(home->time_bloom, LV_OBJ_FLAG_HIDDEN);
}

static void back(lv_event_t *event)
{
    (void)event;
    lv_async_call(close_page, NULL);
}

void inkorb_action_open(inkorb_home_t *main_home)
{
    if (page) return;
    home = main_home;
    if (home->time_bloom) lv_obj_add_flag(home->time_bloom, LV_OBJ_FLAG_HIDDEN);
    page = round_page(back);
    round_title(page, "DJI Controller", back);
    device_name = round_text(page, ROUND_WHITE);
    lv_obj_set_width(device_name, LV_SIZE_CONTENT);
    lv_obj_set_style_max_width(device_name, 220, 0);
    lv_label_set_long_mode(device_name, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(device_name, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(device_name, LV_ALIGN_TOP_MID, 0, 74);
    battery_label = lv_label_create(page);
    lv_obj_set_style_text_font(battery_label, &lv_font_montserrat_16, 0);
    lv_label_set_text(battery_label, "");
    lv_obj_add_flag(battery_label, LV_OBJ_FLAG_HIDDEN);
    link_dot = lv_obj_create(page);
    lv_obj_remove_style_all(link_dot);
    lv_obj_set_size(link_dot, 10, 10);
    lv_obj_set_style_radius(link_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(link_dot, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_width(link_dot, 10, 0);
    lv_obj_set_style_shadow_opa(link_dot, LV_OPA_60, 0);
    lv_obj_remove_flag(link_dot, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(link_dot, LV_OBJ_FLAG_HIDDEN);
    status = round_text(page, ROUND_BLUE);
    lv_obj_set_style_text_align(status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(status, 290);
    lv_obj_align(status, LV_ALIGN_TOP_MID, 0, 124);
    rec_badge = lv_obj_create(page);
    lv_obj_remove_style_all(rec_badge);
    lv_obj_set_size(rec_badge, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(rec_badge, lv_color_hex(0xE5323E), 0);
    lv_obj_set_style_bg_opa(rec_badge, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(rec_badge, 22, 0);
    lv_obj_set_style_pad_hor(rec_badge, 20, 0);
    lv_obj_set_style_pad_ver(rec_badge, 6, 0);
    lv_obj_set_style_shadow_width(rec_badge, 20, 0);
    lv_obj_set_style_shadow_color(rec_badge, lv_color_hex(0xE5323E), 0);
    lv_obj_set_style_shadow_opa(rec_badge, LV_OPA_40, 0);
    lv_obj_remove_flag(rec_badge, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(rec_badge, LV_ALIGN_TOP_MID, 0, 116);
    lv_obj_add_flag(rec_badge, LV_OBJ_FLAG_HIDDEN);
    rec_label = lv_label_create(rec_badge);
    lv_obj_set_style_text_font(rec_label, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(rec_label, lv_color_hex(ROUND_WHITE), 0);
    lv_label_set_text(rec_label, "");
    lv_obj_center(rec_label);
    res_box = side_stat(page, -138, 0, "RES", &res_value);
    fps_box = side_stat(page, 138, 0, "FPS", &fps_value_label);
    spec_label = round_text(page, ROUND_MUTED);
    lv_obj_set_style_text_letter_space(spec_label, 1, 0);
    lv_obj_align(spec_label, LV_ALIGN_CENTER, 0, 82);
    lv_label_set_text(spec_label, "");
    for (int i = 0; i < MAX_CAMERAS; ++i) {
        /* Camera card: icon badge, name, and an "asleep" tag on the right. */
        rows[i] = lv_button_create(page);
        lv_obj_remove_style_all(rows[i]);
        lv_obj_set_size(rows[i], 288, 54);
        lv_obj_set_style_radius(rows[i], 27, 0);
        lv_obj_set_style_bg_opa(rows[i], LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(rows[i], lv_color_hex(0x18181C), 0);
        lv_obj_set_style_bg_color(rows[i], lv_color_hex(0x26221C), LV_STATE_PRESSED);
        lv_obj_set_style_border_color(rows[i], lv_color_hex(0x303036), 0);
        lv_obj_set_style_border_color(rows[i], lv_color_hex(0xFFB347), LV_STATE_PRESSED);
        lv_obj_set_style_border_width(rows[i], 1, 0);
        lv_obj_set_style_transform_scale(rows[i], 240, LV_STATE_PRESSED);
        lv_obj_set_style_transform_pivot_x(rows[i], 144, 0);
        lv_obj_set_style_transform_pivot_y(rows[i], 27, 0);
        lv_obj_set_style_pad_left(rows[i], 7, 0);
        lv_obj_set_style_pad_right(rows[i], 18, 0);
        lv_obj_add_flag(rows[i], LV_OBJ_FLAG_GESTURE_BUBBLE);
        lv_obj_align(rows[i], LV_ALIGN_CENTER, 0, -38 + i * 62);
        lv_obj_add_event_cb(rows[i], click, LV_EVENT_CLICKED, (void *)(intptr_t)(CONNECT_0 + i));
        /* Drawn action-cam glyph: body outline, big lens ring, record LED. */
        lv_obj_t *badge = lv_obj_create(rows[i]);
        lv_obj_remove_style_all(badge);
        lv_obj_set_size(badge, 40, 40);
        lv_obj_set_style_radius(badge, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(badge, lv_color_hex(0x2A2620), 0);
        lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, 0);
        lv_obj_remove_flag(badge, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_align(badge, LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_t *body = lv_obj_create(badge);
        lv_obj_remove_style_all(body);
        lv_obj_set_size(body, 24, 18);
        lv_obj_set_style_radius(body, 5, 0);
        lv_obj_set_style_border_width(body, 2, 0);
        lv_obj_set_style_border_color(body, lv_color_hex(0xFFB347), 0);
        lv_obj_remove_flag(body, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_center(body);
        icon_action[i] = body;
        lv_obj_t *lens = lv_obj_create(body);
        lv_obj_remove_style_all(lens);
        lv_obj_set_size(lens, 10, 10);
        lv_obj_set_style_radius(lens, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(lens, 2, 0);
        lv_obj_set_style_border_color(lens, lv_color_hex(ROUND_WHITE), 0);
        lv_obj_remove_flag(lens, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_align(lens, LV_ALIGN_RIGHT_MID, -2, 0);
        lv_obj_t *led = lv_obj_create(body);
        lv_obj_remove_style_all(led);
        lv_obj_set_size(led, 4, 4);
        lv_obj_set_style_radius(led, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(led, lv_color_hex(0xFF4D5A), 0);
        lv_obj_set_style_bg_opa(led, LV_OPA_COVER, 0);
        lv_obj_remove_flag(led, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_align(led, LV_ALIGN_TOP_LEFT, 3, 3);
        /* Osmo 360: tall stick with a domed lens near the top and a small
         * screen strip below it. */
        lv_obj_t *stick = lv_obj_create(badge);
        lv_obj_remove_style_all(stick);
        lv_obj_set_size(stick, 16, 28);
        lv_obj_set_style_radius(stick, 7, 0);
        lv_obj_set_style_border_width(stick, 2, 0);
        lv_obj_set_style_border_color(stick, lv_color_hex(0xFFB347), 0);
        lv_obj_remove_flag(stick, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_center(stick);
        lv_obj_t *dome = lv_obj_create(stick);
        lv_obj_remove_style_all(dome);
        lv_obj_set_size(dome, 10, 10);
        lv_obj_set_style_radius(dome, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(dome, lv_color_hex(ROUND_WHITE), 0);
        lv_obj_set_style_bg_opa(dome, LV_OPA_COVER, 0);
        lv_obj_remove_flag(dome, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_align(dome, LV_ALIGN_TOP_MID, 0, 2);
        lv_obj_t *strip = lv_obj_create(stick);
        lv_obj_remove_style_all(strip);
        lv_obj_set_size(strip, 6, 3);
        lv_obj_set_style_radius(strip, 1, 0);
        lv_obj_set_style_bg_color(strip, lv_color_hex(0xFFB347), 0);
        lv_obj_set_style_bg_opa(strip, LV_OPA_COVER, 0);
        lv_obj_remove_flag(strip, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_align(strip, LV_ALIGN_BOTTOM_MID, 0, -4);
        lv_obj_add_flag(stick, LV_OBJ_FLAG_HIDDEN);
        icon_360[i] = stick;
        names[i] = lv_label_create(rows[i]);
        lv_obj_set_style_text_font(names[i], &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(names[i], lv_color_hex(0xF2EEE8), 0);
        lv_obj_set_size(names[i], 178, 24);    /* fixed height: one line, "..." if long */
        lv_label_set_long_mode(names[i], LV_LABEL_LONG_DOT);
        lv_label_set_text(names[i], "");
        lv_obj_align(names[i], LV_ALIGN_LEFT_MID, 52, 0);
        tags[i] = lv_label_create(rows[i]);
        lv_obj_set_style_text_font(tags[i], &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(tags[i], lv_color_hex(0x9AA8B6), 0);
        lv_obj_set_style_bg_color(tags[i], lv_color_hex(0x2C2C32), 0);
        lv_obj_set_style_bg_opa(tags[i], LV_OPA_COVER, 0);
        lv_obj_set_style_radius(tags[i], 10, 0);
        lv_obj_set_style_pad_hor(tags[i], 8, 0);
        lv_obj_set_style_pad_ver(tags[i], 3, 0);
        lv_label_set_text(tags[i], "zZ");
        lv_obj_align(tags[i], LV_ALIGN_RIGHT_MID, 0, 0);
    }
    scan_button = round_button(page, LV_SYMBOL_REFRESH, "Search", 90, 148, 64, click, (void *)SCAN, &scan_caption);
    scan_arc = lv_arc_create(scan_button);
    lv_obj_remove_style_all(scan_arc);
    lv_obj_set_size(scan_arc, 56, 56);
    lv_obj_center(scan_arc);
    lv_obj_set_style_arc_width(scan_arc, 2, LV_PART_MAIN);
    lv_obj_set_style_arc_color(scan_arc, lv_color_hex(ROUND_NEON), LV_PART_MAIN);
    lv_obj_clear_flag(scan_arc, LV_OBJ_FLAG_CLICKABLE);
    connect_button = round_button(page, LV_SYMBOL_BLUETOOTH, "Connect", 90, 172, 50, click, (void *)CONNECT, &connect_caption);
    lv_obj_set_style_text_color(lv_obj_get_child(connect_button, 0), lv_color_hex(0x3EE07F), 0);
    power_button = round_button(page, LV_SYMBOL_POWER, "Sleep", 125, 168, 50, toggle_power, NULL, &power_caption);
    switch_button = round_button(page, LV_SYMBOL_REFRESH, "Switch", 55, 168, 50, click, (void *)SWITCH, &switch_caption);
    /* One shutter, camera-app style: red dot to record, shrinks to a
     * rounded square while recording. */
    shutter = lv_button_create(page);
    lv_obj_remove_style_all(shutter);
    lv_obj_set_size(shutter, 112, 112);
    lv_obj_set_style_radius(shutter, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(shutter, 4, 0);
    lv_obj_set_style_border_color(shutter, lv_color_hex(0xE8EEF4), 0);
    lv_obj_set_style_border_color(shutter, lv_color_hex(0xFF4D5A), LV_STATE_CHECKED);
    lv_obj_set_style_shadow_color(shutter, lv_color_hex(0xFF3B47), 0);
    lv_obj_set_style_shadow_width(shutter, 0, 0);
    lv_obj_set_style_shadow_width(shutter, 28, LV_STATE_CHECKED);
    lv_obj_set_style_shadow_opa(shutter, LV_OPA_50, 0);
    lv_obj_set_style_opa(shutter, LV_OPA_40, LV_STATE_DISABLED);
    lv_obj_set_style_transform_scale(shutter, 236, LV_STATE_PRESSED);
    lv_obj_set_style_transform_pivot_x(shutter, 56, 0);
    lv_obj_set_style_transform_pivot_y(shutter, 56, 0);
    lv_obj_set_ext_click_area(shutter, 14);
    lv_obj_add_flag(shutter, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_align(shutter, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_event_cb(shutter, toggle_record, LV_EVENT_CLICKED, NULL);
    shutter_dot = lv_obj_create(shutter);
    lv_obj_remove_style_all(shutter_dot);
    lv_obj_set_size(shutter_dot, 88, 88);
    lv_obj_set_style_radius(shutter_dot, 44, 0);
    lv_obj_set_style_size(shutter_dot, 40, 40, LV_STATE_CHECKED);
    lv_obj_set_style_radius(shutter_dot, 10, LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(shutter_dot, lv_color_hex(0xFF3B47), 0);
    lv_obj_set_style_bg_opa(shutter_dot, LV_OPA_COVER, 0);
    static const lv_style_prop_t dot_props[] = {LV_STYLE_WIDTH, LV_STYLE_HEIGHT, LV_STYLE_RADIUS, 0};
    static lv_style_transition_dsc_t dot_tr;
    lv_style_transition_dsc_init(&dot_tr, dot_props, lv_anim_path_ease_in_out, 220, 0, NULL);
    lv_obj_set_style_transition(shutter_dot, &dot_tr, 0);
    lv_obj_set_style_transition(shutter_dot, &dot_tr, LV_STATE_CHECKED);
    lv_obj_remove_flag(shutter_dot, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(shutter_dot);
    shutter_hint = round_text(page, ROUND_FAINT);
    lv_label_set_text(shutter_hint, "");
    lv_obj_add_flag(shutter_hint, LV_OBJ_FLAG_HIDDEN);
    heat_seen = 0;
    heat_card = lv_obj_create(page);
    lv_obj_remove_style_all(heat_card);
    lv_obj_set_size(heat_card, 280, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(heat_card, lv_color_hex(0x24130A), 0);
    lv_obj_set_style_bg_opa(heat_card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(heat_card, 26, 0);
    lv_obj_set_style_border_width(heat_card, 2, 0);
    lv_obj_set_style_pad_all(heat_card, 18, 0);
    lv_obj_set_style_shadow_width(heat_card, 40, 0);
    lv_obj_set_style_shadow_color(heat_card, lv_color_hex(0x000000), 0);
    lv_obj_set_style_shadow_opa(heat_card, LV_OPA_80, 0);
    lv_obj_set_flex_flow(heat_card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(heat_card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(heat_card, 6, 0);
    lv_obj_remove_flag(heat_card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(heat_card, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(heat_card, dismiss_heat, LV_EVENT_CLICKED, NULL);
    lv_obj_t *heat_icon = lv_label_create(heat_card);
    lv_obj_set_style_text_font(heat_icon, &lv_font_montserrat_36, 0);
    lv_obj_set_style_text_color(heat_icon, lv_color_hex(0xFFB45E), 0);
    lv_label_set_text(heat_icon, LV_SYMBOL_WARNING);
    heat_text = round_text(heat_card, ROUND_WHITE);
    lv_obj_set_style_text_align(heat_text, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(heat_text, "");
    lv_obj_t *heat_hint = round_text(heat_card, ROUND_FAINT);
    lv_label_set_text(heat_hint, "Tap to dismiss");
    lv_obj_center(heat_card);
    /* Give the radio to Bluetooth: idle Wi-Fi turns off now. */
    wifi_sta_stop_if_idle();
    if (!started && !stopping) start_ble();
    else if (started) post(OPEN);
    timer = lv_timer_create(refresh, 60, NULL);
    refresh(NULL);
}

void inkorb_action_init(void)
{
    requests = xQueueCreate(8, sizeof(int));
}

static void start_ble(void)
{
    state(IDLE, NULL);
    esp_err_t error = requests ? nimble_port_init() : ESP_ERR_NO_MEM;
    if (error != ESP_OK) {
        ESP_LOGE("inkorb_action", "BLE init: %s", esp_err_to_name(error));
        state(FAILED, "Bluetooth init failed");
        return;
    }
    started = true;
    closing = false;
    ESP_LOGI("inkorb_action", "Bluetooth on");
    ble_att_set_preferred_mtu(128);
    ble_hs_cfg.sync_cb = sync_host;
    ble_npl_callout_init(&poll, nimble_port_get_dflt_eventq(), poll_requests, NULL);
    ble_npl_event_init(&kick, run_kick, NULL);
    ble_npl_callout_reset(&poll, ble_npl_time_ms_to_ticks32(100));
    nimble_port_freertos_init(run_host);
}
