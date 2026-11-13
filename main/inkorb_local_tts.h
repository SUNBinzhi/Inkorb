#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
/* Output is mono signed 16-bit PCM at 16000Hz. No network requests. */
bool inkorb_local_tts_synthesize(const char *text,
    bool (*pcm)(const int16_t *samples,size_t count,void *context),void *context);
