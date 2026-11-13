#include "inkorb_local_tts.h"
#include "esp_tts.h"
#include "esp_tts_voice_template.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdlib.h>
#include <math.h>

static const char *TAG="inkorb_local_tts";
static esp_tts_handle_t s_tts;
static esp_tts_voice_t *s_voice;
extern const uint8_t voice_start[] asm("_binary_voice_xiaole_dat_start");
extern const uint8_t voice_end[] asm("_binary_voice_xiaole_dat_end");

bool inkorb_local_tts_synthesize(const char *text,
    bool (*pcm)(const int16_t *samples,size_t count,void *context),void *context)
{
    TickType_t start=xTaskGetTickCount();
    if(!s_tts) {
        /* The upstream descriptor has no audio samples until its data bank
         * is supplied. Embed the exact official bank in the existing app. */
        if(voice_end-voice_start!=2938039){ESP_LOGE(TAG,"voice bank size mismatch");return false;}
        s_voice=esp_tts_voice_set_init(&esp_tts_voice_template,(void *)voice_start);
        if(!s_voice){ESP_LOGE(TAG,"voice bank initialization failed");return false;}
        s_tts=esp_tts_create(s_voice);
        if(!s_tts){ESP_LOGE(TAG,"engine allocation failed");esp_tts_voice_set_free(s_voice);s_voice=NULL;return false;}
        ESP_LOGI(TAG,"ESP-TTS voice=%s format=%s, local 16k PCM, no cloud TTS",
                 s_voice->voice_name,s_voice->format);
    }
    esp_tts_stream_reset(s_tts);
    if(!esp_tts_parse_chinese(s_tts,text)) {
        ESP_LOGW(TAG,"Chinese text parse failed");return false;
    }
    bool first=true,ok=true;
    size_t samples=0;
    uint64_t energy=0;unsigned peak=0;
    for(;;) {
        int count=0;
        int16_t *data=esp_tts_stream_play(s_tts,&count,3);
        if(count<=0)break;
        if(!data){ok=false;break;}
        if(first) {
            ESP_LOGI(TAG,"first local PCM after %u ms",(unsigned)pdTICKS_TO_MS(xTaskGetTickCount()-start));
            first=false;
        }
        for(int i=0;i<count;i++) {
            int32_t v=data[i];unsigned magnitude=v<0?-v:v;
            if(magnitude>peak)peak=magnitude;
            energy+=(int64_t)v*v;
        }
        if(!pcm(data,(size_t)count,context)){ok=false;break;}
        samples+=count;
        /* Let UI and mic tasks run between synthesis batches. */
        vTaskDelay(1);
    }
    esp_tts_stream_reset(s_tts);
    ESP_LOGI(TAG,"local synth done, samples=%u internal=%u PSRAM=%u",
             (unsigned)samples,(unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    ESP_LOGI(TAG,"local PCM peak=%u RMS=%u",peak,samples?(unsigned)sqrt((double)energy/samples):0);
    return ok && !first;
}
