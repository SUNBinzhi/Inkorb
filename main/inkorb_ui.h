#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    lv_obj_t *title;
    lv_obj_t *title_glow[3];
    lv_obj_t *time_bloom;
    lv_obj_t *time_label;
    lv_obj_t *time_glow[8];
    lv_obj_t *date_label;
    lv_obj_t *weekday_label;
    lv_obj_t *capsule;
    lv_obj_t *wifi_icon;
    lv_obj_t *divider;
    lv_obj_t *ip_label;
    lv_obj_t *battery_icon;
    lv_obj_t *battery_fill;
    lv_obj_t *battery_label;
    lv_obj_t *settings_button;
    int battery_percent;
    bool battery_plugged, battery_charging;
} inkorb_home_t;

/* Overlay roots (chat, Wi-Fi, settings, usage) carry this flag so home-screen
 * gestures can tell they started inside a page. */
#define INKORB_OVERLAY_FLAG LV_OBJ_FLAG_USER_1

void inkorb_home_create(inkorb_home_t *out);
void inkorb_home_set_demo_clock(inkorb_home_t *ui, const char *time_str, const char *date_str);
void inkorb_home_set_ip(inkorb_home_t *ui, const char *ip_or_status);
/* Negative means unavailable/not connected; never invent a percentage. */
void inkorb_home_set_battery(inkorb_home_t *ui, int percent);
/* Bolt before the percentage while plugged; the fill animates while charging. */
void inkorb_home_set_charging(inkorb_home_t *ui, bool plugged, bool charging);

#ifdef __cplusplus
}
#endif
