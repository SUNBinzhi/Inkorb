#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Create one lv_image that renders text as rounded pixel blocks (5x7 glyphs).
 * Rasterizes into a reused A8 buffer (PSRAM on device); no per-dot lv_obj.
 * cell/gap/radius in px; color via image recolor.
 */
lv_obj_t *inkorb_pixel_label_create(lv_obj_t *parent, int cell, int gap, int radius,
                                    lv_color_t color);

/** Soft-redraw A8 buffer + invalidate (no child create/destroy). */
void inkorb_pixel_label_set_text(lv_obj_t *label, const char *text);

#ifdef __cplusplus
}
#endif
