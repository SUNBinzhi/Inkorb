#include "inkorb_ui.h"
#include "inkorb_time_bloom_img.h"
#include "inkorb_mic.h"

#include <stdio.h>
#include <math.h>
#include <string.h>
#include <time.h>

/* v3c: real large time face (custom Montserrat Medium 72) — same on sim & ESP.
 * Stock Montserrat tops out at 48; transform_scale is NOT used for size. */
LV_FONT_DECLARE(lv_font_montserrat_time_72);
#define INKORB_FONT_TIME (&lv_font_montserrat_time_72)

/* Title: prefer larger Montserrat for geometric weight (no separate Bold in LVGL). */
#if LV_FONT_MONTSERRAT_36
#define INKORB_FONT_TITLE &lv_font_montserrat_36
#elif LV_FONT_MONTSERRAT_28
#define INKORB_FONT_TITLE &lv_font_montserrat_28
#elif LV_FONT_MONTSERRAT_24
#define INKORB_FONT_TITLE &lv_font_montserrat_24
#else
#define INKORB_FONT_TITLE LV_FONT_DEFAULT
#endif

#if LV_FONT_MONTSERRAT_18
#define INKORB_FONT_DATE &lv_font_montserrat_18
#elif LV_FONT_MONTSERRAT_16
#define INKORB_FONT_DATE &lv_font_montserrat_16
#else
#define INKORB_FONT_DATE LV_FONT_DEFAULT
#endif

#if LV_FONT_MONTSERRAT_14
#define INKORB_FONT_WEEKDAY &lv_font_montserrat_14
#elif LV_FONT_MONTSERRAT_12
#define INKORB_FONT_WEEKDAY &lv_font_montserrat_12
#else
#define INKORB_FONT_WEEKDAY LV_FONT_DEFAULT
#endif

#if LV_FONT_MONTSERRAT_16
#define INKORB_FONT_IP &lv_font_montserrat_16
#elif LV_FONT_MONTSERRAT_14
#define INKORB_FONT_IP &lv_font_montserrat_14
#else
#define INKORB_FONT_IP LV_FONT_DEFAULT
#endif

#if LV_FONT_MONTSERRAT_20
#define INKORB_FONT_WIFI &lv_font_montserrat_20
#elif LV_FONT_MONTSERRAT_16
#define INKORB_FONT_WIFI &lv_font_montserrat_16
#else
#define INKORB_FONT_WIFI LV_FONT_DEFAULT
#endif

#define INKORB_COL_WHITE  0xFFFFFF
#define INKORB_COL_IP     0x7EC8FF
#define INKORB_COL_CAP_BG 0x050E1C
#define INKORB_COL_CAP_BD 0x4EB4FF
#define INKORB_COL_CAP_SH 0x1E90FF
#define INKORB_COL_DIV    0x555555
#define INKORB_COL_BAT    0x62E69A

/* Codex v3 — match ref-inkorb-face.png (center-relative, 466 circle).
 * Ref measured: title~-136, time~-32, date~+41, weekday~+66, capsule~+120;
 * capsule min rim margin target ≥24px (v2c was ~7px). */
#define Y_TITLE   (-136)
#define Y_TIME    (-32)
#define Y_DATE    (41)
#define Y_WEEKDAY (66)
#define Y_CAPSULE (120)

#define CAP_W 270
#define CAP_H 48

/* v3i: no transform_scale — sim and firmware share the same 72px face. */

static const char *const k_weekdays[] = {
    "SUNDAY", "MONDAY", "TUESDAY", "WEDNESDAY",
    "THURSDAY", "FRIDAY", "SATURDAY"
};

static lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color,
                            int32_t letter_space)
{
    lv_obj_t *lbl = lv_label_create(parent);
    lv_obj_remove_style_all(lbl); /* kill theme grey/bg so time never gets a pill */
    lv_obj_set_style_text_font(lbl, font, 0);
    lv_obj_set_style_text_color(lbl, color, 0);
    lv_obj_set_style_text_opa(lbl, LV_OPA_COVER, 0);
    lv_obj_set_style_text_letter_space(lbl, letter_space, 0);
    lv_obj_set_style_bg_opa(lbl, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(lbl, 0, 0);
    lv_obj_set_style_outline_width(lbl, 0, 0);
    lv_obj_set_style_shadow_width(lbl, 0, 0);
    lv_obj_set_style_pad_all(lbl, 0, 0);
    return lbl;
}

static void style_glow_ghost(lv_obj_t *lbl, lv_opa_t opa)
{
    lv_obj_set_style_text_opa(lbl, opa, 0);
    lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
}

static void weekday_from_date(char *out, size_t out_sz, const char *date_str)
{
    int y = 0, m = 0, d = 0;
    if (!date_str || sscanf(date_str, "%d-%d-%d", &y, &m, &d) != 3 || y < 1970) {
        snprintf(out, out_sz, "--------");
        return;
    }
    struct tm t;
    memset(&t, 0, sizeof(t));
    t.tm_year = y - 1900;
    t.tm_mon = m - 1;
    t.tm_mday = d;
    t.tm_isdst = -1;
    if (mktime(&t) == (time_t)-1 || t.tm_wday < 0 || t.tm_wday > 6) {
        snprintf(out, out_sz, "--------");
        return;
    }
    snprintf(out, out_sz, "%s", k_weekdays[t.tm_wday]);
}

static void sync_glow_text(lv_obj_t **glows, int n, const char *text)
{
    for (int i = 0; i < n; i++) {
        if (glows[i]) {
            lv_label_set_text(glows[i], text ? text : "");
        }
    }
}


/* v3l: bloom drive — smooth timer lerp (no lv_anim scale jumps) + mic overlay.
 * Peak flash fix: rate-limit integer scale, soft max, gentler opa (no COVER spike).
 * A8 mask + recolor: quiet white soft ellipse; mic gate → lerp to IP cyan #7EC8FF (no RGB565 hard rect). */
#define INKORB_BLOOM_TICK_MS       25
#define INKORB_BLOOM_START_DELAY   2200
#define INKORB_BLOOM_SCALE_BASE    ((float)LV_SCALE_NONE)          /* 256 = 1.0 */
#define INKORB_BLOOM_SCALE_MIN_F   (INKORB_BLOOM_SCALE_BASE * 0.88f)
#define INKORB_BLOOM_SCALE_MAX_F   (INKORB_BLOOM_SCALE_BASE * 1.14f) /* visible but DMA-safer */
#define INKORB_BLOOM_VOICE_MIN_F   (INKORB_BLOOM_SCALE_BASE * 1.10f)
#define INKORB_BLOOM_VOICE_MAX_F   (INKORB_BLOOM_SCALE_BASE * 1.18f) /* below known ~1.22 DMA risk */
#define INKORB_BLOOM_OPA_MIN_F     100.0f
#define INKORB_BLOOM_OPA_MAX_F     255.0f  /* v3j: full crest; rate-limit still softens flash */
#define INKORB_BLOOM_IDLE_PERIOD_S  3.4f
#define INKORB_BLOOM_LERP          0.32f   /* v3j: snappier follow */
#define INKORB_BLOOM_VOICE_GATE    0.035f  /* v3n: quieter speech also turns blue */
#define INKORB_BLOOM_COLOR_LERP    0.22f   /* v3k: softer than drive; no hard cut */
#define INKORB_BLOOM_COL_IDLE_R    255
#define INKORB_BLOOM_COL_IDLE_G    255
#define INKORB_BLOOM_COL_IDLE_B    255
#define INKORB_BLOOM_COL_VOICE_R   0x7E    /* same family as Wi‑Fi/IP capsule */
#define INKORB_BLOOM_COL_VOICE_G   0xC8
#define INKORB_BLOOM_COL_VOICE_B   0xFF

typedef struct {
    lv_obj_t *bloom;
    float phase;       /* idle sine radians */
    float smooth;      /* 0..1 drive */
    float color_smooth; /* 0=white idle, 1=voice cyan */
    int32_t last_scale;
    lv_opa_t last_opa;
    uint8_t last_cr;
    uint8_t last_cg;
    uint8_t last_cb;
    uint8_t scale_gate;
    bool running;
} bloom_fx_t;

static bloom_fx_t s_bloom_fx;

static float bloom_clampf(float v, float lo, float hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static void bloom_fx_apply(bloom_fx_t *fx, float drive01, float color01)
{
    float d = bloom_clampf(drive01, 0.0f, 1.0f);
    float c = bloom_clampf(color01, 0.0f, 1.0f);
    float scale_f = INKORB_BLOOM_SCALE_MIN_F +
                    (INKORB_BLOOM_SCALE_MAX_F - INKORB_BLOOM_SCALE_MIN_F) * d;
    /* v3m: even softer speech gets a larger blue halo. Follow the existing
     * smoothed voice tint so expansion and return to idle stay continuous. */
    float voice_scale_f = INKORB_BLOOM_VOICE_MIN_F +
                         (INKORB_BLOOM_VOICE_MAX_F - INKORB_BLOOM_VOICE_MIN_F) * d;
    scale_f += (voice_scale_f - scale_f) * c;
    float opa_f = INKORB_BLOOM_OPA_MIN_F +
                  (INKORB_BLOOM_OPA_MAX_F - INKORB_BLOOM_OPA_MIN_F) * d;

    int32_t scale = (int32_t)(scale_f + 0.5f);
    lv_opa_t opa = (lv_opa_t)(opa_f + 0.5f);

    /* RGB lerp white → IP cyan on A8 mask (alpha keeps soft ellipse; no black-rect dye). */
    uint8_t cr = (uint8_t)(INKORB_BLOOM_COL_IDLE_R +
                           (INKORB_BLOOM_COL_VOICE_R - INKORB_BLOOM_COL_IDLE_R) * c + 0.5f);
    uint8_t cg = (uint8_t)(INKORB_BLOOM_COL_IDLE_G +
                           (INKORB_BLOOM_COL_VOICE_G - INKORB_BLOOM_COL_IDLE_G) * c + 0.5f);
    uint8_t cb = (uint8_t)(INKORB_BLOOM_COL_IDLE_B +
                           (INKORB_BLOOM_COL_VOICE_B - INKORB_BLOOM_COL_IDLE_B) * c + 0.5f);

    /* Opa every tick (cheap). Scale only on real int change + at most ~every other tick
     * to limit transform buffer churn / SPI DMA NO_MEM while mic is active. */
    if (opa != fx->last_opa) {
        lv_obj_set_style_image_opa(fx->bloom, opa, 0);
        fx->last_opa = opa;
    }
    if (cr != fx->last_cr || cg != fx->last_cg || cb != fx->last_cb) {
        lv_obj_set_style_image_recolor(fx->bloom, lv_color_make(cr, cg, cb), 0);
        fx->last_cr = cr;
        fx->last_cg = cg;
        fx->last_cb = cb;
    }
    fx->scale_gate++;
    if (scale != fx->last_scale && (fx->scale_gate & 1)) {
        lv_image_set_scale(fx->bloom, (uint32_t)scale);
        fx->last_scale = scale;
    }
}

static void bloom_fx_tick(lv_timer_t *t)
{
    bloom_fx_t *fx = (bloom_fx_t *)lv_timer_get_user_data(t);
    if (!fx || !fx->bloom || !lv_obj_is_valid(fx->bloom)) {
        return;
    }

    fx->phase += (6.2831853f * ((float)INKORB_BLOOM_TICK_MS / 1000.0f)) / INKORB_BLOOM_IDLE_PERIOD_S;
    if (fx->phase > 6.2831853f) {
        fx->phase -= 6.2831853f;
    }
    /* Idle breathe stays subtle so mic drive reads clearly (v3j). */
    float idle = 0.5f + 0.5f * sinf(fx->phase);
    idle *= 0.22f;

    float mic = inkorb_mic_level(); /* already EMA'd */
    float target = idle;
    if (mic > target) {
        target = mic;
    }

    /* v3k: voice gate on mic alone — quiet keeps white; sound lerps to cyan. */
    float color_target = (mic > INKORB_BLOOM_VOICE_GATE) ? 1.0f : 0.0f;
    fx->color_smooth += (color_target - fx->color_smooth) * INKORB_BLOOM_COLOR_LERP;

    fx->smooth += (target - fx->smooth) * INKORB_BLOOM_LERP;
    bloom_fx_apply(fx, fx->smooth, fx->color_smooth);
}

static void bloom_fx_start_now(lv_timer_t *t)
{
    bloom_fx_t *fx = (bloom_fx_t *)lv_timer_get_user_data(t);
    lv_timer_delete(t);
    if (!fx || !fx->bloom || !lv_obj_is_valid(fx->bloom) || fx->running) {
        return;
    }
    fx->running = true;
    fx->phase = 0.0f;
    fx->smooth = 0.2f;
    fx->color_smooth = 0.0f;
    fx->last_scale = -1;
    fx->last_opa = 0;
    fx->last_cr = fx->last_cg = fx->last_cb = 255;
    lv_image_set_pivot(fx->bloom, inkorb_time_bloom_img.header.w / 2,
                       inkorb_time_bloom_img.header.h / 2);
    lv_image_set_antialias(fx->bloom, false); /* sharper zoom edges, less white fringe flash */
    lv_obj_set_style_image_recolor(fx->bloom, lv_color_make(255, 255, 255), 0);
    lv_obj_set_style_image_recolor_opa(fx->bloom, LV_OPA_COVER, 0);
    lv_timer_create(bloom_fx_tick, INKORB_BLOOM_TICK_MS, fx);
}

static void start_time_bloom_breathe(lv_obj_t *bloom)
{
    if (!bloom) {
        return;
    }
    s_bloom_fx.bloom = bloom;
    s_bloom_fx.running = false;
    s_bloom_fx.smooth = 0.2f;
    s_bloom_fx.color_smooth = 0.0f;
    s_bloom_fx.last_scale = LV_SCALE_NONE;
    s_bloom_fx.last_opa = LV_OPA_COVER;
    s_bloom_fx.last_cr = s_bloom_fx.last_cg = s_bloom_fx.last_cb = 255;
    lv_image_set_pivot(bloom, inkorb_time_bloom_img.header.w / 2,
                       inkorb_time_bloom_img.header.h / 2);
    lv_image_set_scale(bloom, LV_SCALE_NONE);
    lv_obj_set_style_image_opa(bloom, (lv_opa_t)INKORB_BLOOM_OPA_MIN_F, 0);
    lv_obj_set_style_image_recolor(bloom, lv_color_make(255, 255, 255), 0);
    lv_obj_set_style_image_recolor_opa(bloom, LV_OPA_COVER, 0);
    lv_timer_t *tm = lv_timer_create(bloom_fx_start_now, INKORB_BLOOM_START_DELAY, &s_bloom_fx);
    lv_timer_set_repeat_count(tm, 1);
}

/* Rim arcs: thin white arcs concentric with the glass at the bottom and,
 * mirrored, at the top, fading out toward both sides. The settings gear sits
 * just inside the bottom arc. Rendered once into A8 images with per-pixel
 * distance anti-aliasing and a continuous fade (no banding); at runtime each
 * is one image blit, in strips clear of the breathing glow. */
#define RIM_R     230.5f    /* hugs the glass: line spans r 229.5-231.5 of 233 */
#define RIM_FULL  0.23f     /* full brightness out to this angle from the axis, radians */
#define RIM_FADE  0.94f     /* then fades to zero over this many radians */
#define RIM_TOP   300       /* bottom strip starts here; the top strip mirrors it */
#define RIM_W     440
#define RIM_H     (466 - RIM_TOP)
#define RIM_PEAK  0.9f
#define GEAR_Y    (RIM_R - 22.0f)   /* gear centre, just inside the bottom arc */

static lv_image_dsc_t s_rim_img[2];   /* [0] bottom, [1] top (mirror) */

static void fill_dsc(lv_image_dsc_t *dsc, uint8_t *px)
{
    dsc->header.magic = LV_IMAGE_HEADER_MAGIC;
    dsc->header.cf = LV_COLOR_FORMAT_A8;
    dsc->header.w = RIM_W;
    dsc->header.h = RIM_H;
    dsc->header.stride = RIM_W;
    dsc->data_size = RIM_W * RIM_H;
    dsc->data = px;
}

static bool rim_images(void)
{
    if (s_rim_img[0].data) return true;
    uint32_t started = lv_tick_get();
    uint8_t *px = lv_malloc_zeroed(RIM_W * RIM_H), *top = lv_malloc(RIM_W * RIM_H);
    if (!px || !top) { lv_free(px); lv_free(top); return false; }
    for (int py = 0; py < RIM_H; py++) {
        for (int pxl = 0; pxl < RIM_W; pxl++) {
            /* Pixel centre relative to the screen centre (y grows downward). */
            float x = pxl + (466 - RIM_W) / 2 + 0.5f - 233.0f, y = py + RIM_TOP + 0.5f - 233.0f;
            float dist = fabsf(sqrtf(x * x + y * y) - RIM_R);
            float cover = fminf(fmaxf(1.5f - dist, 0.0f), 1.0f);
            if (cover <= 0) continue;
            float aphi = fabsf(atan2f(x, y));      /* 0 straight down */
            if (aphi > RIM_FULL + RIM_FADE) continue;
            float fade = 1.0f;
            if (aphi > RIM_FULL) {
                float u = (aphi - RIM_FULL) / RIM_FADE;
                fade = 0.5f * (1.0f + cosf(3.1415927f * u));   /* smooth both ends */
            }
            px[py * RIM_W + pxl] = (uint8_t)(cover * fade * RIM_PEAK * 255.0f + 0.5f);
        }
    }
    for (int row = 0; row < RIM_H; row++)
        memcpy(top + row * RIM_W, px + (RIM_H - 1 - row) * RIM_W, RIM_W);
    fill_dsc(&s_rim_img[0], px);
    fill_dsc(&s_rim_img[1], top);
    printf("inkorb_ui: rim arcs rendered in %u ms\n", (unsigned)lv_tick_elaps(started));
    return true;
}

static void rim_arc(lv_obj_t *scr, const lv_image_dsc_t *img, int32_t y)
{
    lv_obj_t *rim = lv_image_create(scr);
    lv_image_set_src(rim, img);
    lv_obj_set_pos(rim, (466 - RIM_W) / 2, y);
    lv_obj_set_style_image_recolor(rim, lv_color_white(), 0);
    lv_obj_set_style_image_recolor_opa(rim, LV_OPA_COVER, 0);
    lv_obj_clear_flag(rim, LV_OBJ_FLAG_CLICKABLE);
}

void inkorb_home_create(inkorb_home_t *out)
{
    memset(out, 0, sizeof(*out));

    lv_obj_t *scr = lv_screen_active();
    lv_obj_clean(scr);

    lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_OFF);

    /* Title: Montserrat ~36, letter_space 18 (wide geometric tracking), 3 light ghosts. */
    static const int32_t title_ox[3] = { -1, 1, 0 };
    static const int32_t title_oy[3] = { 0, 1, -1 };
    static const lv_opa_t title_opa[3] = { LV_OPA_20, LV_OPA_20, LV_OPA_10 };
    for (int i = 0; i < 3; i++) {
        out->title_glow[i] = make_label(scr, INKORB_FONT_TITLE, lv_color_hex(INKORB_COL_WHITE), 18);
        lv_label_set_text(out->title_glow[i], "INKORB");
        style_glow_ghost(out->title_glow[i], title_opa[i]);
        lv_obj_align(out->title_glow[i], LV_ALIGN_CENTER, title_ox[i], Y_TITLE + title_oy[i]);
    }
    out->title = make_label(scr, INKORB_FONT_TITLE, lv_color_hex(INKORB_COL_WHITE), 18);
    lv_label_set_text(out->title, "INKORB");
    lv_obj_align(out->title, LV_ALIGN_CENTER, 0, Y_TITLE);


    /* v3l: soft oval bloom UNDER the time (A8 alpha mask + recolor).
     * Same elliptical Gaussian as v3g (luma→A8); recolor white/cyan without hard rect.
     * Glyph time_glow[] stay NULL. */
    for (int i = 0; i < 8; i++) {
        out->time_glow[i] = NULL;
    }

    /* v3l: A8 soft bloom + breathe/mic + voice cyan recolor. Digits still. */
    out->time_bloom = lv_image_create(scr);
    lv_image_set_src(out->time_bloom, &inkorb_time_bloom_img);
    lv_obj_set_style_image_opa(out->time_bloom, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_opa(out->time_bloom, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(out->time_bloom, 0, 0);
    lv_obj_set_style_outline_width(out->time_bloom, 0, 0);
    lv_obj_set_style_pad_all(out->time_bloom, 0, 0);
    lv_obj_set_style_shadow_width(out->time_bloom, 0, 0);
    lv_obj_clear_flag(out->time_bloom, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(out->time_bloom, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(out->time_bloom, LV_ALIGN_CENTER, 0, Y_TIME);
    start_time_bloom_breathe(out->time_bloom);

    /* Crisp time face on top of bloom — Montserrat 72, no glyph ghosts. */
    out->time_label = make_label(scr, INKORB_FONT_TIME, lv_color_hex(INKORB_COL_WHITE), 4);
    lv_label_set_text(out->time_label, "14:32");
    lv_obj_align(out->time_label, LV_ALIGN_CENTER, 0, Y_TIME);

    out->date_label = make_label(scr, INKORB_FONT_DATE, lv_color_hex(INKORB_COL_WHITE), 2);
    lv_label_set_text(out->date_label, "2026-10-28");
    lv_obj_align(out->date_label, LV_ALIGN_CENTER, 0, Y_DATE);

    out->weekday_label = make_label(scr, INKORB_FONT_WEEKDAY, lv_color_hex(INKORB_COL_WHITE), 12);
    lv_label_set_text(out->weekday_label, "WEDNESDAY");
    lv_obj_align(out->weekday_label, LV_ALIGN_CENTER, 0, Y_WEEKDAY);

    out->capsule = lv_obj_create(scr);
    lv_obj_set_size(out->capsule, CAP_W, CAP_H);
    lv_obj_set_style_radius(out->capsule, CAP_H / 2, 0);
    lv_obj_set_style_bg_color(out->capsule, lv_color_hex(INKORB_COL_CAP_BG), 0);
    lv_obj_set_style_bg_opa(out->capsule, (lv_opa_t)191, 0); /* ~75% */
    lv_obj_set_style_border_width(out->capsule, 3, 0);
    lv_obj_set_style_border_color(out->capsule, lv_color_hex(INKORB_COL_CAP_BD), 0);
    lv_obj_set_style_border_opa(out->capsule, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_width(out->capsule, 14, 0);
    lv_obj_set_style_shadow_spread(out->capsule, 2, 0);
    lv_obj_set_style_shadow_color(out->capsule, lv_color_hex(INKORB_COL_CAP_SH), 0);
    lv_obj_set_style_shadow_opa(out->capsule, LV_OPA_50, 0);
    lv_obj_set_style_pad_left(out->capsule, 12, 0);
    lv_obj_set_style_pad_right(out->capsule, 12, 0);
    lv_obj_set_style_pad_top(out->capsule, 0, 0);
    lv_obj_set_style_pad_bottom(out->capsule, 0, 0);
    lv_obj_clear_flag(out->capsule, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(out->capsule, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_flex_flow(out->capsule, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(out->capsule, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(out->capsule, 7, 0);
    lv_obj_align(out->capsule, LV_ALIGN_CENTER, 0, Y_CAPSULE);

    /* Wi‑Fi must be #7EC8FF (same as IP), never white. */
    out->wifi_icon = make_label(out->capsule, INKORB_FONT_WIFI, lv_color_hex(INKORB_COL_IP), 0);
    lv_label_set_text(out->wifi_icon, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_color(out->wifi_icon, lv_color_hex(INKORB_COL_IP), 0);

    out->divider = lv_obj_create(out->capsule);
    lv_obj_set_size(out->divider, 1, 22);
    lv_obj_set_style_bg_color(out->divider, lv_color_hex(INKORB_COL_DIV), 0);
    lv_obj_set_style_bg_opa(out->divider, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(out->divider, 0, 0);
    lv_obj_set_style_radius(out->divider, 0, 0);
    lv_obj_set_style_pad_all(out->divider, 0, 0);
    lv_obj_clear_flag(out->divider, LV_OBJ_FLAG_SCROLLABLE);

    out->ip_label = make_label(out->capsule, INKORB_FONT_IP, lv_color_hex(INKORB_COL_IP), 0);
    lv_label_set_text(out->ip_label, "--");
    lv_obj_set_style_text_color(out->ip_label, lv_color_hex(INKORB_COL_IP), 0);
    lv_obj_set_flex_grow(out->ip_label, 1);
    lv_label_set_long_mode(out->ip_label, LV_LABEL_LONG_DOT);

    /* Green battery outline + proportional fill, grouped as one flex item. */
    out->battery_icon = lv_obj_create(out->capsule);
    lv_obj_remove_style_all(out->battery_icon);
    lv_obj_set_size(out->battery_icon, 27, 16);
    lv_obj_clear_flag(out->battery_icon, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *body = lv_obj_create(out->battery_icon);
    lv_obj_remove_style_all(body);
    lv_obj_set_pos(body, 0, 1);
    lv_obj_set_size(body, 24, 14);
    lv_obj_set_style_radius(body, 3, 0);
    lv_obj_set_style_border_width(body, 2, 0);
    lv_obj_set_style_border_color(body, lv_color_hex(INKORB_COL_BAT), 0);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *tip = lv_obj_create(out->battery_icon);
    lv_obj_remove_style_all(tip);
    lv_obj_set_pos(tip, 24, 5);
    lv_obj_set_size(tip, 3, 6);
    lv_obj_set_style_radius(tip, 1, 0);
    lv_obj_set_style_bg_color(tip, lv_color_hex(INKORB_COL_BAT), 0);
    lv_obj_set_style_bg_opa(tip, LV_OPA_COVER, 0);
    out->battery_fill = lv_obj_create(out->battery_icon);
    lv_obj_remove_style_all(out->battery_fill);
    lv_obj_set_pos(out->battery_fill, 3, 4);
    lv_obj_set_size(out->battery_fill, 18, 8);
    lv_obj_set_style_radius(out->battery_fill, 1, 0);
    lv_obj_set_style_bg_color(out->battery_fill, lv_color_hex(INKORB_COL_BAT), 0);
    lv_obj_set_style_bg_opa(out->battery_fill, LV_OPA_COVER, 0);
    lv_obj_clear_flag(out->battery_fill, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    out->battery_label = make_label(out->capsule, &lv_font_montserrat_14,
                                   lv_color_hex(INKORB_COL_BAT), 0);
    lv_obj_set_width(out->battery_label, 52); /* room for the charging bolt */
    inkorb_home_set_battery(out, -1);

    /* Rim arcs top and bottom; the white settings gear sits inside the bottom one. */
    if (rim_images()) {
        rim_arc(scr, &s_rim_img[0], RIM_TOP);
        rim_arc(scr, &s_rim_img[1], 466 - RIM_TOP - RIM_H);
    }

    out->settings_button = lv_button_create(scr);
    lv_obj_remove_style_all(out->settings_button);
    lv_obj_set_size(out->settings_button, 76, 52);
    lv_obj_align(out->settings_button, LV_ALIGN_CENTER, 0, (int32_t)GEAR_Y - 4);
    lv_obj_set_style_radius(out->settings_button, 26, 0);
    lv_obj_set_style_bg_color(out->settings_button, lv_color_hex(0x16324C), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(out->settings_button, LV_OPA_50, LV_STATE_PRESSED);
    lv_obj_set_ext_click_area(out->settings_button, 12);
    lv_obj_t *gear = lv_label_create(out->settings_button);
    lv_obj_set_style_text_font(gear, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(gear, lv_color_white(), 0);
    lv_label_set_text(gear, LV_SYMBOL_SETTINGS);
    lv_obj_align(gear, LV_ALIGN_CENTER, 0, 4);
}

static void set_fill_width(void *obj, int32_t width)
{
    lv_obj_set_width(obj, width);
}

static void render_battery(inkorb_home_t *ui)
{
    int percent = ui->battery_percent;
    bool valid = percent >= 0 && percent <= 100;
    const char *bolt = ui->battery_plugged ? LV_SYMBOL_CHARGE : "";
    if (valid) lv_label_set_text_fmt(ui->battery_label, "%s%d%%", bolt, percent);
    else lv_label_set_text_fmt(ui->battery_label, "%s--%%", bolt);
    int width = valid ? (18 * percent + 50) / 100 : 0;
    lv_obj_set_style_opa(ui->battery_icon, valid ? LV_OPA_COVER : LV_OPA_50, 0);
    lv_anim_delete(ui->battery_fill, set_fill_width);
    if (valid && ui->battery_charging && width < 18) {
        /* "Filling up" sweep; only the small icon redraws. */
        lv_obj_remove_flag(ui->battery_fill, LV_OBJ_FLAG_HIDDEN);
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, ui->battery_fill);
        lv_anim_set_exec_cb(&a, set_fill_width);
        lv_anim_set_values(&a, width > 2 ? width : 2, 18);
        lv_anim_set_duration(&a, 1400);
        lv_anim_set_repeat_delay(&a, 300);
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
        lv_anim_start(&a);
    } else if (width > 0) {
        lv_obj_set_width(ui->battery_fill, width);
        lv_obj_remove_flag(ui->battery_fill, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(ui->battery_fill, LV_OBJ_FLAG_HIDDEN);
    }
}

void inkorb_home_set_battery(inkorb_home_t *ui, int percent)
{
    if (!ui || !ui->battery_label || !ui->battery_fill) return;
    ui->battery_percent = percent;
    render_battery(ui);
}

void inkorb_home_set_charging(inkorb_home_t *ui, bool plugged, bool charging)
{
    if (!ui || !ui->battery_label || !ui->battery_fill) return;
    if (ui->battery_plugged == plugged && ui->battery_charging == charging) return;
    ui->battery_plugged = plugged;
    ui->battery_charging = charging;
    render_battery(ui);
}

void inkorb_home_set_demo_clock(inkorb_home_t *ui, const char *time_str, const char *date_str)
{
    if (!ui) {
        return;
    }
    if (ui->time_label && time_str) {
        lv_label_set_text(ui->time_label, time_str);
        /* v3i: glyph ghosts off; mic+breathe bloom under digits. */
        sync_glow_text(ui->time_glow, 8, time_str);
        lv_obj_align(ui->time_label, LV_ALIGN_CENTER, 0, Y_TIME);
        if (ui->time_bloom) {
            lv_obj_align(ui->time_bloom, LV_ALIGN_CENTER, 0, Y_TIME);
        }
    }
    if (ui->date_label && date_str) {
        lv_label_set_text(ui->date_label, date_str);
        lv_obj_align(ui->date_label, LV_ALIGN_CENTER, 0, Y_DATE);
    }
    if (ui->weekday_label) {
        char wd[16];
        weekday_from_date(wd, sizeof(wd), date_str);
        lv_label_set_text(ui->weekday_label, wd);
        lv_obj_align(ui->weekday_label, LV_ALIGN_CENTER, 0, Y_WEEKDAY);
    }
}

void inkorb_home_set_ip(inkorb_home_t *ui, const char *ip_or_status)
{
    if (!ui || !ui->ip_label) {
        return;
    }
    lv_label_set_text(ui->ip_label, ip_or_status ? ip_or_status : "WiFi...");
    lv_obj_set_style_text_color(ui->ip_label, lv_color_hex(INKORB_COL_IP), 0);
}
