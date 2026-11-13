#pragma once
#include "inkorb_ui.h"
bool inkorb_voice_start(inkorb_home_t *home);
void inkorb_voice_open(void);
void inkorb_voice_button(bool pressed);
/* True while recording, waiting for or playing an answer. */
bool inkorb_voice_busy(void);
