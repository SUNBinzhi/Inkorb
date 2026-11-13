#include "inkorb_chat.h"
#include "inkorb_mic.h"
#include <string.h>
LV_FONT_DECLARE(lv_font_inkorb_cn18);

/* Same palette as the home capsule and boot orbit. */
#define COL_WHITE   0xFFFFFF
#define COL_BLUE    0x7EC8FF
#define COL_NEON    0x4EB4FF
#define COL_GLOW    0x1E90FF
#define COL_DEEP    0x050E1C
#define COL_TRACK   0x0E1B28
#define COL_MUTED   0x89A0B4
#define COL_FAINT   0x5D7186
#define COL_BUBBLE  0x0B1D2E
#define COL_WARN    0xFFB45E

#define RECORD_LIMIT_MS 12000
#define LISTEN_PROMPT   "正在听你说话…"
#define SOURCES_MARK    "\n\n联网来源：\n"
#define SOURCES_PREFIX  "来源\n"

enum { MODE_IDLE, MODE_LISTEN, MODE_THINK, MODE_SPEAK, MODE_ERROR };

static lv_obj_t *text(lv_obj_t *parent, uint32_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, &lv_font_inkorb_cn18, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_set_style_text_line_space(label, 6, 0);
    return label;
}

static lv_obj_t *box(lv_obj_t *parent)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return obj;
}

static bool has(const char *value, const char *word) { return strstr(value, word) != NULL; }

/* Voice/cloud code reports progress as text; map it to a visual state here so
 * the voice pipeline stays untouched. */
static int mode_for(const char *value)
{
    if (has(value, "失败") || has(value, "超时") || has(value, "没听清") || has(value, "不足") ||
        has(value, "忙") || has(value, "尚未") || has(value, "无法") || has(value, "请检查")) return MODE_ERROR;
    if (has(value, "正在录音")) return MODE_LISTEN;
    if (has(value, "正在回答")) return MODE_SPEAK;
    if (has(value, "正在")) return MODE_THINK;
    return MODE_IDLE;
}

static void ring_span(inkorb_chat_t *ui, int32_t start, int32_t length)
{
    start %= 360;
    if (start < 0) start += 360;
    lv_arc_set_angles(ui->ring, start, (start + length) % 360);
}

