#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Start ES7210 mic capture task (no-op / fail-soft on sim). */
bool inkorb_mic_start(void);

/** Smoothed level in [0,1]. 0 when mic unavailable. */
float inkorb_mic_level(void);

bool inkorb_mic_ok(void);
bool inkorb_mic_record_start(void);
const uint8_t *inkorb_mic_record_stop(size_t *bytes);
bool inkorb_speaker_begin(void);
bool inkorb_speaker_write(const void *pcm, size_t bytes);
/* User volume 0-100; 100 is the tuned default loudness, each step -0.3 dB, 0 mutes. */
void inkorb_speaker_set_volume(int level);
/* Screen-off power saving: close the ES7210/I2S input while inactive; it is
 * reopened (~tens of ms) when activated again. */
void inkorb_mic_set_active(bool active);
void inkorb_speaker_end(void);

#ifdef __cplusplus
}
#endif
