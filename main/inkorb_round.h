#pragma once
#include "lvgl.h"
#include <stdbool.h>

/* Building blocks for pages laid out around the 466 px round glass: things
 * are placed by angle and radius from the centre instead of in columns.
 * Angles are degrees clockwise from 3 o'clock (LVGL's convention): 270 is the
 * top, 90 the bottom. */

#define ROUND_WHITE  0xFFFFFF
#define ROUND_BLUE   0x7EC8FF
#define ROUND_NEON   0x4EB4FF
#define ROUND_MUTED  0x89A0B4
#define ROUND_FAINT  0x5D7186
#define ROUND_CARD   0x0B1D2E
#define ROUND_TRACK  0x16324C
#define ROUND_WARN   0xFFB45E
#define ROUND_OK     0x62E69A

/* Full-screen black page on the active screen, flagged as an overlay, that
 * calls on_back when swiped right (NULL: no swipe-back, e.g. settings). */
lv_obj_t *round_page(lv_event_cb_t on_back);

/* Label in the CJK font (falls back to Montserrat for Latin). */
lv_obj_t *round_text(lv_obj_t *parent, uint32_t color);

/* Centre `obj` on the point at (angle, radius) from the screen centre. */
void round_place(lv_obj_t *obj, int32_t angle, int32_t radius);

/* Curved title along the top rim; tapping it calls on_tap (e.g. back). */
lv_obj_t *round_title(lv_obj_t *page, const char *text, lv_event_cb_t on_tap);

/* Round icon button at (angle, radius) with a caption underneath. `icon` is a
 * Montserrat symbol/text, or NULL when the caller draws its own glyph.
 * Returns the button; *caption (optional) receives the caption label. */
lv_obj_t *round_button(lv_obj_t *page, const char *icon, const char *caption_text, int32_t angle,
                       int32_t radius, int32_t size, lv_event_cb_t cb, void *user_data, lv_obj_t **caption);

/* Arc along the rim covering [start, end] degrees (clockwise), used as a
 * slider when `knob` is true. Value 0 sits at `start` unless reversed. */
lv_obj_t *round_rim_arc(lv_obj_t *page, int32_t start, int32_t end, int32_t width, uint32_t color,
                        bool knob, bool reverse);

/* Small sun glyph (brightness), drawn since Montserrat has none. */
lv_obj_t *round_sun_icon(lv_obj_t *parent, uint32_t color);
