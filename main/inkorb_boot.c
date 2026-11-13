#include "inkorb_boot.h"
#include <math.h>
#include <string.h>

#define RING_SIZE     458   /* physical 466px rim with a 4px safety inset */
#define ORBIT_MS      1700
#define HEAD_DEG      14
#define TAIL_SEGS     15    /* fine steps so the fade reads as continuous */
#define SEG_DEG       5
#define TAIL_DEG      (TAIL_SEGS * SEG_DEG)
_Static_assert(TAIL_SEGS <= sizeof(((inkorb_boot_t *)0)->comet_seg) / sizeof(lv_area_t), "comet_seg too small");
#define COL_TRACK     0x0E1B28
#define COL_PROGRESS  0x24557E
#define COL_HEAD      0xBFE6FF
#define COL_TAIL      0x4EB4FF
#define COL_READY     0x62E69A

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, uint32_t color)
{
    lv_obj_t *obj = lv_label_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_style_text_font(obj, font, 0);
    lv_obj_set_style_text_color(obj, lv_color_hex(color), 0);
    return obj;
}

static lv_obj_t *ring(lv_obj_t *parent, int32_t width, uint32_t color, lv_opa_t opa, bool rounded)
{
    lv_obj_t *arc = lv_arc_create(parent);
    lv_obj_set_size(arc, RING_SIZE, RING_SIZE);
    lv_obj_center(arc);
    lv_obj_set_style_pad_all(arc, 0, 0);
    lv_arc_set_rotation(arc, 270);
    lv_arc_set_bg_angles(arc, 0, 0);
    lv_arc_set_angles(arc, 0, 0);
    lv_obj_remove_style(arc, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(arc, 0, LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, width, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc, lv_color_hex(color), LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(arc, opa, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(arc, rounded, LV_PART_INDICATOR);
    return arc;
}

/* Comet slows near the top and speeds through the bottom, like an orbit. */
static int32_t comet_angle(uint32_t ms)
{
    float p = (float)(ms % ORBIT_MS) / ORBIT_MS;
    return (int32_t)(360.0f * (p - 0.30f * sinf(6.2831853f * p) / 6.2831853f));
}

/* Bounding box of a rim arc between two clockwise-from-top angles. */
static void arc_area(const lv_obj_t *obj, int32_t from, int32_t to, int32_t width, lv_area_t *out)
{
    lv_area_t coords;
    lv_obj_get_coords(obj, &coords);
    float cx = coords.x1 + RING_SIZE / 2.0f, cy = coords.y1 + RING_SIZE / 2.0f;
    float r_out = RING_SIZE / 2.0f, r_in = r_out - width;
    out->x1 = out->y1 = INT16_MAX;
    out->x2 = out->y2 = INT16_MIN;
    for (int32_t a = from;; a += 5) {
        if (a > to) a = to;
        float s = sinf(a * 0.0174533f), c = cosf(a * 0.0174533f);
        for (int k = 0; k < 2; k++) {
            float r = k ? r_in : r_out;
            int32_t x = (int32_t)(cx + r * s), y = (int32_t)(cy - r * c);
            if (x < out->x1) out->x1 = x;
            if (x > out->x2) out->x2 = x;
            if (y < out->y1) out->y1 = y;
            if (y > out->y2) out->y2 = y;
        }
        if (a == to) break;
    }
    lv_area_increase(out, 6, 6); /* rounded head cap + anti-aliasing */
}

/* Invalidate the band swept from the old tail end to the new head in
 * 20-degree pieces: one box around the whole comet is mostly empty
 * background, and old/new positions overlap almost entirely. */
static void invalidate_band(inkorb_boot_t *ui, int32_t from, int32_t to)
{
    for (int32_t a = from; a < to; a += 20) {
        lv_area_t area;
        arc_area(ui->comet, a, a + 20 < to ? a + 20 : to, 8, &area);
        lv_obj_invalidate_area(ui->comet, &area);
    }
}

/* One object draws the whole comet: a single pass per redraw area instead
 * of every full-screen arc object running its own draw. */
static void draw_comet(lv_event_t *event)
{
    inkorb_boot_t *ui = lv_event_get_user_data(event);
    if (!ui->comet_opa) return;
    lv_layer_t *layer = lv_event_get_layer(event);
    lv_area_t coords;
    lv_obj_get_coords(ui->comet, &coords);
    lv_draw_arc_dsc_t dsc;
    lv_draw_arc_dsc_init(&dsc);
    dsc.center.x = coords.x1 + RING_SIZE / 2;
    dsc.center.y = coords.y1 + RING_SIZE / 2;
    dsc.radius = RING_SIZE / 2;
    dsc.width = 6;
    dsc.color = lv_color_hex(COL_TAIL);
    /* LVGL draw angles start at 3 o'clock; ours start at the top. */
    int32_t base = ui->comet_head + 270 - HEAD_DEG;
    for (int i = 0; i < TAIL_SEGS; i++) {
        int32_t left = TAIL_SEGS - i;
        uint32_t opa = 210U * left * left / (TAIL_SEGS * TAIL_SEGS) * ui->comet_opa / 255U;
        if (opa < 2) break;
        /* LVGL clips arcs as whole circles; skip segments outside this pass. */
        const lv_area_t *clip = &layer->_clip_area, seg = ui->comet_seg[i];
        if (seg.x2 < clip->x1 || seg.x1 > clip->x2 || seg.y2 < clip->y1 || seg.y1 > clip->y2) continue;
        int32_t from = base - SEG_DEG * (i + 1);
        dsc.start_angle = ((from % 360) + 360) % 360;
        dsc.end_angle = (((from + SEG_DEG) % 360) + 360) % 360;
        dsc.opa = (lv_opa_t)opa;
        lv_draw_arc(layer, &dsc);
    }
    dsc.width = 8;
    dsc.rounded = 1;
    dsc.color = lv_color_hex(COL_HEAD);
    dsc.opa = ui->comet_opa;
    dsc.start_angle = ((base % 360) + 360) % 360;
    dsc.end_angle = ((base + HEAD_DEG) % 360 + 360) % 360;
    lv_draw_arc(layer, &dsc);
}

/* Only the comet's swept area and the progress tip change per frame. */
static void animate(lv_timer_t *timer)
{
    inkorb_boot_t *ui = lv_timer_get_user_data(timer);
    uint32_t now = lv_tick_elaps(ui->started);
    if (!ui->ready && ui->target < ui->cap) ui->target += (ui->cap - ui->target) * 0.006f;
    ui->shown += (ui->target - ui->shown) * (ui->ready ? 0.22f : 0.10f);
    if (ui->target - ui->shown < 0.002f) ui->shown = ui->target;
    int32_t end = (int32_t)(ui->shown * 360.0f + 0.5f);
    if (end >= 360) lv_arc_set_angles(ui->progress, 0, 360);
    else lv_arc_set_angles(ui->progress, 0, end);

    lv_opa_t fade = LV_OPA_COVER;
    if (ui->ready) {
        uint32_t since = lv_tick_elaps(ui->ready_at);
        fade = since >= 240 ? LV_OPA_TRANSP : (lv_opa_t)(255 - since * 255 / 240);
    }
    if (!fade && !ui->comet_opa) return;
    int32_t old_head = ui->comet_head;
    int32_t head = comet_angle(now);
    if (head < old_head) head += 360;  /* wrapped past the top */
    invalidate_band(ui, old_head - HEAD_DEG - TAIL_DEG, head);
    ui->comet_head = head % 360;
    ui->comet_opa = fade;
    for (int i = 0; i < TAIL_SEGS; i++) {
        int32_t top = ui->comet_head - HEAD_DEG - SEG_DEG * (i + 1);
        arc_area(ui->comet, top, top + SEG_DEG, 6, &ui->comet_seg[i]);
    }
}

static void stop_animation(lv_event_t *event)
{
    inkorb_boot_t *ui = lv_event_get_user_data(event);
    if (ui->timer) lv_timer_delete(ui->timer);
    ui->timer = NULL;
}

static lv_obj_t *boot_button(lv_obj_t *scr, const char *caption, bool primary, int32_t y,
                             lv_event_cb_t cb, void *user_data)
{
    lv_obj_t *button = lv_button_create(scr);
    lv_obj_remove_style_all(button);
    lv_obj_set_size(button, 188, 38);
    lv_obj_align(button, LV_ALIGN_CENTER, 0, y);
    lv_obj_set_style_radius(button, 19, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(primary ? 0x4EB4FF : 0x050E1C), 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(button, 1, 0);
    lv_obj_set_style_border_color(button, lv_color_hex(0x4EB4FF), 0);
    lv_obj_add_flag(button, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_HIDDEN);
    if (cb) lv_obj_add_event_cb(button, cb, LV_EVENT_CLICKED, user_data);
    lv_obj_t *text = label(button, &lv_font_montserrat_14, primary ? 0x050E1C : 0x7EC8FF);
    lv_label_set_text(text, caption);
    lv_obj_center(text);
    return button;
}

void inkorb_boot_create(inkorb_boot_t *ui, lv_event_cb_t offline_cb, lv_event_cb_t wifi_cb, void *user_data)
{
    memset(ui, 0, sizeof(*ui));
    lv_obj_t *scr = lv_screen_active();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_OFF);

    ui->orbit = ring(scr, 2, COL_TRACK, LV_OPA_COVER, false);
    lv_arc_set_angles(ui->orbit, 0, 360);
    ui->progress = ring(scr, 4, COL_PROGRESS, LV_OPA_COVER, true);
    ui->comet = lv_obj_create(scr);
    lv_obj_remove_style_all(ui->comet);
    lv_obj_set_size(ui->comet, RING_SIZE, RING_SIZE);
    lv_obj_center(ui->comet);
    lv_obj_clear_flag(ui->comet, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    ui->comet_opa = LV_OPA_COVER;
    lv_obj_add_event_cb(ui->comet, draw_comet, LV_EVENT_DRAW_MAIN, ui);

    lv_obj_t *wordmark = label(scr, &lv_font_montserrat_48, 0xFFFFFF);
    lv_obj_set_style_text_letter_space(wordmark, 8, 0);
    lv_label_set_text(wordmark, "INKORB");
    lv_obj_align(wordmark, LV_ALIGN_CENTER, 0, -20);

    ui->status = label(scr, &lv_font_montserrat_16, 0x89A0B4);
    lv_obj_set_width(ui->status, 300);
    lv_obj_set_style_text_align(ui->status, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(ui->status, "Starting...");
    lv_obj_align(ui->status, LV_ALIGN_CENTER, 0, 35);

    ui->wifi_button = boot_button(scr, "Set up Wi-Fi", true, 104, wifi_cb, user_data);
    ui->offline_button = boot_button(scr, "Continue offline", false, 152, offline_cb, user_data);

    ui->target = 0.04f;
    ui->cap = 0.25f;
    ui->started = lv_tick_get();
    ui->timer = lv_timer_create(animate, 30, ui);
    /* Home creation cleans the screen; take the timer down with the ring. */
    lv_obj_add_event_cb(ui->orbit, stop_animation, LV_EVENT_DELETE, ui);
}

void inkorb_boot_set_status(inkorb_boot_t *ui, const char *text, bool ready)
{
    if (strcmp(lv_label_get_text(ui->status), text) != 0) lv_label_set_text(ui->status, text);
    float floor = 0.04f, cap = 0.25f;
    if (ready) floor = cap = 1.0f;
    else if (!strcmp(text, "Syncing time")) { floor = 0.30f; cap = 0.60f; }
    else if (!strcmp(text, "Getting ready")) { floor = 0.65f; cap = 0.85f; }
    if (ui->target < floor) ui->target = floor;
    ui->cap = cap;
    if (ready && !ui->ready) {
        ui->ready = true;
        ui->ready_at = lv_tick_get();
        lv_obj_set_style_arc_color(ui->progress, lv_color_hex(COL_READY), LV_PART_INDICATOR);
    }
    lv_obj_set_style_text_color(ui->status, lv_color_hex(ready ? COL_READY : 0x89A0B4), 0);
}

void inkorb_boot_allow_offline(inkorb_boot_t *ui)
{
    lv_obj_remove_flag(ui->offline_button, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(ui->wifi_button, LV_OBJ_FLAG_HIDDEN);
}
