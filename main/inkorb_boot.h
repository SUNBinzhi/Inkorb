#pragma once
#include "lvgl.h"

typedef struct {
    lv_obj_t *orbit;            /* dim full-circle track */
    lv_obj_t *progress;         /* stage progress, clockwise from the top */
    lv_obj_t *comet;            /* head + fading tail, drawn in one pass */
    int32_t comet_head;         /* degrees clockwise from the top */
    lv_opa_t comet_opa;
    lv_area_t comet_seg[15];    /* per-segment bounds, refreshed each frame */
    lv_obj_t *status;
    lv_obj_t *offline_button, *wifi_button;
    lv_timer_t *timer;
    float shown, target, cap;   /* progress fractions */
    uint32_t started, ready_at;
    bool ready;
} inkorb_boot_t;

void inkorb_boot_create(inkorb_boot_t *ui, lv_event_cb_t offline_cb, lv_event_cb_t wifi_cb, void *user_data);
void inkorb_boot_set_status(inkorb_boot_t *ui, const char *text, bool ready);
void inkorb_boot_allow_offline(inkorb_boot_t *ui);
