#include "inkorb_duplex_tts.h"
#include "voice_cloud_config.h"
#ifndef INKORB_VOLCENGINE_KEY
#define INKORB_VOLCENGINE_KEY ""
#endif
#include "esp_websocket_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "esp_log.h"
#include "freertos/event_groups.h"
#include "cJSON.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#define FRAME_CAP 131072
#define CONNECTED BIT0
#define CONNECTION_READY BIT1
#define SESSION_READY BIT2
#define SESSION_DONE BIT3
#define FAILED BIT4
static const char *TAG="inkorb_duplex";
struct inkorb_duplex {
    esp_websocket_client_handle_t ws;
    EventGroupHandle_t events;
    uint8_t *rx;
    size_t received,total,frame_base;
    char session[40];
    bool (*audio)(const uint8_t *,size_t,void *);
    void *context;
    unsigned packets;
};
static uint32_t be32(const uint8_t *p){return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];}
static void put32(uint8_t *p,uint32_t n){p[0]=n>>24;p[1]=n>>16;p[2]=n>>8;p[3]=n;}
static void fail(inkorb_duplex_t *d,unsigned code)
{
    ESP_LOGW(TAG,"stream event failure code=%u",code);
    xEventGroupSetBits(d->events,FAILED);
}
static void parse(inkorb_duplex_t *d)
{
    const uint8_t *b=d->rx;size_t n=d->total;
    if(n<8 || (b[0]>>4)!=1 || (b[2]&15)){fail(d,1);return;}
    unsigned type=b[1]>>4,flags=b[1]&15;
    size_t at=(b[0]&15)*4;
    if(at<4 || at+4>n){fail(d,2);return;}
    if(type==15){fail(d,be32(b+at));return;}
    if(!(flags&4))return;
    unsigned event=be32(b+at);at+=4;
    if(event==50){xEventGroupSetBits(d->events,CONNECTION_READY);return;}
    if(event==51 || event==153){fail(d,event);return;}
    if(event==52)return;
    if(at+4>n){fail(d,3);return;}
    size_t sid=be32(b+at);at+=4;
    if(sid>n-at){fail(d,4);return;}at+=sid;
    if(event==150){xEventGroupSetBits(d->events,SESSION_READY);return;}
    if(event==152){xEventGroupSetBits(d->events,SESSION_DONE);return;}
    if(event!=352)return;
    if(type!=11 || (b[2]>>4)!=0){fail(d,12);return;}
    if(at+4>n){fail(d,5);return;}
    size_t bytes=be32(b+at);at+=4;
    if(bytes>n-at || (bytes&1)){fail(d,6);return;}
    if(bytes && !d->audio(b+at,bytes,d->context)){fail(d,7);return;}
    d->packets++;
}
static void event(void *arg,esp_event_base_t base,int32_t id,void *payload)
{
    (void)base;inkorb_duplex_t *d=arg;
    esp_websocket_event_data_t *e=payload;
    if(id==WEBSOCKET_EVENT_CONNECTED){xEventGroupSetBits(d->events,CONNECTED);return;}
    if(id==WEBSOCKET_EVENT_ERROR){fail(d,e?e->error_handle.esp_ws_handshake_status_code:8);return;}
    if(id==WEBSOCKET_EVENT_DISCONNECTED){
        if(!(xEventGroupGetBits(d->events)&SESSION_DONE))fail(d,9);
        return;
    }
    if(id!=WEBSOCKET_EVENT_DATA || (e->op_code!=2 && e->op_code!=0))return;
    if(e->payload_len<0 || e->data_len<0 || e->payload_offset<0 || e->payload_len>FRAME_CAP){fail(d,10);return;}
    if(e->payload_offset==0) {
        if(e->op_code==2)d->received=0;
        d->frame_base=d->received;
        if((size_t)e->payload_len>FRAME_CAP-d->frame_base){fail(d,10);return;}
        d->total=d->frame_base+e->payload_len;
    }
    if(d->frame_base+(size_t)e->payload_offset!=d->received || (size_t)e->data_len>d->total-d->received){fail(d,11);return;}
    memcpy(d->rx+d->received,e->data_ptr,e->data_len);d->received+=e->data_len;
    if(d->received==d->total && e->fin){parse(d);d->received=0;}
}
static bool send_event(inkorb_duplex_t *d,unsigned evt,cJSON *body,bool session)
{
    if(xEventGroupGetBits(d->events)&FAILED)return false;
    char *json=body?cJSON_PrintUnformatted(body):NULL;
    if(body && !json)return false;
    const char *text=json?json:"{}";size_t len=strlen(text),sid=session?strlen(d->session):0;
    size_t bytes=12+len+(session?4+sid:0);
    uint8_t *b=heap_caps_malloc(bytes,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(!b){cJSON_free(json);return false;}
    b[0]=0x11;b[1]=0x14;b[2]=0x10;b[3]=0;put32(b+4,evt);size_t at=8;
    if(session){put32(b+at,sid);at+=4;memcpy(b+at,d->session,sid);at+=sid;}
    put32(b+at,len);memcpy(b+at+4,text,len);
    int sent=esp_websocket_client_send_bin(d->ws,(const char *)b,bytes,pdMS_TO_TICKS(3000));
    free(b);cJSON_free(json);return sent==(int)bytes;
}
static cJSON *request(unsigned evt,const char *text)
{
    cJSON *b=cJSON_CreateObject(),*u=cJSON_AddObjectToObject(b,"user");
    cJSON_AddStringToObject(u,"uid","inkorb-device");cJSON_AddNumberToObject(b,"event",evt);
    cJSON_AddStringToObject(b,"namespace","BidirectionalTTS");
    cJSON *p=cJSON_AddObjectToObject(b,"req_params");
    cJSON_AddStringToObject(p,"speaker","zh_female_vv_uranus_bigtts");
    if(text)cJSON_AddStringToObject(p,"text",text);
    cJSON *a=cJSON_AddObjectToObject(p,"audio_params");
    cJSON_AddStringToObject(a,"format","pcm");cJSON_AddNumberToObject(a,"sample_rate",22050);
    cJSON_AddNumberToObject(a,"speech_rate",0);cJSON_AddNumberToObject(a,"loudness_rate",0);
    return b;
}
static volatile bool s_abort;
void inkorb_duplex_abort(bool abort) { s_abort=abort; }
/* Wait in 100 ms slices so a barge-in can end a long wait early. */
static bool wait_for(inkorb_duplex_t *d,EventBits_t bit,unsigned ms)
{
    for(unsigned waited=0;;waited+=100) {
        if(s_abort)return false;
        unsigned slice=ms-waited<100?ms-waited:100;
        EventBits_t b=xEventGroupWaitBits(d->events,bit|FAILED,pdFALSE,pdFALSE,pdMS_TO_TICKS(slice));
        if(b&(bit|FAILED))return (b&bit) && !(b&FAILED);
        if(waited+slice>=ms)return false;
    }
}
inkorb_duplex_t *inkorb_duplex_open(bool (*audio)(const uint8_t *,size_t,void *),void *context)
{
    /* The SDK transport can print an entire outbound header on write errors.
     * Keep credentials out of logs; our handler reports numeric failures. */
    esp_log_level_set("transport_ws",ESP_LOG_NONE);
    inkorb_duplex_t *d=heap_caps_calloc(1,sizeof(*d),MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
    if(!d)return NULL;
    d->events=xEventGroupCreate();d->rx=heap_caps_malloc(FRAME_CAP,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    d->audio=audio;d->context=context;
    uint8_t uuid[16];esp_fill_random(uuid,sizeof(uuid));uuid[6]=(uuid[6]&15)|0x40;uuid[8]=(uuid[8]&63)|0x80;
    unsigned pos=0;
    for(unsigned i=0;i<16;i++){if(i==4||i==6||i==8||i==10)d->session[pos++]='-';snprintf(d->session+pos,3,"%02x",uuid[i]);pos+=2;}
    if(!d->events || !d->rx)goto error;
    char headers[700];snprintf(headers,sizeof(headers),"X-Api-Key: %s\r\nX-Api-Resource-Id: seed-tts-2.0\r\nX-Api-Connect-Id: %s\r\n",INKORB_VOLCENGINE_KEY,d->session);
    esp_websocket_client_config_t cfg={
        .uri="wss://openspeech.bytedance.com/api/v3/tts/bidirection",.headers=headers,
        .crt_bundle_attach=esp_crt_bundle_attach,.disable_auto_reconnect=true,
        .task_stack=8192,.task_prio=3,.buffer_size=4096,.network_timeout_ms=10000,
        .ping_interval_sec=10,.keep_alive_enable=true,
    };
    d->ws=esp_websocket_client_init(&cfg);if(!d->ws)goto error;
    esp_websocket_register_events(d->ws,WEBSOCKET_EVENT_ANY,event,d);
    if(esp_websocket_client_start(d->ws)!=ESP_OK || !wait_for(d,CONNECTED,10000))goto error;
    if(!send_event(d,1,NULL,false) || !wait_for(d,CONNECTION_READY,10000))goto error;
    cJSON *b=request(100,NULL);bool sent=send_event(d,100,b,true);cJSON_Delete(b);
    if(!sent || !wait_for(d,SESSION_READY,10000))goto error;
    ESP_LOGI(TAG,"bidirectional TTS session ready, PCM 22050 mono");return d;
error:
    inkorb_duplex_close(d);return NULL;
}
bool inkorb_duplex_text(inkorb_duplex_t *d,const char *text)
{
    if(!d || !text[0])return d!=NULL;
    cJSON *b=request(200,text);bool ok=send_event(d,200,b,true);cJSON_Delete(b);return ok;
}
bool inkorb_duplex_finish(inkorb_duplex_t *d)
{
    if(!d || !send_event(d,102,NULL,true))return false;
    bool ok=wait_for(d,SESSION_DONE,30000);
    ESP_LOGI(TAG,"stream session finished: ok=%d audio_packets=%u",ok,d->packets);return ok;
}
void inkorb_duplex_close(inkorb_duplex_t *d)
{
    if(!d)return;
    if(d->ws){
        if(esp_websocket_client_is_connected(d->ws))send_event(d,2,NULL,false);
        esp_websocket_client_stop(d->ws);esp_websocket_client_destroy(d->ws);
    }
    if(d->events)vEventGroupDelete(d->events);
    free(d->rx);free(d);
}