static void apply_mode(inkorb_chat_t *ui, int mode)
{
    static const uint32_t ring_color[] = { COL_NEON, COL_BLUE, COL_NEON, COL_BLUE, COL_WARN };
    static const uint32_t text_color[] = { COL_MUTED, COL_BLUE, COL_BLUE, COL_WHITE, COL_WARN };
    if (ui->mode == mode) return;
    ui->mode = mode;
    ui->mode_started = lv_tick_get();
    lv_obj_set_style_arc_color(ui->ring, lv_color_hex(ring_color[mode]), LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(ui->ring, lv_color_hex(mode == MODE_SPEAK ? 0x12344F : COL_TRACK), LV_PART_MAIN);
    lv_obj_set_style_text_color(ui->status, lv_color_hex(text_color[mode]), 0);
    lv_obj_set_style_bg_color(ui->dot, lv_color_hex(mode == MODE_IDLE ? COL_FAINT : ring_color[mode]), 0);
    if (mode == MODE_IDLE || mode == MODE_ERROR) lv_arc_set_angles(ui->ring, 0, 0);
    if (mode == MODE_ERROR) lv_obj_set_style_arc_color(ui->ring, lv_color_hex(0x4A3114), LV_PART_MAIN);

    lv_obj_t *button = ui->record_button;
    bool listen = mode == MODE_LISTEN, busy = mode == MODE_THINK || mode == MODE_SPEAK;
    lv_obj_set_style_bg_color(button, lv_color_hex(listen ? COL_NEON : COL_DEEP), 0);
    lv_obj_set_style_border_color(button, lv_color_hex(busy ? 0x1C3A55 : COL_NEON), 0);
    lv_obj_set_style_shadow_opa(button, busy ? LV_OPA_TRANSP : listen ? LV_OPA_70 : LV_OPA_40, 0);
    lv_obj_set_style_text_color(ui->button_label, lv_color_hex(listen ? COL_DEEP : busy ? COL_FAINT : COL_WHITE), 0);
    lv_label_set_text(ui->button_label, listen ? "松开发送" : busy ? "请稍候" : "按住说话");
    lv_obj_set_style_bg_color(ui->mic, lv_color_hex(busy ? COL_FAINT : COL_BLUE), 0);
    if (listen) {
        lv_obj_add_flag(ui->mic, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(ui->bars, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(ui->mic, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ui->bars, LV_OBJ_FLAG_HIDDEN);
    }
}

/* Only short arc spans and the small level meter move, so each frame
 * invalidates a few small areas instead of the whole round screen. */
static void animate(lv_timer_t *timer)
{
    inkorb_chat_t *ui = lv_timer_get_user_data(timer);
    if (!ui->root || lv_obj_has_flag(ui->root, LV_OBJ_FLAG_HIDDEN)) return;
    uint32_t elapsed = lv_tick_elaps(ui->mode_started);
    if (ui->mode == MODE_LISTEN) {
        uint32_t sweep = elapsed >= RECORD_LIMIT_MS ? 360 : elapsed * 360 / RECORD_LIMIT_MS;
        lv_arc_set_angles(ui->ring, 0, sweep ? sweep : 1);
        static const uint8_t weight[5] = { 45, 80, 100, 80, 45 };
        float level = inkorb_mic_level();
        for (int i = 0; i < 5; i++) {
            uint32_t wobble = (elapsed / 90 + i * 3) % 5;
            int32_t h = 6 + (int32_t)(level * 22.0f * weight[i] / 100.0f) + (level > 0.05f ? (int32_t)wobble : 0);
            if (h > 26) h = 26;
            lv_obj_set_height(ui->bar[i], h);
        }
    } else if (ui->mode == MODE_THINK) {
        ring_span(ui, (int32_t)(elapsed * 360 / 1400), 70);
    } else if (ui->mode == MODE_SPEAK) {
        ring_span(ui, (int32_t)(elapsed * 360 / 3200), 48);
    }
}

void inkorb_chat_create(inkorb_chat_t *ui, lv_event_cb_t press, lv_event_cb_t release,
                        lv_event_cb_t back)
{
    memset(ui, 0, sizeof(*ui));
    ui->mode = -1;
    ui->root = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(ui->root);
    lv_obj_set_size(ui->root, 466, 466);
    lv_obj_set_style_bg_color(ui->root, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(ui->root, LV_OPA_COVER, 0);
    lv_obj_clear_flag(ui->root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(ui->root, LV_OBJ_FLAG_USER_1); /* INKORB_OVERLAY_FLAG: home swipes ignore it */

    /* Rim ring: recording countdown, thinking orbit, speaking orbit, error. */
    ui->ring = lv_arc_create(ui->root);
    lv_obj_set_size(ui->ring, 458, 458);
    lv_obj_center(ui->ring);
    lv_arc_set_rotation(ui->ring, 270);
    lv_arc_set_bg_angles(ui->ring, 0, 360);
    lv_obj_remove_style(ui->ring, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(ui->ring, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_pad_all(ui->ring, 0, 0);
    lv_obj_set_style_arc_width(ui->ring, 3, LV_PART_MAIN);
    lv_obj_set_style_arc_width(ui->ring, 8, LV_PART_INDICATOR);

    /* Header doubles as the back button. */
    lv_obj_t *header = lv_button_create(ui->root);
    lv_obj_remove_style_all(header);
    lv_obj_set_size(header, 150, 40);
    lv_obj_align(header, LV_ALIGN_CENTER, 0, -176);
    lv_obj_set_style_radius(header, 20, 0);
    lv_obj_set_style_bg_color(header, lv_color_hex(COL_BUBBLE), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(header, LV_OPA_COVER, LV_STATE_PRESSED);
    if (back) lv_obj_add_event_cb(header, back, LV_EVENT_CLICKED, NULL);
    lv_obj_t *brand = lv_label_create(header);
    lv_obj_set_style_text_font(brand, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(brand, lv_color_hex(COL_MUTED), 0);
    lv_obj_set_style_text_letter_space(brand, 4, 0);
    lv_label_set_text(brand, LV_SYMBOL_LEFT "  INKORB");
    lv_obj_center(brand);

    lv_obj_t *row = box(ui->root);
    lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 8, 0);
    lv_obj_align(row, LV_ALIGN_CENTER, 0, -134);
    ui->dot = box(row);
    lv_obj_set_size(ui->dot, 8, 8);
    lv_obj_set_style_radius(ui->dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(ui->dot, LV_OPA_COVER, 0);
    ui->status = text(row, COL_MUTED);
    lv_label_set_long_mode(ui->status, LV_LABEL_LONG_DOT);
    lv_obj_set_style_max_width(ui->status, 300, 0);
    lv_label_set_text(ui->status, "按住大键或下方按钮说话");

    ui->body = lv_obj_create(ui->root);
    lv_obj_remove_style_all(ui->body);
    lv_obj_set_size(ui->body, 356, 212);
    lv_obj_align(ui->body, LV_ALIGN_CENTER, 0, -4);
    lv_obj_set_style_pad_hor(ui->body, 4, 0);
    lv_obj_set_style_pad_ver(ui->body, 6, 0);
    lv_obj_set_style_pad_row(ui->body, 14, 0);
    lv_obj_set_flex_flow(ui->body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(ui->body, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(ui->body, LV_SCROLLBAR_MODE_OFF);

    ui->hint = text(ui->body, COL_FAINT);
    lv_obj_set_width(ui->hint, LV_PCT(100));
    lv_obj_set_style_text_align(ui->hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_top(ui->hint, 46, 0);
    lv_label_set_text(ui->hint, "有什么想问的？\n按住说话，松开就发送");

    /* User turn: right-aligned bubble. */
    lv_obj_t *lane = box(ui->body);
    lv_obj_set_size(lane, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(lane, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(lane, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_add_flag(lane, LV_OBJ_FLAG_HIDDEN);
    ui->bubble = box(lane);
    lv_obj_set_size(ui->bubble, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(ui->bubble, lv_color_hex(COL_BUBBLE), 0);
    lv_obj_set_style_bg_opa(ui->bubble, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(ui->bubble, 18, 0);
    lv_obj_set_style_pad_hor(ui->bubble, 16, 0);
    lv_obj_set_style_pad_ver(ui->bubble, 9, 0);
    ui->question = text(ui->bubble, COL_BLUE);
    lv_label_set_long_mode(ui->question, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_max_width(ui->question, 260, 0);
    lv_obj_set_width(ui->question, LV_SIZE_CONTENT);
    lv_label_set_text(ui->question, "");

    /* Assistant turn: plain text, sources dimmed underneath. */
    ui->answer = text(ui->body, COL_WHITE);
    lv_obj_set_width(ui->answer, LV_PCT(100));
    lv_label_set_text(ui->answer, "");
    lv_obj_add_flag(ui->answer, LV_OBJ_FLAG_HIDDEN);
    ui->sources = text(ui->body, COL_FAINT);
    lv_obj_set_width(ui->sources, LV_PCT(100));
    lv_obj_set_style_border_side(ui->sources, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(ui->sources, 1, 0);
    lv_obj_set_style_border_color(ui->sources, lv_color_hex(0x1A2A3A), 0);
    lv_obj_set_style_pad_top(ui->sources, 10, 0);
    lv_label_set_text(ui->sources, "");
    lv_obj_add_flag(ui->sources, LV_OBJ_FLAG_HIDDEN);

    /* Talk button in the home-capsule style. */
    ui->record_button = lv_button_create(ui->root);
    lv_obj_remove_style_all(ui->record_button);
    lv_obj_set_size(ui->record_button, 200, 56);
    lv_obj_align(ui->record_button, LV_ALIGN_CENTER, 0, 150);
    lv_obj_set_style_radius(ui->record_button, 28, 0);
    lv_obj_set_style_bg_opa(ui->record_button, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(ui->record_button, 2, 0);
    lv_obj_set_style_shadow_color(ui->record_button, lv_color_hex(COL_GLOW), 0);
    lv_obj_set_style_shadow_width(ui->record_button, 18, 0);
    lv_obj_set_style_shadow_spread(ui->record_button, 1, 0);
    lv_obj_set_flex_flow(ui->record_button, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ui->record_button, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(ui->record_button, 12, 0);
    if (press) lv_obj_add_event_cb(ui->record_button, press, LV_EVENT_PRESSED, NULL);
    if (release) {
        lv_obj_add_event_cb(ui->record_button, release, LV_EVENT_RELEASED, NULL);
        lv_obj_add_event_cb(ui->record_button, release, LV_EVENT_PRESS_LOST, NULL);
    }
    /* Microphone glyph built from shapes (the CJK font has no icon). */
    ui->mic = box(ui->record_button);
    lv_obj_set_size(ui->mic, 12, 20);
    lv_obj_set_style_radius(ui->mic, 6, 0);
    lv_obj_set_style_bg_opa(ui->mic, LV_OPA_COVER, 0);
    ui->bars = box(ui->record_button);
    lv_obj_set_size(ui->bars, 44, 26);
    lv_obj_set_flex_flow(ui->bars, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ui->bars, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(ui->bars, 4, 0);
    for (int i = 0; i < 5; i++) {
        ui->bar[i] = box(ui->bars);
        lv_obj_set_size(ui->bar[i], 5, 6);
        lv_obj_set_style_radius(ui->bar[i], 3, 0);
        lv_obj_set_style_bg_color(ui->bar[i], lv_color_hex(COL_DEEP), 0);
        lv_obj_set_style_bg_opa(ui->bar[i], LV_OPA_COVER, 0);
    }
    ui->button_label = text(ui->record_button, COL_WHITE);

    apply_mode(ui, MODE_IDLE);
    ui->timer = lv_timer_create(animate, 40, ui);
}

void inkorb_chat_status(inkorb_chat_t *ui, const char *value)
{
    if (!ui->root) return;
    /* The CJK subset font has no U+00B7, so "思考 · 4秒" becomes "思考  4秒". */
    char shown[160];
    size_t n = 0;
    for (const char *c = value; *c && n < sizeof(shown) - 1; c++) {
        if ((unsigned char)c[0] == 0xC2 && (unsigned char)c[1] == 0xB7) { c++; continue; }
        shown[n++] = *c;
    }
    shown[n] = '\0';
    if (strcmp(lv_label_get_text(ui->status), shown) != 0) lv_label_set_text(ui->status, shown);
    apply_mode(ui, mode_for(value));
}

static void follow_bottom(inkorb_chat_t *ui, bool force)
{
    if (!force && lv_obj_get_scroll_bottom(ui->body) > 60) return;
    lv_obj_update_layout(ui->body);
    lv_obj_scroll_to_y(ui->body, LV_COORD_MAX, LV_ANIM_OFF);
}

void inkorb_chat_text(inkorb_chat_t *ui, const char *question, const char *answer)
{
    bool follow = false, force = false;
    if (question && strcmp(lv_label_get_text(ui->question), question) != 0) {
        lv_label_set_text(ui->question, question);
        lv_obj_set_style_text_color(ui->question,
                                    lv_color_hex(strcmp(question, LISTEN_PROMPT) ? COL_BLUE : COL_FAINT), 0);
        lv_obj_add_flag(ui->hint, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(lv_obj_get_parent(ui->bubble), LV_OBJ_FLAG_HIDDEN);
        follow = force = true;
    }
    if (answer) {
        const char *mark = strstr(answer, SOURCES_MARK);
        size_t length = mark ? (size_t)(mark - answer) : strlen(answer);
        const char *current = lv_label_get_text(ui->answer);
        if (strlen(current) != length || strncmp(current, answer, length) != 0) {
            lv_label_set_text_fmt(ui->answer, "%.*s", (int)length, answer);
            follow = true;
        }
        if (length) lv_obj_remove_flag(ui->answer, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(ui->answer, LV_OBJ_FLAG_HIDDEN);
        const char *sources = mark ? mark + strlen(SOURCES_MARK) : "";
        const char *shown = lv_label_get_text(ui->sources);
        size_t prefix = strlen(SOURCES_PREFIX);
        bool same = mark ? strncmp(shown, SOURCES_PREFIX, prefix) == 0 && strcmp(shown + prefix, sources) == 0
                         : shown[0] == '\0';
        if (!same) {
            if (mark) lv_label_set_text_fmt(ui->sources, "%s%s", SOURCES_PREFIX, sources);
            else lv_label_set_text(ui->sources, "");
            follow = true;
        }
        if (mark) lv_obj_remove_flag(ui->sources, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(ui->sources, LV_OBJ_FLAG_HIDDEN);
    }
    if (follow) follow_bottom(ui, force);
}
