#include "inkorb_settings_ui.h"
#include "inkorb_cloud.h"
#include "inkorb_device.h"
#include "inkorb_round.h"
#include "inkorb_wifi_ui.h"
#include "wifi_sta.h"
#include <stdio.h>
#include <string.h>

/* Settings as a dial for the round glass:
 *  - volume and brightness are arcs along the left and right rim; drag along
 *    the edge, the centre shows the value while dragging;
 *  - Wi-Fi, auto screen-off, memory and about sit as round buttons at the
 *    compass points; restart is a small red power mark on the bottom rim.
 * Sub-pages keep the curved rim title; tap it ("‹") to go back. Settings has
 * no swipe-back: the rim sliders are dragged along the edge. */

enum { PAGE_MAIN, PAGE_MEMORY, PAGE_ABOUT, PAGE_SCREEN };

/* Rim slider spans, degrees clockwise from 3 o'clock. Value grows upward. */
#define VOL_START   112
#define VOL_END     248
#define BRI_START   292
#define BRI_END      68

static inkorb_home_t *s_home;
static lv_obj_t *s_root, *s_readout_value, *s_readout_name, *s_idle, *s_wifi_caption;
static lv_obj_t *s_about, *s_memory_text, *s_armed, *s_screen_desc;
static lv_obj_t *s_screen_opts[4];
static lv_timer_t *s_timer;
static int s_page;

static void show(int page);

static void close_overlay(void)
{
    if (s_timer) lv_timer_delete(s_timer);
    s_timer = NULL;
    if (s_root) lv_obj_delete(s_root);
    s_root = NULL;
    if (s_home && s_home->time_bloom) lv_obj_remove_flag(s_home->time_bloom, LV_OBJ_FLAG_HIDDEN);
}

/* Buttons rebuild or delete the view they live in, so defer past the event. */
static void show_later(void *page) { if (s_root) show((int)(intptr_t)page); }
static void close_later(void *unused) { (void)unused; close_overlay(); }

static void on_back(lv_event_t *event)
{
    (void)event;
    if (s_page == PAGE_MAIN) lv_async_call(close_later, NULL);
    else lv_async_call(show_later, (void *)(intptr_t)PAGE_MAIN);
}

/* ---------- main dial ---------- */

static void refresh_idle(void)
{
    lv_label_set_text_fmt(s_idle, "音量 %d\n亮度 %d", inkorb_device_volume(), inkorb_device_brightness());
    lv_obj_set_style_text_color(s_idle, lv_color_hex(ROUND_FAINT), 0);
}

