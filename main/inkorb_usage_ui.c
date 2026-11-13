#include "inkorb_usage_ui.h"
#include "inkorb_round.h"
#include "inkorb_usage.h"
#include "wifi_sta.h"
#include <stdio.h>
#include <string.h>
LV_FONT_DECLARE(lv_font_inkorb_cn18);

/* Usage as a round dashboard:
 *  - curved "DeepSeek 余额" along the top rim, the balance large in the centre
 *    (tap to refresh);
 *  - the bottom rim is four coloured arc gauges (conversations, DeepSeek
 *    tokens, speech recognition, speech synthesis). In "今日" mode each arc is
 *    today against the daily average (half full = an ordinary day); values
 *    sit just inside their arc;
 *  - a pill toggles 今日 / 累计. Swipe right (or tap the title) to go home. */

enum { M_TURNS, M_TOKENS, M_ASR, M_TTS, M_COUNT };

/* Gauge spans on the bottom rim, left to right, degrees clockwise from 3 o'clock. */
static const int s_arc_start[M_COUNT] = { 127, 92, 57, 22 };
#define ARC_SPAN 31
static const uint32_t s_color[M_COUNT] = { 0x62E69A, 0x4EB4FF, 0xB48CFF, 0xFFB45E };
static const char *s_name[M_COUNT] = { "对话", "DeepSeek", "识别", "合成" };

static inkorb_home_t *s_home;
static lv_obj_t *s_root, *s_amount, *s_unit, *s_split, *s_state, *s_toggle;
static lv_obj_t *s_arc[M_COUNT], *s_value[M_COUNT];
static lv_timer_t *s_timer;
static bool s_total_mode;

static void close_overlay(void)
{
    if (s_root) wifi_sta_release();
    if (s_timer) lv_timer_delete(s_timer);
    s_timer = NULL;
    if (s_root) lv_obj_delete(s_root);
    s_root = NULL;
    if (s_home && s_home->time_bloom) lv_obj_remove_flag(s_home->time_bloom, LV_OBJ_FLAG_HIDDEN);
}

static void close_later(void *unused) { (void)unused; close_overlay(); }
static void on_back(lv_event_t *event) { (void)event; lv_async_call(close_later, NULL); }

/* 12345 -> "1.2万"; keeps big counts short on a round screen. */
static void compact(char *out, size_t len, uint32_t n)
{
    if (n >= 100000000U) snprintf(out, len, "%.1f亿", n / 100000000.0);
    else if (n >= 10000U) snprintf(out, len, "%.1f万", n / 10000.0);
    else snprintf(out, len, "%u", (unsigned)n);
}

static uint32_t metric(const inkorb_usage_t *u, int m)
{
    switch (m) {
    case M_TURNS: return u->turns;
    case M_TOKENS: return u->llm_in + u->llm_out;
    case M_ASR: return u->asr_ms / 1000;
    default: return u->tts_chars;
    }
}

static void format_metric(char *out, size_t len, const inkorb_usage_t *u, int m)
{
    char n[16];
    uint32_t v = metric(u, m);
    if (m == M_ASR) {
        if (v >= 3600) snprintf(out, len, "%u时%u分", (unsigned)(v / 3600), (unsigned)(v / 60 % 60));
        else if (v >= 60) snprintf(out, len, "%u分%u秒", (unsigned)(v / 60), (unsigned)(v % 60));
        else snprintf(out, len, "%u秒", (unsigned)v);
        return;
    }
    compact(n, sizeof(n), v);
    snprintf(out, len, "%s%s", n, m == M_TURNS ? "轮" : m == M_TTS ? "字" : "");
}

