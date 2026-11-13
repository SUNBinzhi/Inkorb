#include "inkorb_wifi_ui.h"
#include "inkorb_portal.h"
#include "wifi_sta.h"
#include <stdio.h>
#include <string.h>
LV_FONT_DECLARE(lv_font_inkorb_cn18);

/* Same palette as the chat page. */
#define COL_WHITE   0xFFFFFF
#define COL_BLUE    0x7EC8FF
#define COL_NEON    0x4EB4FF
#define COL_GLOW    0x1E90FF
#define COL_DEEP    0x050E1C
#define COL_MUTED   0x89A0B4
#define COL_FAINT   0x5D7186
#define COL_BUBBLE  0x0B1D2E
#define COL_OK      0x62E69A
#define COL_WARN    0xFFB45E

static inkorb_home_t *s_home;
static lv_obj_t *s_root, *s_dot, *s_line1, *s_line2, *s_status, *s_armed;
static lv_timer_t *s_timer;
static bool s_setup, s_switching;
static uint32_t s_ok_at;
static char s_rows[WIFI_STA_MAX_SAVED][33];

static void show(bool setup);

static lv_obj_t *text(lv_obj_t *parent, uint32_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, &lv_font_inkorb_cn18, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_set_style_text_line_space(label, 6, 0);
    return label;
}

static void close_overlay(void)
{
    if (s_root) wifi_sta_release();
    if (inkorb_portal_running()) inkorb_portal_stop();
    if (s_timer) lv_timer_delete(s_timer);
    s_timer = NULL;
    if (s_root) lv_obj_delete(s_root);
    s_root = NULL;
    if (s_home && s_home->time_bloom) lv_obj_remove_flag(s_home->time_bloom, LV_OBJ_FLAG_HIDDEN);
}

/* Buttons rebuild or delete the view they live in, so defer past the event. */
static void show_later(void *setup) { if (s_root) show((bool)(intptr_t)setup); }
static void close_later(void *unused) { (void)unused; close_overlay(); }

static void on_back(lv_event_t *event)
{
    (void)event;
    if (s_setup) {
        inkorb_portal_stop();
        lv_async_call(show_later, (void *)(intptr_t)false);
    } else {
        lv_async_call(close_later, NULL);
    }
}