static void rim_event(lv_event_t *event)
{
    lv_obj_t *arc = lv_event_get_target(event);
    bool volume = (bool)(intptr_t)lv_event_get_user_data(event);
    int level = lv_arc_get_value(arc);
    if (!volume && level < 5) level = 5;
    if (lv_event_get_code(event) == LV_EVENT_VALUE_CHANGED) {
        /* Live while dragging: big value in the centre, saved on release. */
        if (volume) inkorb_device_preview_volume(level);
        else inkorb_device_preview_brightness(level);
        lv_label_set_text_fmt(s_readout_value, "%d", level);
        lv_label_set_text(s_readout_name, volume ? "音量" : "亮度");
        lv_obj_remove_flag(s_readout_value, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_readout_name, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_idle, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    if (volume) inkorb_device_set_volume(level);
    else inkorb_device_set_brightness(level);
    lv_obj_add_flag(s_readout_value, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_readout_name, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_idle, LV_OBJ_FLAG_HIDDEN);
    refresh_idle();
}

static void rim_slider(int32_t start, int32_t end, bool reverse, int value, bool volume)
{
    lv_obj_t *arc = round_rim_arc(s_root, start, end, 14, ROUND_NEON, true, reverse);
    lv_arc_set_value(arc, value);
    lv_obj_add_event_cb(arc, rim_event, LV_EVENT_VALUE_CHANGED, (void *)(intptr_t)volume);
    lv_obj_add_event_cb(arc, rim_event, LV_EVENT_RELEASED, (void *)(intptr_t)volume);
    lv_obj_add_event_cb(arc, rim_event, LV_EVENT_PRESS_LOST, (void *)(intptr_t)volume);
}

/* Wi-Fi has its own overlay; settings closes first so back returns home. */
static void wifi_later(void *unused)
{
    (void)unused;
    close_overlay();
    inkorb_wifi_ui_open(!wifi_sta_status(NULL, 0, NULL, NULL));
}

static void on_wifi(lv_event_t *event) { (void)event; lv_async_call(wifi_later, NULL); }
static void on_page(lv_event_t *event) { lv_async_call(show_later, lv_event_get_user_data(event)); }
static void restart_later(void *unused) { (void)unused; inkorb_device_restart(); }

static void on_restart(lv_event_t *event)
{
    lv_obj_t *b = lv_event_get_current_target(event);
    if (s_armed == b) { lv_async_call(restart_later, NULL); return; }
    /* First tap arms: the mark turns solid red and the centre explains. */
    s_armed = b;
    lv_obj_set_style_bg_color(b, lv_color_hex(0x5A1F1F), 0);
    lv_label_set_text(s_idle, "再点一次\n立即重启");
    lv_obj_set_style_text_color(s_idle, lv_color_hex(ROUND_WARN), 0);
}

static const char *off_text(int seconds)
{
    switch (seconds) {
    case 0: return "熄屏 从不";
    case 60: return "熄屏 1分";
    case 300: return "熄屏 5分";
    default: return "熄屏 15分";
    }
}

static void refresh_wifi_caption(void)
{
    char ssid[33];
    if (wifi_sta_status(ssid, sizeof(ssid), NULL, NULL)) lv_label_set_text(s_wifi_caption, ssid);
    else lv_label_set_text(s_wifi_caption, "未连接");
}

static void build_main(void)
{
    round_title(s_root, "设 置", on_back);

    /* Rim sliders: left = volume, right = brightness, both grow upward. */
    rim_slider(VOL_START, VOL_END, false, inkorb_device_volume(), true);
    rim_slider(BRI_START, BRI_END, true, inkorb_device_brightness(), false);
    lv_obj_t *speaker = lv_label_create(s_root);
    lv_obj_set_style_text_font(speaker, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(speaker, lv_color_hex(ROUND_MUTED), 0);
    lv_label_set_text(speaker, LV_SYMBOL_VOLUME_MAX);
    round_place(speaker, 180, 186);
    lv_obj_t *sun = round_sun_icon(s_root, ROUND_MUTED);
    round_place(sun, 0, 186);

    /* Compass of round buttons. */
    lv_obj_t *wifi = round_button(s_root, LV_SYMBOL_WIFI, NULL, 270, 112, 62, on_wifi, NULL, NULL);
    s_wifi_caption = round_text(s_root, ROUND_MUTED);
    lv_label_set_long_mode(s_wifi_caption, LV_LABEL_LONG_DOT);
    lv_obj_set_style_max_width(s_wifi_caption, 120, 0);
    refresh_wifi_caption();
    lv_obj_align_to(s_wifi_caption, wifi, LV_ALIGN_OUT_BOTTOM_MID, 0, 4);
    round_button(s_root, LV_SYMBOL_EYE_CLOSE, off_text(inkorb_device_auto_off()), 180, 112, 62, on_page,
                 (void *)(intptr_t)PAGE_SCREEN, NULL);
    round_button(s_root, LV_SYMBOL_EDIT, "记忆", 0, 112, 62, on_page, (void *)(intptr_t)PAGE_MEMORY, NULL);
    round_button(s_root, "i", "关于", 90, 112, 62, on_page, (void *)(intptr_t)PAGE_ABOUT, NULL);

    /* Centre: idle summary, or the big value while a rim slider moves. */
    s_idle = round_text(s_root, ROUND_FAINT);
    lv_obj_set_style_text_align(s_idle, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(s_idle);
    refresh_idle();
    s_readout_value = lv_label_create(s_root);
    lv_obj_set_style_text_font(s_readout_value, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(s_readout_value, lv_color_hex(ROUND_WHITE), 0);
    lv_obj_align(s_readout_value, LV_ALIGN_CENTER, 0, 8);
    lv_obj_add_flag(s_readout_value, LV_OBJ_FLAG_HIDDEN);
    s_readout_name = round_text(s_root, ROUND_BLUE);
    lv_obj_align(s_readout_name, LV_ALIGN_CENTER, 0, -30);
    lv_obj_add_flag(s_readout_name, LV_OBJ_FLAG_HIDDEN);

    /* Restart: a small red power mark tucked into the bottom rim. */
    lv_obj_t *power = lv_button_create(s_root);
    lv_obj_remove_style_all(power);
    lv_obj_set_size(power, 40, 40);
    lv_obj_set_style_radius(power, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(power, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(power, lv_color_black(), 0);
    lv_obj_set_ext_click_area(power, 10);
    lv_obj_add_flag(power, LV_OBJ_FLAG_GESTURE_BUBBLE);
    round_place(power, 90, 196);
    lv_obj_add_event_cb(power, on_restart, LV_EVENT_CLICKED, NULL);
    lv_obj_t *mark = lv_label_create(power);
    lv_obj_set_style_text_font(mark, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(mark, lv_color_hex(0xE0605A), 0);
    lv_label_set_text(mark, LV_SYMBOL_POWER);
    lv_obj_center(mark);
}

/* ---------- auto screen-off: four choices on a dial ---------- */

static const int s_off_seconds[4] = { 0, 60, 300, 900 };
static const char *s_off_label[4] = { "从不", "1分", "5分", "15分" };
static const int s_off_angle[4] = { 270, 0, 90, 180 };

static void paint_screen_opts(void)
{
    int now = inkorb_device_auto_off();
    for (int i = 0; i < 4; i++) {
        bool on = s_off_seconds[i] == now;
        lv_obj_set_style_bg_color(s_screen_opts[i], lv_color_hex(on ? ROUND_NEON : ROUND_CARD), 0);
        lv_obj_set_style_text_color(lv_obj_get_child(s_screen_opts[i], 0), lv_color_hex(on ? 0x050E1C : ROUND_MUTED), 0);
    }
    if (now) lv_label_set_text_fmt(s_screen_desc, "%d 分钟无操作\n自动熄屏", now / 60);
    else lv_label_set_text(s_screen_desc, "屏幕常亮\n不自动熄屏");
}

static void on_screen_opt(lv_event_t *event)
{
    inkorb_device_set_auto_off(s_off_seconds[(int)(intptr_t)lv_event_get_user_data(event)]);
    paint_screen_opts();
}

static void build_screen(void)
{
    round_title(s_root, "自动熄屏", on_back);
    round_rim_arc(s_root, 0, 360, 2, ROUND_TRACK, false, false);
    for (int i = 0; i < 4; i++) {
        lv_obj_t *b = round_button(s_root, NULL, NULL, s_off_angle[i], 128, 78, on_screen_opt, (void *)(intptr_t)i, NULL);
        lv_obj_t *t = round_text(b, ROUND_MUTED);
        lv_label_set_text(t, s_off_label[i]);
        lv_obj_center(t);
        s_screen_opts[i] = b;
    }
    s_screen_desc = round_text(s_root, ROUND_BLUE);
    lv_obj_set_style_text_align(s_screen_desc, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(s_screen_desc);
    paint_screen_opts();
}

/* ---------- memory ---------- */

static void refresh_memory(void)
{
    char memory[1300];
    inkorb_cloud_memory_text(memory, sizeof(memory));
    lv_label_set_text(s_memory_text, memory[0] ? memory : "还没有记住关于你的事。\n和 Inkorb 聊聊你的名字、喜好，\n或者直接说“记住……”。");
    lv_obj_set_style_text_color(s_memory_text, lv_color_hex(memory[0] ? ROUND_BLUE : ROUND_FAINT), 0);
}

static void on_forget(lv_event_t *event)
{
    lv_obj_t *b = lv_event_get_current_target(event);
    bool long_term = (bool)(intptr_t)lv_event_get_user_data(event);
    if (s_armed != b) {
        s_armed = b;
        lv_obj_set_style_bg_color(b, lv_color_hex(0x5A1F1F), 0);
        lv_label_set_text(s_memory_text, long_term ? "再点一次\n清空长期记忆" : "再点一次\n清空最近对话");
        lv_obj_set_style_text_color(s_memory_text, lv_color_hex(ROUND_WARN), 0);
        return;
    }
    inkorb_cloud_forget(long_term, !long_term);
    s_armed = NULL;
    lv_obj_set_style_bg_color(b, lv_color_hex(ROUND_CARD), 0);
    refresh_memory();
}

/* Centre column whose top and bottom fade into the black glass. */
static lv_obj_t *round_column(int32_t width, int32_t height, int32_t y)
{
    lv_obj_t *col = lv_obj_create(s_root);
    lv_obj_remove_style_all(col);
    lv_obj_set_size(col, width, height);
    lv_obj_align(col, LV_ALIGN_CENTER, 0, y);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_ver(col, 20, 0);
    lv_obj_set_style_pad_row(col, 8, 0);
    lv_obj_set_scroll_dir(col, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(col, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(col, LV_OBJ_FLAG_GESTURE_BUBBLE);
    for (int edge = 0; edge < 2; edge++) {
        lv_obj_t *fade = lv_obj_create(s_root);
        lv_obj_remove_style_all(fade);
        lv_obj_set_size(fade, width + 10, 26);
        lv_obj_align(fade, LV_ALIGN_CENTER, 0, y + (edge ? height / 2 - 13 : -height / 2 + 13));
        lv_obj_set_style_bg_color(fade, lv_color_black(), 0);
        lv_obj_set_style_bg_grad_color(fade, lv_color_black(), 0);
        lv_obj_set_style_bg_main_opa(fade, edge ? LV_OPA_TRANSP : LV_OPA_COVER, 0);
        lv_obj_set_style_bg_grad_opa(fade, edge ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_bg_grad_dir(fade, LV_GRAD_DIR_VER, 0);
        lv_obj_set_style_bg_opa(fade, LV_OPA_COVER, 0);
        lv_obj_clear_flag(fade, LV_OBJ_FLAG_CLICKABLE);
    }
    return col;
}

static void build_memory(void)
{
    round_title(s_root, "记 忆", on_back);
    lv_obj_t *col = round_column(300, 230, -30);
    s_memory_text = round_text(col, ROUND_BLUE);
    lv_obj_set_width(s_memory_text, LV_PCT(100));
    lv_obj_set_style_text_align(s_memory_text, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_memory_text, LV_LABEL_LONG_WRAP);
    refresh_memory();
    round_button(s_root, LV_SYMBOL_TRASH, "清空记忆", 118, 150, 56, on_forget, (void *)(intptr_t)true, NULL);
    round_button(s_root, LV_SYMBOL_REFRESH, "清空对话", 62, 150, 56, on_forget, (void *)(intptr_t)false, NULL);
}

/* ---------- about ---------- */

static void refresh_about(void)
{
    char info[512];
    inkorb_device_info(info, sizeof(info));
    lv_label_set_text(s_about, info);
}

static void build_about(void)
{
    round_title(s_root, "关于本机", on_back);
    lv_obj_t *name = lv_label_create(s_root);
    lv_obj_set_style_text_font(name, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(name, lv_color_hex(ROUND_WHITE), 0);
    lv_obj_set_style_text_letter_space(name, 6, 0);
    lv_label_set_text(name, "INKORB");
    lv_obj_align(name, LV_ALIGN_CENTER, 0, -128);
    lv_obj_t *col = round_column(310, 250, 30);
    s_about = round_text(col, ROUND_BLUE);
    lv_obj_set_width(s_about, LV_PCT(100));
    lv_obj_set_style_text_align(s_about, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_about, LV_LABEL_LONG_WRAP);
    refresh_about();
}

static void tick(lv_timer_t *timer)
{
    (void)timer;
    if (!s_root) return;
    if (s_page == PAGE_MAIN && s_wifi_caption) refresh_wifi_caption();
    else if (s_page == PAGE_ABOUT && s_about) refresh_about();
}

static void show(int page)
{
    lv_obj_clean(s_root);
    s_readout_value = s_readout_name = s_idle = s_wifi_caption = NULL;
    s_about = s_memory_text = s_armed = s_screen_desc = NULL;
    s_page = page;
    if (page == PAGE_MEMORY) build_memory();
    else if (page == PAGE_ABOUT) build_about();
    else if (page == PAGE_SCREEN) build_screen();
    else build_main();
}

void inkorb_settings_ui_init(inkorb_home_t *home)
{
    s_home = home;
}

void inkorb_settings_ui_open(void)
{
    inkorb_settings_ui_open_page(PAGE_MAIN);
}

void inkorb_settings_ui_open_page(int page)
{
    if (!s_root) {
        s_root = round_page(NULL); /* settings exits by tapping the title, not swiping */
        s_timer = lv_timer_create(tick, 1000, NULL);
    }
    lv_obj_move_foreground(s_root);
    /* The breathing glow keeps invalidating under an opaque overlay. */
    if (s_home && s_home->time_bloom) lv_obj_add_flag(s_home->time_bloom, LV_OBJ_FLAG_HIDDEN);
    show(page);
}
