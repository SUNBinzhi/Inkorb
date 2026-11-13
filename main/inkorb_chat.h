#pragma once
#include "lvgl.h"
#include <stdint.h>
typedef struct {
    lv_obj_t *root, *status, *question, *answer, *record_button;
    lv_obj_t *ring, *dot, *body, *hint, *bubble, *sources, *mic, *bars, *button_label;
    lv_obj_t *bar[5];
    lv_timer_t *timer;
    int mode;
    uint32_t mode_started;
} inkorb_chat_t;
void inkorb_chat_create(inkorb_chat_t *ui, lv_event_cb_t press, lv_event_cb_t release,
                        lv_event_cb_t back);
void inkorb_chat_status(inkorb_chat_t *ui, const char *text);
void inkorb_chat_text(inkorb_chat_t *ui, const char *question, const char *answer);
