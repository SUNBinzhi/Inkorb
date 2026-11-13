#include "inkorb_mic.h"

#include <math.h>
#include <string.h>

#ifndef ESP_PLATFORM
/* Host sim: no mic hardware; INKORB_SIM_LEVEL fakes a level for previews. */
#include <stdlib.h>
bool inkorb_mic_start(void) { return false; }
float inkorb_mic_level(void) { const char *v = getenv("INKORB_SIM_LEVEL"); return v ? (float)atof(v) : 0.0f; }
bool inkorb_mic_ok(void) { return false; }
bool inkorb_mic_record_start(void) { return false; }
const uint8_t *inkorb_mic_record_stop(size_t *bytes) { *bytes = 0; return NULL; }
bool inkorb_speaker_begin(void) { return false; }
bool inkorb_speaker_write(const void *pcm, size_t bytes) { (void)pcm; (void)bytes; return false; }
void inkorb_speaker_end(void) {}
void inkorb_speaker_set_volume(int level) { (void)level; }
void inkorb_mic_set_active(bool active) { (void)active; }
#else

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "bsp/esp-bsp.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"

static const char *TAG = "inkorb_mic";

static esp_codec_dev_handle_t s_mic;
static volatile float s_level;
static volatile bool s_ok;
static esp_codec_dev_handle_t s_speaker;
static uint8_t *s_record_buffer;
static size_t s_record_bytes;
static bool s_recording;
static portMUX_TYPE s_record_lock = portMUX_INITIALIZER_UNLOCKED;
#define RECORD_CAPACITY (22050U * 2U * 12U)

bool inkorb_mic_record_start(void)
{
    if (!s_ok) return false;
    if (!s_record_buffer) {
        s_record_buffer = heap_caps_malloc(RECORD_CAPACITY, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_record_buffer) return false;
    }
    taskENTER_CRITICAL(&s_record_lock);
    s_record_bytes = 0;
    s_recording = true;
    taskEXIT_CRITICAL(&s_record_lock);
    return true;
}

const uint8_t *inkorb_mic_record_stop(size_t *bytes)
{
    taskENTER_CRITICAL(&s_record_lock);
    s_recording = false;
    *bytes = s_record_bytes;
    taskEXIT_CRITICAL(&s_record_lock);
    return s_record_buffer;
}

bool inkorb_speaker_begin(void)
{
    if (!s_speaker) {
        s_speaker = bsp_audio_codec_speaker_init();
        if (!s_speaker) return false;
        esp_codec_dev_sample_info_t fs = {
            .bits_per_sample = 16, .channel = 1, .sample_rate = 22050,
        };
        if (esp_codec_dev_open(s_speaker, &fs) != ESP_CODEC_DEV_OK) {
            esp_codec_dev_delete(s_speaker);
            s_speaker = NULL;
            return false;
        }
        esp_codec_dev_set_out_vol(s_speaker, 100.0f);
    }
    esp_codec_dev_set_out_mute(s_speaker, false);
    return true;
}

static volatile int32_t s_volume_q15 = 32768; /* 1.0 at volume 100 */

void inkorb_speaker_set_volume(int level)
{
    if (level < 0) level = 0;
    if (level > 100) level = 100;
    s_volume_q15 = level ? (int32_t)(32768.0f * powf(10.0f, (level - 100) * 0.3f / 20.0f)) : 0;
}

bool inkorb_speaker_write(const void *pcm, size_t bytes)
{
    if (!s_speaker) return false;
    int32_t q15 = s_volume_q15;
    if (q15 == 32768) return esp_codec_dev_write(s_speaker, (void *)pcm, (int)bytes) == ESP_CODEC_DEV_OK;
    /* Scale on the way out so every voice path (cloud, local, bridge) follows it. */
    const int16_t *in = pcm;
    int16_t out[256];
    for (size_t done = 0; done < bytes / 2;) {
        size_t n = bytes / 2 - done;
        if (n > 256) n = 256;
        for (size_t i = 0; i < n; i++) out[i] = (int16_t)(((int32_t)in[done + i] * q15) >> 15);
        if (esp_codec_dev_write(s_speaker, out, (int)(n * 2)) != ESP_CODEC_DEV_OK) return false;
        done += n;
    }
    return true;
}

void inkorb_speaker_end(void)
{
    if (s_speaker) esp_codec_dev_set_out_mute(s_speaker, true);
}

bool inkorb_mic_ok(void)
{
    return s_ok;
}

float inkorb_mic_level(void)
{
    return s_level;
}

