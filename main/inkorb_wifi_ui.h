#pragma once
#include "inkorb_ui.h"
#include <stdbool.h>

/* Wi-Fi settings overlay on top of the home screen.
 * setup=true opens straight into "add a network with your phone". */
void inkorb_wifi_ui_init(inkorb_home_t *home);
void inkorb_wifi_ui_open(bool setup);