static void header(const char *title)
{
    lv_obj_t *button = lv_button_create(s_root);
    lv_obj_remove_style_all(button);
    lv_obj_set_size(button, 170, 40);
    lv_obj_align(button, LV_ALIGN_CENTER, 0, -176);
    lv_obj_set_style_radius(button, 20, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(COL_BUBBLE), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_add_event_cb(button, on_back, LV_EVENT_CLICKED, NULL);
    lv_obj_t *row = lv_obj_create(button);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 10, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(row);
    lv_obj_t *arrow = lv_label_create(row);
    lv_obj_set_style_text_font(arrow, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(arrow, lv_color_hex(COL_MUTED), 0);
    lv_label_set_text(arrow, LV_SYMBOL_LEFT);
    lv_obj_t *name = text(row, COL_MUTED);
    lv_label_set_text(name, title);
}

static lv_obj_t *capsule(const char *label, lv_event_cb_t cb, int32_t y)
{
    lv_obj_t *button = lv_button_create(s_root);
    lv_obj_remove_style_all(button);
    lv_obj_set_size(button, 236, 56);
    lv_obj_align(button, LV_ALIGN_CENTER, 0, y);
    lv_obj_set_style_radius(button, 28, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(COL_DEEP), 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(COL_BUBBLE), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(button, 2, 0);
    lv_obj_set_style_border_color(button, lv_color_hex(COL_NEON), 0);
    lv_obj_set_style_shadow_color(button, lv_color_hex(COL_GLOW), 0);
    lv_obj_set_style_shadow_width(button, 18, 0);
    lv_obj_set_style_shadow_spread(button, 1, 0);
    lv_obj_set_style_shadow_opa(button, LV_OPA_40, 0);
    lv_obj_add_event_cb(button, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *caption = text(button, COL_WHITE);
    lv_label_set_text(caption, label);
    lv_obj_center(caption);
    return button;
}

static const char *signal_word(int rssi)
{
    return rssi > -60 ? "信号强" : rssi > -72 ? "信号中" : "信号弱";
}

/* ---------- saved networks ---------- */

static void refresh_settings(void)
{
    char ssid[33], ip[16];
    int rssi = 0;
    if (s_switching) {
        const char *why = NULL;
        wifi_try_state_t st = wifi_sta_try_state(ssid, sizeof(ssid), &why);
        if (st == WIFI_TRY_CONNECTING) {
            lv_obj_set_style_bg_color(s_dot, lv_color_hex(COL_NEON), 0);
            lv_label_set_text_fmt(s_line1, "正在切换到 %s", ssid);
            lv_label_set_text(s_line2, "请稍候…");
            return;
        }
        s_switching = false;
        if (st == WIFI_TRY_FAILED) {
            lv_label_set_text_fmt(s_line2, "切换失败：%s", why ? why : "请重试");
            lv_obj_set_style_text_color(s_line2, lv_color_hex(COL_WARN), 0);
            return;
        }
        lv_async_call(show_later, (void *)(intptr_t)false); /* re-mark the network in use */
    }
    bool on = wifi_sta_status(ssid, sizeof(ssid), &rssi, ip);
    lv_obj_set_style_text_color(s_line2, lv_color_hex(COL_MUTED), 0);
    lv_obj_set_style_bg_color(s_dot, lv_color_hex(on ? COL_OK : COL_FAINT), 0);
    if (on) {
        lv_label_set_text_fmt(s_line1, "已连接 %s", ssid);
        lv_label_set_text_fmt(s_line2, "%s    %s", signal_word(rssi), ip[0] ? ip : "获取地址中");
    } else {
        lv_label_set_text(s_line1, "未连接");
        lv_label_set_text(s_line2, "正在寻找已保存的网络");
    }
}

static void on_forget(lv_event_t *event)
{
    lv_obj_t *button = lv_event_get_target(event);
    int index = (int)(intptr_t)lv_event_get_user_data(event);
    if (s_armed != button) {
        /* First tap arms, second tap forgets. */
        if (s_armed) lv_label_set_text(lv_obj_get_child(s_armed, 0), "忘记");
        s_armed = button;
        lv_label_set_text(lv_obj_get_child(button, 0), "确认忘记");
        return;
    }
    wifi_sta_forget(s_rows[index]);
    lv_async_call(show_later, (void *)(intptr_t)false);
}

static void on_use(lv_event_t *event)
{
    int index = (int)(intptr_t)lv_event_get_user_data(event);
    char current[33] = "";
    wifi_sta_status(current, sizeof(current), NULL, NULL);
    if (!strcmp(current, s_rows[index]) || s_switching) return;
    wifi_sta_use(s_rows[index]);
    s_switching = true;
    refresh_settings();
}

static void on_add(lv_event_t *event)
{
    (void)event;
    lv_async_call(show_later, (void *)(intptr_t)true);
}

static void build_settings(void)
{
    header("Wi-Fi");
    lv_obj_t *row = lv_obj_create(s_root);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 8, 0);
    lv_obj_align(row, LV_ALIGN_CENTER, 0, -128);
    s_dot = lv_obj_create(row);
    lv_obj_remove_style_all(s_dot);
    lv_obj_set_size(s_dot, 8, 8);
    lv_obj_set_style_radius(s_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(s_dot, LV_OPA_COVER, 0);
    s_line1 = text(row, COL_WHITE);
    lv_label_set_long_mode(s_line1, LV_LABEL_LONG_DOT);
    lv_obj_set_style_max_width(s_line1, 300, 0);
    s_line2 = text(s_root, COL_MUTED);
    lv_obj_align(s_line2, LV_ALIGN_CENTER, 0, -98);

    lv_obj_t *title = text(s_root, COL_FAINT);
    lv_label_set_text(title, "已保存的网络（点按切换）");
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -58);

    lv_obj_t *list = lv_obj_create(s_root);
    lv_obj_remove_style_all(list);
    lv_obj_set_size(list, 330, 150);
    lv_obj_align(list, LV_ALIGN_CENTER, 0, 30);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list, 6, 0);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);
    int count = wifi_sta_saved(s_rows, WIFI_STA_MAX_SAVED);
    char current[33] = "";
    wifi_sta_status(current, sizeof(current), NULL, NULL);
    for (int i = 0; i < count; i++) {
        lv_obj_t *item = lv_obj_create(list);
        lv_obj_remove_style_all(item);
        lv_obj_set_size(item, LV_PCT(100), 44);
        lv_obj_set_style_bg_color(item, lv_color_hex(COL_BUBBLE), 0);
        lv_obj_set_style_bg_opa(item, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(item, 14, 0);
        lv_obj_set_style_pad_hor(item, 16, 0);
        lv_obj_clear_flag(item, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(item, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_color(item, lv_color_hex(0x16324C), LV_STATE_PRESSED);
        /* Tap a saved network to switch; the one in use is outlined. */
        lv_obj_add_event_cb(item, on_use, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        if (!strcmp(current, s_rows[i])) {
            lv_obj_set_style_border_width(item, 2, 0);
            lv_obj_set_style_border_color(item, lv_color_hex(COL_NEON), 0);
        }
        lv_obj_t *name = text(item, COL_BLUE);
        lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
        lv_obj_set_width(name, 190);
        lv_label_set_text(name, s_rows[i]);
        lv_obj_align(name, LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_t *forget = lv_button_create(item);
        lv_obj_remove_style_all(forget);
        lv_obj_set_size(forget, LV_SIZE_CONTENT, 36);
        lv_obj_set_style_pad_hor(forget, 6, 0);
        lv_obj_align(forget, LV_ALIGN_RIGHT_MID, 0, 0);
        lv_obj_add_event_cb(forget, on_forget, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *caption = text(forget, COL_WARN);
        lv_label_set_text(caption, "忘记");
        lv_obj_center(caption);
    }
    if (!count) {
        lv_obj_t *empty = text(list, COL_FAINT);
        lv_obj_set_width(empty, LV_PCT(100));
        lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_pad_top(empty, 30, 0);
        lv_label_set_text(empty, "还没有保存的网络");
    }
    capsule("用手机添加网络", on_add, 150);
    refresh_settings();
}

/* ---------- phone setup ---------- */

static void set_status(const char *value, uint32_t color)
{
    if (strcmp(lv_label_get_text(s_status), value)) lv_label_set_text(s_status, value);
    lv_obj_set_style_text_color(s_status, lv_color_hex(color), 0);
}

static void refresh_setup(void)
{
    char ssid[33], line[120];
    const char *why = NULL;
    wifi_try_state_t st = wifi_sta_try_state(ssid, sizeof(ssid), &why);
    if (!inkorb_portal_running()) {
        set_status("热点启动失败，请返回后重试", COL_WARN);
    } else if (st == WIFI_TRY_CONNECTING) {
        snprintf(line, sizeof(line), "正在连接 %s …", ssid);
        set_status(line, COL_BLUE);
    } else if (st == WIFI_TRY_OK) {
        snprintf(line, sizeof(line), "已连接 %s\n已保存，即将返回", ssid);
        set_status(line, COL_OK);
        if (!s_ok_at) s_ok_at = lv_tick_get();
    } else if (st == WIFI_TRY_FAILED) {
        snprintf(line, sizeof(line), "连接失败：%s\n可在手机上重新选择", why ? why : "请重试");
        set_status(line, COL_WARN);
    } else if (wifi_sta_ap_clients() > 0) {
        set_status("手机已连接\n请在弹出的网页中选择 Wi-Fi", COL_BLUE);
    } else {
        set_status("用手机扫码连接热点\n网页会自动打开（或访问 192.168.4.1）", COL_MUTED);
    }
}

static void build_setup(void)
{
    header("添加网络");
    char ssid[33] = "", pass[16] = "", qr[80];
    bool up = inkorb_portal_start();
    inkorb_portal_credentials(ssid, sizeof(ssid), pass, sizeof(pass));

    lv_obj_t *frame = lv_obj_create(s_root);
    lv_obj_remove_style_all(frame);
    lv_obj_set_size(frame, 148, 148);
    lv_obj_align(frame, LV_ALIGN_CENTER, 0, -66);
    lv_obj_set_style_bg_color(frame, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(frame, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(frame, 12, 0);
    lv_obj_clear_flag(frame, LV_OBJ_FLAG_SCROLLABLE);
    if (up) {
        /* Standard Wi-Fi QR: phone cameras offer to join directly. */
        snprintf(qr, sizeof(qr), "WIFI:T:WPA;S:%s;P:%s;;", ssid, pass);
        lv_obj_t *code = lv_qrcode_create(frame);
        lv_qrcode_set_size(code, 128);
        lv_qrcode_set_dark_color(code, lv_color_black());
        lv_qrcode_set_light_color(code, lv_color_white());
        lv_qrcode_update(code, qr, strlen(qr));
        lv_obj_center(code);
        memset(qr, 0, sizeof(qr));
    }

    lv_obj_t *name = text(s_root, COL_WHITE);
    lv_label_set_text_fmt(name, "热点  %s", ssid);
    lv_obj_align(name, LV_ALIGN_CENTER, 0, 26);
    lv_obj_t *secret = text(s_root, COL_BLUE);
    lv_label_set_text_fmt(secret, "密码  %s", pass);
    lv_obj_align(secret, LV_ALIGN_CENTER, 0, 56);
    memset(pass, 0, sizeof(pass));

    s_status = text(s_root, COL_MUTED);
    lv_obj_set_width(s_status, 330);
    lv_obj_set_style_text_align(s_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_status, "");
    lv_obj_align(s_status, LV_ALIGN_CENTER, 0, 114);
    refresh_setup();
}

static void tick(lv_timer_t *timer)
{
    (void)timer;
    if (!s_root) return;
    if (!s_setup) { refresh_settings(); return; }
    refresh_setup();
    if (s_ok_at && lv_tick_elaps(s_ok_at) > 2500) close_overlay();
}

static void show(bool setup)
{
    lv_obj_clean(s_root);
    s_switching = false;
    s_armed = NULL;
    s_ok_at = 0;
    s_setup = setup;
    if (setup) build_setup();
    else build_settings();
}

void inkorb_wifi_ui_init(inkorb_home_t *home)
{
    s_home = home;
}

void inkorb_wifi_ui_open(bool setup)
{
    if (!s_root) {
        wifi_sta_hold();   /* scanning, switching and phone setup need the radio */
        s_root = lv_obj_create(lv_screen_active());
        lv_obj_remove_style_all(s_root);
        lv_obj_set_size(s_root, 466, 466);
        lv_obj_set_style_bg_color(s_root, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
        lv_obj_clear_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(s_root, INKORB_OVERLAY_FLAG);
        s_timer = lv_timer_create(tick, 500, NULL);
    }
    lv_obj_move_foreground(s_root);
    /* The breathing glow keeps invalidating under an opaque overlay. */
    if (s_home && s_home->time_bloom) lv_obj_add_flag(s_home->time_bloom, LV_OBJ_FLAG_HIDDEN);
    show(setup);
}