static float rms_i16(const int16_t *s, int n)
{
    if (n <= 0) {
        return 0.0f;
    }
    double acc = 0.0;
    for (int i = 0; i < n; i++) {
        double v = (double)s[i];
        acc += v * v;
    }
    return (float)sqrt(acc / (double)n);
}

static volatile bool s_mic_active = true;

void inkorb_mic_set_active(bool active)
{
    s_mic_active = active;
}

static esp_codec_dev_sample_info_t s_mic_fs = {
    .bits_per_sample = 16, .channel = 1, .channel_mask = 0, .sample_rate = 22050, .mclk_multiple = 0,
};

static void mic_task(void *arg)
{
    (void)arg;
    enum { N = 512 };
    int16_t *buf = heap_caps_malloc(N * sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!buf) {
        buf = malloc(N * sizeof(int16_t));
    }
    if (!buf) {
        ESP_LOGE(TAG, "mic buffer alloc failed");
        s_ok = false;
        vTaskDelete(NULL);
        return;
    }

    float ema = 0.0f;
    const float attack = 0.55f; /* v3j faster attack */
    const float release = 0.18f; /* a bit snappier release */
    /* Empiric: quiet room ~200-800, speech peaks often 2k-8k at 30dB gain. */
    const float noise_floor = 140.0f; /* v3n: retain softer near-field speech */
    const float loud = 3200.0f;

    bool open = true;
    while (1) {
        if (!s_mic_active) {
            /* Screen off: stop I2S capture and the ADC entirely. */
            if (open) { esp_codec_dev_close(s_mic); open = false; s_level = 0.0f; ema = 0.0f; }
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        if (!open) {
            if (esp_codec_dev_open(s_mic, &s_mic_fs) != ESP_CODEC_DEV_OK) { vTaskDelay(pdMS_TO_TICKS(100)); continue; }
            esp_codec_dev_set_in_gain(s_mic, 40.0f);
            open = true;
        }
        int ret = esp_codec_dev_read(s_mic, buf, N * (int)sizeof(int16_t));
        if (ret != ESP_CODEC_DEV_OK) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        taskENTER_CRITICAL(&s_record_lock);
        if (s_recording) {
            size_t bytes = N * sizeof(int16_t);
            if (bytes > RECORD_CAPACITY - s_record_bytes) bytes = RECORD_CAPACITY - s_record_bytes;
            memcpy(s_record_buffer + s_record_bytes, buf, bytes);
            s_record_bytes += bytes;
            if (s_record_bytes == RECORD_CAPACITY) s_recording = false;
        }
        taskEXIT_CRITICAL(&s_record_lock);
        float r = rms_i16(buf, N);
        float n = (r - noise_floor) / (loud - noise_floor);
        if (n < 0.0f) {
            n = 0.0f;
        }
        if (n > 1.0f) {
            n = 1.0f;
        }
        /* v3j: keep quiet levels visible (less squashing). */
        n = 0.35f * n + 0.65f * (n * n);
        float a = (n > ema) ? attack : release;
        ema = ema + (n - ema) * a;
        s_level = ema;
    }
}

bool inkorb_mic_start(void)
{
    if (s_ok) {
        return true;
    }

    s_mic = bsp_audio_codec_microphone_init();
    if (!s_mic) {
        ESP_LOGW(TAG, "bsp_audio_codec_microphone_init failed — bloom idle-only");
        return false;
    }

    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 16,
        .channel = 1,
        .channel_mask = 0,
        .sample_rate = 22050, /* match BSP I2S default */
        .mclk_multiple = 0,
    };
    int ret = esp_codec_dev_open(s_mic, &fs);
    if (ret != ESP_CODEC_DEV_OK) {
        ESP_LOGW(TAG, "esp_codec_dev_open failed (%d) — bloom idle-only", ret);
        s_mic = NULL;
        return false;
    }
    /* Match Waveshare example: useful gain on ES7210 mics. */
    esp_codec_dev_set_in_gain(s_mic, 40.0f); /* v3j louder mic */

    s_ok = true;
    s_level = 0.0f;
    BaseType_t ok = xTaskCreatePinnedToCore(mic_task, "inkorb_mic", 4096, NULL, 5, NULL, 0);
    if (ok != pdPASS) {
        ESP_LOGW(TAG, "mic task create failed");
        s_ok = false;
        return false;
    }
    ESP_LOGI(TAG, "ES7210 mic up (I2S RX 22.05kHz mono, gain 40dB)");
    return true;
}

#endif /* ESP_PLATFORM */
