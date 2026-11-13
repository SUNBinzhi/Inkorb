#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Usage the device measures itself, per day and all-time, saved in NVS
 * (namespace inkorb_usage), plus the DeepSeek account balance fetched from
 * its official endpoint. SiliconFlow and Volcengine have no balance endpoint
 * usable with the device's keys, so only local usage is shown for them. */
typedef struct {
    uint32_t turns;          /* completed voice turns */
    uint32_t llm_in, llm_out;/* DeepSeek prompt / completion tokens */
    uint32_t searches;       /* DeepSeek web searches */
    uint32_t asr_ms;         /* audio sent to SiliconFlow ASR */
    uint32_t tts_chars;      /* characters sent to Volcengine TTS */
} inkorb_usage_t;

typedef struct {
    bool valid, available;
    char currency[8];
    char total[16], granted[16], topped[16];
    uint32_t fetched_at;     /* lv_tick-independent: seconds since boot */
    bool fetching;
    char error[48];
} inkorb_balance_t;

void inkorb_usage_load(void);
void inkorb_usage_add_llm(uint32_t prompt_tokens, uint32_t completion_tokens);
void inkorb_usage_add_search(void);
void inkorb_usage_add_asr(uint32_t ms);
void inkorb_usage_add_tts(uint32_t chars);
void inkorb_usage_add_turn(void);      /* also saves to NVS */
void inkorb_usage_get(inkorb_usage_t *today, inkorb_usage_t *total);
/* Average per finished day of use (excluding today); false before the first. */
bool inkorb_usage_average(inkorb_usage_t *avg);

/* Starts a background fetch of the DeepSeek balance; result via _balance(). */
void inkorb_usage_refresh_balance(void);
void inkorb_usage_balance(inkorb_balance_t *out);
/* Seconds since boot, the clock fetched_at uses. */
uint32_t inkorb_usage_now(void);
