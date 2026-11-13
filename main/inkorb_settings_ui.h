#pragma once
#include "inkorb_ui.h"

/* Settings overlay on top of the home screen: volume, brightness, auto
 * screen-off, Wi-Fi, Inkorb's memory, about this device, restart. */
void inkorb_settings_ui_init(inkorb_home_t *home);
void inkorb_settings_ui_open(void);
/* 0 main, 1 memory, 2 about (sim previews jump straight to a sub-page). */
void inkorb_settings_ui_open_page(int page);
