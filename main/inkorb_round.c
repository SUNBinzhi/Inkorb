#include "inkorb_round.h"
#include "inkorb_ui.h"
#include <math.h>
LV_FONT_DECLARE(lv_font_inkorb_cn18);

#define SCREEN 466
#define CENTER 233

/* Swipe right = back, but not when the finger is on the rim: that band
 * belongs to the rim sliders, and a drag along it can read as a swipe. */
static void on_page_gesture(lv_event_t *event)
{
    lv_event_cb_t back = (lv_event_cb_t)lv_event_get_user_data(event);
    lv_indev_t *indev = lv_indev_active();
    if (!back || lv_indev_get_gesture_dir(indev) != LV_DIR_RIGHT) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    float dx = p.x - CENTER, dy = p.y - CENTER;
    if (dx * dx + dy * dy > 175.0f * 175.0f) return;
    back(event);
}

lv_obj_t *round_page(lv_event_cb_t on_back)
{
    lv_obj_t *page = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(page);
    lv_obj_set_size(page, SCREEN, SCREEN);
    lv_obj_set_style_bg_color(page, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(page, LV_OPA_COVER, 0);
    lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_flag(page, INKORB_OVERLAY_FLAG);
    if (on_back) lv_obj_add_event_cb(page, on_page_gesture, LV_EVENT_GESTURE, (void *)on_back);
    return page;
}

lv_obj_t *round_text(lv_obj_t *parent, uint32_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, &lv_font_inkorb_cn18, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_set_style_text_line_space(label, 6, 0);
    lv_obj_add_flag(label, LV_OBJ_FLAG_GESTURE_BUBBLE);
    return label;
}

void round_place(lv_obj_t *obj, int32_t angle, int32_t radius)
{
    float a = angle * 0.01745329f;
    lv_obj_align(obj, LV_ALIGN_CENTER, (int32_t)lroundf(radius * cosf(a)), (int32_t)lroundf(radius * sinf(a)));
}

/* Curved rim title, drawn smooth: the text is rendered flat once, then each
 * pixel of a band along the top rim samples it (bilinear) at the matching arc
 * position. Rotating each glyph of the 2-bpp CJK font (lv_arclabel) looked
 * jagged and measured letter spacing loosely, so it sat off centre. */
#define TITLE_R      205.0f   /* radius of the text's vertical centre */
#define TITLE_W      300
#define TITLE_H      64
#define TITLE_TOP    4

typedef struct { lv_image_dsc_t dsc; int32_t text_w; } title_img_t;

static void free_title(lv_event_t *event)
{
    title_img_t *img = lv_event_get_user_data(event);
    lv_free((void *)img->dsc.data);
    lv_free(img);
}

static title_img_t *render_title(const char *text)
{
    const lv_font_t *font = &lv_font_inkorb_cn18;
    const int32_t spacing = 2;
    lv_point_t size;
    lv_text_get_size(&size, text, font, spacing, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    int32_t tw = size.x + 4, th = size.y + 4;
    uint32_t stride = LV_DRAW_BUF_STRIDE(tw, LV_COLOR_FORMAT_ARGB8888);
    uint8_t *flat = lv_malloc_zeroed(stride * th);
    title_img_t *img = lv_malloc_zeroed(sizeof(*img));
    uint8_t *out = lv_malloc_zeroed(TITLE_W * TITLE_H);
    if (!flat || !img || !out) { lv_free(flat); lv_free(img); lv_free(out); return NULL; }

    /* 1. Flat white text on a transparent ARGB canvas; alpha = coverage. */
    lv_obj_t *canvas = lv_canvas_create(lv_layer_sys());
    lv_canvas_set_buffer(canvas, flat, tw, th, LV_COLOR_FORMAT_ARGB8888);
    lv_canvas_fill_bg(canvas, lv_color_black(), LV_OPA_TRANSP);
    lv_layer_t layer;
    lv_canvas_init_layer(canvas, &layer);
    lv_draw_label_dsc_t dsc;
    lv_draw_label_dsc_init(&dsc);
    dsc.text = text;
    dsc.font = font;
    dsc.color = lv_color_white();
    dsc.letter_space = spacing;
    lv_area_t area = { 2, 2, tw - 3, th - 3 };
    lv_draw_label(&layer, &dsc, &area);
    lv_canvas_finish_layer(canvas, &layer);
    lv_obj_delete(canvas);

    /* 2. Warp onto the arc, centred on 12 o'clock. Text "up" points outward. */
    for (int oy = 0; oy < TITLE_H; oy++) {
        for (int ox = 0; ox < TITLE_W; ox++) {
            float dx = ox + (SCREEN - TITLE_W) / 2 + 0.5f - CENTER;
            float dy = oy + TITLE_TOP + 0.5f - CENTER;
            float r = sqrtf(dx * dx + dy * dy);
            float u = atan2f(dx, -dy) * TITLE_R + tw / 2.0f - 0.5f;   /* along the arc */
            float v = (TITLE_R + th / 2.0f) - r - 0.5f;               /* outer edge = text top */
            if (u < 0 || v < 0 || u >= tw - 1 || v >= th - 1) continue;
            int x0 = (int)u, y0 = (int)v;
            float fx = u - x0, fy = v - y0;
            const uint8_t *p = flat + y0 * stride + x0 * 4 + 3;      /* alpha byte */
            float a = p[0] * (1 - fx) * (1 - fy) + p[4] * fx * (1 - fy) +
                      p[stride] * (1 - fx) * fy + p[stride + 4] * fx * fy;
            out[oy * TITLE_W + ox] = (uint8_t)(a + 0.5f);
        }
    }
    lv_free(flat);
    img->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    img->dsc.header.cf = LV_COLOR_FORMAT_A8;
    img->dsc.header.w = TITLE_W;
    img->dsc.header.h = TITLE_H;
    img->dsc.header.stride = TITLE_W;
    img->dsc.data_size = TITLE_W * TITLE_H;
    img->dsc.data = out;
    img->text_w = tw;
    return img;
}

lv_obj_t *round_title(lv_obj_t *page, const char *text, lv_event_cb_t on_tap)
{
    title_img_t *img = render_title(text);
    lv_obj_t *title;
    if (img) {
        title = lv_image_create(page);
        lv_image_set_src(title, &img->dsc);
        lv_obj_set_pos(title, (SCREEN - TITLE_W) / 2, TITLE_TOP);
        lv_obj_set_style_image_recolor(title, lv_color_hex(ROUND_MUTED), 0);
        lv_obj_set_style_image_recolor_opa(title, LV_OPA_COVER, 0);
        lv_obj_add_event_cb(title, free_title, LV_EVENT_DELETE, img);
    } else {
        title = round_text(page, ROUND_MUTED);    /* out of memory: plain label */
        lv_label_set_text(title, text);
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 24);
    }
    lv_obj_add_flag(title, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_clear_flag(title, LV_OBJ_FLAG_CLICKABLE);
    if (on_tap) {
        /* A "‹" just left of the title, on the same arc, says it goes back. */
        lv_obj_t *chevron = lv_label_create(page);
        lv_obj_set_style_text_font(chevron, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(chevron, lv_color_hex(ROUND_MUTED), 0);
        lv_label_set_text(chevron, LV_SYMBOL_LEFT);
        int32_t half = img ? img->text_w / 2 : 40;
        round_place(chevron, 270 - (int32_t)((half + 16) / TITLE_R * 57.29578f), (int32_t)TITLE_R);
        lv_obj_t *zone = lv_obj_create(page);
        lv_obj_remove_style_all(zone);
        lv_obj_set_size(zone, 240, 60);
        lv_obj_align(zone, LV_ALIGN_TOP_MID, 0, 6);
        lv_obj_add_flag(zone, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
        lv_obj_add_event_cb(zone, on_tap, LV_EVENT_CLICKED, NULL);
    }
    return title;
}

lv_obj_t *round_button(lv_obj_t *page, const char *icon, const char *caption_text, int32_t angle,
                       int32_t radius, int32_t size, lv_event_cb_t cb, void *user_data, lv_obj_t **caption)
{
    lv_obj_t *b = lv_button_create(page);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, size, size);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(ROUND_CARD), 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(ROUND_TRACK), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_border_color(b, lv_color_hex(ROUND_TRACK), 0);
    /* Visible press: border lights up, button shrinks and glows. */
    lv_obj_set_style_border_width(b, 2, LV_STATE_PRESSED);
    lv_obj_set_style_border_color(b, lv_color_hex(ROUND_NEON), LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(b, 18, LV_STATE_PRESSED);
    lv_obj_set_style_shadow_color(b, lv_color_hex(ROUND_NEON), LV_STATE_PRESSED);
    lv_obj_set_style_shadow_opa(b, LV_OPA_50, LV_STATE_PRESSED);
    lv_obj_set_style_transform_scale(b, 230, LV_STATE_PRESSED);
    lv_obj_set_style_transform_pivot_x(b, size / 2, 0);
    lv_obj_set_style_transform_pivot_y(b, size / 2, 0);
    static const lv_style_prop_t props[] = {LV_STYLE_TRANSFORM_SCALE_X, LV_STYLE_TRANSFORM_SCALE_Y, 0};
    static lv_style_transition_dsc_t tr;
    static bool tr_init;
    if (!tr_init) { lv_style_transition_dsc_init(&tr, props, lv_anim_path_ease_out, 90, 0, NULL); tr_init = true; }
    lv_obj_set_style_transition(b, &tr, 0);
    lv_obj_set_style_transition(b, &tr, LV_STATE_PRESSED);
    lv_obj_set_ext_click_area(b, 12);
    lv_obj_add_flag(b, LV_OBJ_FLAG_GESTURE_BUBBLE);
    round_place(b, angle, radius);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, user_data);
    if (icon) {
        lv_obj_t *glyph = lv_label_create(b);
        lv_obj_set_style_text_font(glyph, &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_color(glyph, lv_color_hex(ROUND_BLUE), 0);
        lv_label_set_text(glyph, icon);
        lv_obj_center(glyph);
    }
    if (caption_text) {
        lv_obj_t *c = round_text(page, ROUND_MUTED);
        lv_label_set_text(c, caption_text);
        lv_obj_align_to(c, b, LV_ALIGN_OUT_BOTTOM_MID, 0, 4);
        if (caption) *caption = c;
    }
    return b;
}

lv_obj_t *round_rim_arc(lv_obj_t *page, int32_t start, int32_t end, int32_t width, uint32_t color,
                        bool knob, bool reverse)
{
    lv_obj_t *arc = lv_arc_create(page);
    /* Sliders sit a little further in so their round knob stays on the glass:
     * track outer edge r=223, knob (14 + 2*6 = 26 px) outer edge r=229 < 233. */
    int32_t size = knob ? SCREEN - 20 : SCREEN - 14;
    lv_obj_set_size(arc, size, size);
    lv_obj_center(arc);
    lv_arc_set_rotation(arc, 0);
    lv_arc_set_bg_angles(arc, start, end);
    lv_arc_set_mode(arc, reverse ? LV_ARC_MODE_REVERSE : LV_ARC_MODE_NORMAL);
    lv_arc_set_range(arc, 0, 100);
    lv_obj_set_style_pad_all(arc, 0, 0);
    lv_obj_set_style_arc_width(arc, width, LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, width, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc, lv_color_hex(ROUND_TRACK), LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc, lv_color_hex(color), LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(arc, true, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(arc, true, LV_PART_INDICATOR);
    if (knob) {
        /* A slider's drags must never bubble up as page gestures (swipe-back). */
        lv_obj_clear_flag(arc, LV_OBJ_FLAG_GESTURE_BUBBLE);
        /* LVGL's own round knob: it invalidates exactly what it draws, so a
         * drag leaves no trail (a hand-drawn handle on top did). */
        lv_obj_set_style_bg_color(arc, lv_color_white(), LV_PART_KNOB);
        lv_obj_set_style_bg_opa(arc, LV_OPA_COVER, LV_PART_KNOB);
        lv_obj_set_style_pad_all(arc, 6, LV_PART_KNOB);
        lv_obj_set_style_shadow_width(arc, 0, LV_PART_KNOB);
        /* Only the rim band reacts to touch, so the centre stays tappable; the
         * extra click area widens that band inward to ~44 px for a fingertip. */
        lv_obj_add_flag(arc, LV_OBJ_FLAG_ADV_HITTEST);
        lv_obj_set_ext_click_area(arc, 30);
    } else {
        lv_obj_remove_style(arc, NULL, LV_PART_KNOB);
        lv_obj_clear_flag(arc, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(arc, LV_OBJ_FLAG_GESTURE_BUBBLE);
    }
    return arc;
}

static void draw_sun(lv_event_t *event)
{
    lv_obj_t *obj = lv_event_get_target(event);
    lv_layer_t *layer = lv_event_get_layer(event);
    lv_color_t color = lv_obj_get_style_text_color(obj, 0);
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    float cx = (a.x1 + a.x2) / 2.0f, cy = (a.y1 + a.y2) / 2.0f;
    lv_draw_arc_dsc_t disc;
    lv_draw_arc_dsc_init(&disc);
    disc.color = color;
    disc.center.x = (int32_t)cx;
    disc.center.y = (int32_t)cy;
    disc.radius = 5;
    disc.width = 5;
    disc.start_angle = 0;
    disc.end_angle = 360;
    lv_draw_arc(layer, &disc);
    lv_draw_line_dsc_t ray;
    lv_draw_line_dsc_init(&ray);
    ray.color = color;
    ray.width = 2;
    ray.round_start = ray.round_end = 1;
    for (int i = 0; i < 8; i++) {
        float t = i * 0.785398f;
        ray.p1.x = cx + 8.5f * cosf(t);
        ray.p1.y = cy + 8.5f * sinf(t);
        ray.p2.x = cx + 11.5f * cosf(t);
        ray.p2.y = cy + 11.5f * sinf(t);
        lv_draw_line(layer, &ray);
    }
}

lv_obj_t *round_sun_icon(lv_obj_t *parent, uint32_t color)
{
    lv_obj_t *sun = lv_obj_create(parent);
    lv_obj_remove_style_all(sun);
    lv_obj_set_size(sun, 26, 26);
    lv_obj_set_style_text_color(sun, lv_color_hex(color), 0);
    lv_obj_clear_flag(sun, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(sun, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(sun, draw_sun, LV_EVENT_DRAW_MAIN, NULL);
    return sun;
}