static void refresh(void)
{
    inkorb_usage_t today, total, avg;
    inkorb_usage_get(&today, &total);
    bool has_avg = inkorb_usage_average(&avg);
    const inkorb_usage_t *shown = s_total_mode ? &total : &today;
    for (int m = 0; m < M_COUNT; m++) {
        char text[32];
        format_metric(text, sizeof(text), shown, m);
        lv_label_set_text_fmt(s_value[m], "%s\n%s", text, s_name[m]);
        /* Fill: all-time = full; today = today vs the daily average, where the
         * average lands at half. Without history, any use shows as half. */
        int fill;
        if (s_total_mode) fill = metric(&total, m) ? 100 : 0;
        else if (!metric(&today, m)) fill = 0;
        else if (!has_avg || !metric(&avg, m)) fill = 50;
        else {
            uint32_t ratio = metric(&today, m) * 50U / metric(&avg, m);
            fill = ratio > 100 ? 100 : (int)ratio;
        }
        lv_arc_set_value(s_arc[m], fill);
    }

    inkorb_balance_t b;
    inkorb_usage_balance(&b);
    if (b.valid) {
        lv_label_set_text(s_amount, b.total);
        lv_label_set_text(s_unit, strcmp(b.currency, "CNY") ? b.currency : "元");
        lv_label_set_text_fmt(s_split, "充值 %s    赠送 %s", b.topped, b.granted);
    }
    uint32_t age = inkorb_usage_now() - b.fetched_at;
    if (b.fetching) lv_label_set_text(s_state, "正在查询…");
    else if (b.error[0]) lv_label_set_text_fmt(s_state, "%s，点按重试", b.error);
    else if (b.valid && !b.available) lv_label_set_text(s_state, "余额不足，对话可能无法使用");
    else if (b.valid) {
        if (age < 60) lv_label_set_text(s_state, "刚刚更新");
        else lv_label_set_text_fmt(s_state, "%u 分钟前更新", (unsigned)(age / 60));
    } else lv_label_set_text(s_state, "点按查询余额");
    lv_obj_set_style_text_color(s_state, lv_color_hex(b.error[0] || (b.valid && !b.available) ? ROUND_WARN : ROUND_FAINT), 0);
}

static void on_refresh(lv_event_t *event)
{
    (void)event;
    inkorb_usage_refresh_balance();
    refresh();
}

static void on_toggle(lv_event_t *event)
{
    s_total_mode = lv_buttonmatrix_get_selected_button(lv_event_get_target(event)) == 1;
    refresh();
}

static bool s_fetched_online;

static void tick(lv_timer_t *timer)
{
    (void)timer;
    if (!s_root) return;
    /* The balance needs the on-demand Wi-Fi: fetch once it is up. */
    if (!s_fetched_online && wifi_sta_status(NULL, 0, NULL, NULL)) {
        s_fetched_online = true;
        inkorb_usage_refresh_balance();
    }
    refresh();
}

static const char *s_toggle_map[] = { "今日", "累计", "" };

