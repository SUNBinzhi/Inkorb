#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef struct inkorb_duplex inkorb_duplex_t;
/* PCM is 22050Hz signed 16-bit mono; callback must copy before returning. */
inkorb_duplex_t *inkorb_duplex_open(bool (*audio)(const uint8_t *,size_t,void *),void *context);
bool inkorb_duplex_text(inkorb_duplex_t *,const char *text);
bool inkorb_duplex_finish(inkorb_duplex_t *);
/* Barge-in: makes any pending wait return false within ~100 ms. */
void inkorb_duplex_abort(bool abort);
void inkorb_duplex_close(inkorb_duplex_t *);