static void build(void)
{
    round_title(s_root, "DeepSeek 余额", on_back);

    /* Balance in the centre of the upper half; the whole block refreshes. */
    lv_obj_t *zone = lv_obj_create(s_root);
    lv_obj_remove_style_all(zone);
    lv_obj_set_size(zone, 280, 150);
    lv_obj_align(zone, LV_ALIGN_CENTER, 0, -70);
    lv_obj_set_style_radius(zone, 40, 0);
    lv_obj_set_style_bg_color(zone, lv_color_hex(ROUND_CARD), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(zone, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_add_flag(zone, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_clear_flag(zone, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(zone, on_refresh, LV_EVENT_CLICKED, NULL);
    lv_obj_set_flex_flow(zone, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(zone, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(zone, 4, 0);
    lv_obj_t *line = lv_obj_create(zone);
    lv_obj_remove_style_all(line);
    lv_obj_set_size(line, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(line, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(line, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_column(line, 8, 0);
    lv_obj_clear_flag(line, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(line, LV_OBJ_FLAG_GESTURE_BUBBLE);
    s_amount = lv_label_create(line);
    lv_obj_set_style_text_font(s_amount, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(s_amount, lv_color_hex(ROUND_WHITE), 0);
    lv_label_set_text(s_amount, "--");
    s_unit = round_text(line, ROUND_BLUE);
    lv_obj_set_style_pad_bottom(s_unit, 8, 0);
    lv_label_set_text(s_unit, "");
    s_split = round_text(zone, ROUND_MUTED);
    lv_label_set_text(s_split, "");
    s_state = round_text(zone, ROUND_FAINT);

    /* 今日 / 累计 pill on the equator. */
    s_toggle = lv_buttonmatrix_create(s_root);
    lv_buttonmatrix_set_map(s_toggle, s_toggle_map);
    lv_buttonmatrix_set_button_ctrl_all(s_toggle, LV_BUTTONMATRIX_CTRL_CHECKABLE);
    lv_buttonmatrix_set_one_checked(s_toggle, true);
    lv_buttonmatrix_set_button_ctrl(s_toggle, s_total_mode ? 1 : 0, LV_BUTTONMATRIX_CTRL_CHECKED);
    lv_obj_set_size(s_toggle, 150, 38);
    lv_obj_align(s_toggle, LV_ALIGN_CENTER, 0, 34);
    lv_obj_set_style_bg_color(s_toggle, lv_color_hex(ROUND_CARD), 0);
    lv_obj_set_style_bg_opa(s_toggle, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_toggle, 19, 0);
    lv_obj_set_style_border_width(s_toggle, 0, 0);
    lv_obj_set_style_pad_all(s_toggle, 3, 0);
    lv_obj_set_style_pad_column(s_toggle, 3, 0);
    lv_obj_set_style_text_font(s_toggle, &lv_font_inkorb_cn18, LV_PART_ITEMS);
    lv_obj_set_style_text_color(s_toggle, lv_color_hex(ROUND_MUTED), LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(s_toggle, LV_OPA_TRANSP, LV_PART_ITEMS);
    lv_obj_set_style_radius(s_toggle, 16, LV_PART_ITEMS);
    lv_obj_set_style_border_width(s_toggle, 0, LV_PART_ITEMS);
    lv_obj_set_style_shadow_width(s_toggle, 0, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(s_toggle, lv_color_hex(ROUND_NEON), LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_bg_opa(s_toggle, LV_OPA_COVER, LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_text_color(s_toggle, lv_color_hex(0x050E1C), LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_add_flag(s_toggle, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(s_toggle, on_toggle, LV_EVENT_VALUE_CHANGED, NULL);

    /* Four gauges on the bottom rim; values just inside their arcs. */
    for (int m = 0; m < M_COUNT; m++) {
        s_arc[m] = round_rim_arc(s_root, s_arc_start[m], s_arc_start[m] + ARC_SPAN, 9, s_color[m], false, false);
        lv_obj_set_style_arc_color(s_arc[m], lv_color_mix(lv_color_hex(s_color[m]), lv_color_black(), 60), LV_PART_MAIN);
        s_value[m] = round_text(s_root, s_color[m]);
        lv_obj_set_style_text_align(s_value[m], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_line_space(s_value[m], 0, 0);
        lv_label_set_text(s_value[m], "--");
        round_place(s_value[m], s_arc_start[m] + ARC_SPAN / 2, 162);
    }
}

void inkorb_usage_ui_init(inkorb_home_t *home)
{
    s_home = home;
}

void inkorb_usage_ui_open(void)
{
    if (s_root) return;
    s_root = round_page(on_back);
    wifi_sta_hold();
    s_fetched_online = false;
    /* The breathing glow keeps invalidating under an opaque overlay. */
    if (s_home && s_home->time_bloom) lv_obj_add_flag(s_home->time_bloom, LV_OBJ_FLAG_HIDDEN);
    build();
    refresh();
    s_timer = lv_timer_create(tick, 1000, NULL);
}
