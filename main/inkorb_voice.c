#include "inkorb_voice.h"
#include "inkorb_chat.h"
#include "inkorb_mic.h"
#include "inkorb_cloud.h"
#include "wifi_sta.h"
#include "voice_bridge_config.h"
#include "bsp/esp-bsp.h"
#include "esp_http_client.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "cJSON.h"
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "inkorb_voice";
enum { IDLE, RECORDING, WAITING, PLAYING };
typedef struct { const uint8_t *pcm; size_t bytes; TickType_t released; } recording_t;
static atomic_int s_state;
static atomic_bool s_server_ready;
static inkorb_home_t *s_home;
static inkorb_chat_t s_chat;
static QueueHandle_t s_queue;
static uint32_t s_record_started;
static atomic_bool s_chat_visible;
/* Set when a new press interrupts a running turn; that turn's late status and
 * text updates are dropped so they cannot overwrite the new recording. */
static atomic_bool s_barged;
static char s_wait_label[100];
static uint32_t s_wait_started, s_wait_second;
static bool s_wait_progress;

static void status(const char *value)
{
    if (atomic_load(&s_barged)) return;
    bsp_display_lock(-1);
    inkorb_chat_status(&s_chat, value);
    s_wait_progress=!strcmp(value,"正在云端识别语音") || !strcmp(value,"AI 正在思考") ||
                    !strcmp(value,"正在联网查资料") || !strcmp(value,"正在准备云端语音") || !strcmp(value,"正在本地生成语音");
    snprintf(s_wait_label,sizeof(s_wait_label),"%s",value);
    s_wait_started=lv_tick_get();s_wait_second=0;
    bsp_display_unlock();
}

static void texts(const char *question, const char *answer)
{
    if (atomic_load(&s_barged)) return;
    bsp_display_lock(-1);
    if (s_chat.root) inkorb_chat_text(&s_chat, question, answer);
    bsp_display_unlock();
}

static const char *string(cJSON *json, const char *name)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(json, name);
    return cJSON_IsString(item) ? item->valuestring : "";
}

static esp_http_client_handle_t client(const char *path, esp_http_client_method_t method)
{
    char url[240];
    snprintf(url, sizeof(url), "%s%s", INKORB_VOICE_URL, path);
    esp_http_client_config_t cfg = {
        .url = url, .method = method, .timeout_ms = 15000,
        .buffer_size = 2048, .buffer_size_tx = 1024,
        .disable_auto_redirect = true,
    };
    esp_http_client_handle_t handle = esp_http_client_init(&cfg);
    if (handle) esp_http_client_set_header(handle, "Authorization", "Bearer " INKORB_VOICE_TOKEN);
    return handle;
}

static int request(const char *path, esp_http_client_method_t method,
                   const uint8_t *data, size_t bytes, char *response, size_t capacity)
{
    response[0] = 0;
    esp_http_client_handle_t handle = client(path, method);
    if (!handle) return -1;
    esp_http_client_set_header(handle, "Content-Type", "audio/pcm");
    int code = -1;
    if (esp_http_client_open(handle, (int)bytes) != ESP_OK) goto done;
    for (size_t written = 0; written < bytes;) {
        size_t chunk = bytes - written;
        if (chunk > 4096) chunk = 4096;
        int n = esp_http_client_write(handle, (const char *)data + written, (int)chunk);
        if (n <= 0) goto done;
        written += (size_t)n;
    }
    if (esp_http_client_fetch_headers(handle) < 0) goto done;
    code = esp_http_client_get_status_code(handle);
    size_t total = 0;
    while (total + 1 < capacity) {
        int n = esp_http_client_read(handle, response + total, (int)(capacity-total-1));
        if (n < 0) { code = -1; break; }
        if (!n) break;
        total += (size_t)n;
    }
    response[total] = 0;
done:
    esp_http_client_close(handle);
    esp_http_client_cleanup(handle);
    return code;
}

static bool play(const char *job)
{
    char path[80];
    snprintf(path, sizeof(path), "/audio/%s", job);
    esp_http_client_handle_t handle = client(path, HTTP_METHOD_GET);
    if (!handle) return false;
    bool ok = false;
    if (esp_http_client_open(handle, 0) != ESP_OK) goto done;
    if (esp_http_client_fetch_headers(handle) <= 0 || esp_http_client_get_status_code(handle) != 200) goto done;
    if (!inkorb_speaker_begin()) goto done;
    char pcm[2048];
    size_t total = 0;
    while (total < 22050U * 2U * 24U) {
        /* read_response fills a chunk, preserving 16-bit sample boundaries. */
        int n = esp_http_client_read_response(handle, pcm, sizeof(pcm));
        if (n < 0 || (n & 1)) goto done;
        if (!n) { ok = true; break; }
        if (!inkorb_speaker_write(pcm, (size_t)n)) goto done;
        total += (size_t)n;
    }
    if (total >= 22050U * 2U * 24U) ok = true;
done:
    inkorb_speaker_end();
    esp_http_client_close(handle);
    esp_http_client_cleanup(handle);
    return ok;
}

static void worker(void *arg)
{
    (void)arg;
    char *response = heap_caps_malloc(8192, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!response) {
        ESP_LOGE(TAG, "response buffer allocation failed");
        vTaskDeleteWithCaps(NULL);
        return;
    }
    recording_t recording;
    while (true) {
        if (xQueueReceive(s_queue, &recording, pdMS_TO_TICKS(5000)) != pdTRUE) {
            if (inkorb_cloud_configured()) {
                if (atomic_load(&s_state) == IDLE && s_chat_visible && wifi_sta_status(NULL, 0, NULL, NULL))
                    status("按住大键或下方按钮说话");
                continue;
            }
            int code = request("/health", HTTP_METHOD_GET, NULL, 0, response, 8192);
            cJSON *json = code == 200 ? cJSON_Parse(response) : NULL;
            bool ready = json && cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(json, "ready"));
            bool previous = atomic_exchange(&s_server_ready, ready);
            if (ready && !previous) ESP_LOGI(TAG, "Mac voice bridge ready");
            cJSON_Delete(json);
            if (atomic_load(&s_state) == IDLE && s_chat_visible) {
                status(ready ? "按住大键或下方按钮说话" : "正在连接 Mac 语音服务");
            }
            continue;
        }
        if (inkorb_cloud_configured()) {
            inkorb_cloud_turn(recording.pcm, recording.bytes, recording.released, status, texts);
            /* After a barge-in the state belongs to the new recording, unless
             * nothing is recording or queued (e.g. the mic failed to start). */
            bool barged = atomic_exchange(&s_barged, false);
            if (!barged || (atomic_load(&s_state) == WAITING && !uxQueueMessagesWaiting(s_queue)))
                atomic_store(&s_state, IDLE);
            continue;
        }
        status("正在发送语音");
        int code = request("/turn", HTTP_METHOD_POST, recording.pcm, recording.bytes, response, 8192);
        cJSON *json = cJSON_Parse(response);
        char job[40] = {0};
        if (code != 202 || !json || !string(json, "job")[0]) {
            const char *error = json ? string(json, "error") : "";
            status(error[0] ? error : "连接失败，请确认 Mac 服务已启动");
            cJSON_Delete(json);
            atomic_store(&s_state, IDLE);
            continue;
        }
        ESP_LOGI(TAG, "upload accepted after %u ms, recording=%u bytes",
                 (unsigned)pdTICKS_TO_MS(xTaskGetTickCount()-recording.released), (unsigned)recording.bytes);
        snprintf(job, sizeof(job), "%s", string(json, "job"));
        cJSON_Delete(json);
        bool complete = false;
        TickType_t started = xTaskGetTickCount();
        for (int attempt = 0; attempt < 600 &&
             xTaskGetTickCount() - started < pdMS_TO_TICKS(90000); attempt++) {
            char path[80];
            snprintf(path, sizeof(path), "/jobs/%s", job);
            code = request(path, HTTP_METHOD_GET, NULL, 0, response, 8192);
            json = code == 200 ? cJSON_Parse(response) : NULL;
            if (!json) { vTaskDelay(pdMS_TO_TICKS(150)); continue; }
            const char *stage = string(json, "stage");
            const char *question = string(json, "transcript");
            const char *answer = string(json, "display_answer");
            if (!answer[0]) answer = string(json, "answer");
            if (question[0] || answer[0]) texts(question[0] ? question : NULL, answer[0] ? answer : NULL);
            if (strcmp(stage, "error") == 0) {
                status(string(json, "error"));
                complete = true;
            } else if (strcmp(stage, "done") == 0) {
                bool has_audio = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(json, "has_audio"));
                cJSON_Delete(json);
                json = NULL;
                if (has_audio) {
                    atomic_store(&s_state, PLAYING);
                    status("AI 正在回答");
                    ESP_LOGI(TAG, "reply audio starts after %u ms",
                             (unsigned)pdTICKS_TO_MS(xTaskGetTickCount()-recording.released));
                    bool played = play(job);
                    status(played ? "按住按钮，继续聊" : "回答已显示，扬声器播放失败");
                    ESP_LOGI(TAG, "turn complete, audio=%s", played ? "played" : "failed");
                } else {
                    status("回答已显示，按住按钮继续聊");
                }
                complete = true;
            } else if (strcmp(stage, "thinking") == 0) status("AI 正在思考");
            else if (strcmp(stage, "searching") == 0) status("正在联网查资料");
            else if (strcmp(stage, "speaking") == 0) status("正在准备语音回答");
            else status("正在识别你说的话");
            cJSON_Delete(json);
            if (complete) break;
            vTaskDelay(pdMS_TO_TICKS(150));
        }
        if (!complete) status("等待超时，请再试一次");
        atomic_store(&s_state, IDLE);
    }
}

static void release(lv_event_t *event)
{
    (void)event;
    if (atomic_load(&s_state) != RECORDING) return;
    recording_t recording;
    recording.released = xTaskGetTickCount();
    recording.pcm = inkorb_mic_record_stop(&recording.bytes);
    if (recording.bytes < 22050U / 2U) {
        atomic_store(&s_state, IDLE);
        inkorb_chat_status(&s_chat, "请按住按钮说话，松开发送");
        return;
    }
    atomic_store(&s_state, WAITING);
    inkorb_chat_status(&s_chat, "正在发送语音");
    if (xQueueSend(s_queue, &recording, 0) != pdTRUE) {
        atomic_store(&s_state, IDLE);
        inkorb_chat_status(&s_chat, "服务忙，请再试一次");
    }
}

static void press(lv_event_t *event)
{
    (void)event;
    int state = atomic_load(&s_state);
    if (state == WAITING && inkorb_cloud_configured() && !atomic_load(&s_barged)) {
        /* Barge-in: cut the current answer and start listening right away. */
        atomic_store(&s_barged, true);
        inkorb_cloud_cancel();
        ESP_LOGI(TAG, "barge-in: interrupting current answer");
    } else if (state != IDLE) {
        return;
    }
    if (inkorb_cloud_configured() && !wifi_sta_status(NULL, 0, NULL, NULL)) {
        inkorb_chat_status(&s_chat, "正在连接网络，请稍候再按");
        return;
    }
    if (!inkorb_mic_record_start()) {
        inkorb_chat_status(&s_chat, "麦克风尚未就绪，请稍后再试");
        return;
    }
    atomic_store(&s_state, RECORDING);
    s_record_started = lv_tick_get();
    inkorb_chat_text(&s_chat, "正在听你说话…", "");
    inkorb_chat_status(&s_chat, "正在录音，松开发送，最多12秒");
}

static void back(lv_event_t *event)
{
    (void)event;
    if (atomic_load(&s_state) == RECORDING) release(NULL);
    lv_obj_add_flag(s_chat.root, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_home->time_bloom, LV_OBJ_FLAG_HIDDEN);
    s_chat_visible = false;
    wifi_sta_release();      /* radio goes off WIFI_LINGER_S later if unused */
}

void inkorb_voice_open(void)
{
    if (!s_home || !s_queue) return;
    if (!s_chat.root) inkorb_chat_create(&s_chat, press, release, back);
    lv_obj_remove_flag(s_chat.root, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_chat.root);
    lv_obj_add_flag(s_home->time_bloom, LV_OBJ_FLAG_HIDDEN);
    if (!s_chat_visible) wifi_sta_hold();   /* the assistant needs the network */
    s_chat_visible = true;
}

void inkorb_voice_button(bool pressed)
{
    if (pressed) { inkorb_voice_open(); if (s_chat.root) press(NULL); }
    else if (s_chat.root) release(NULL);
}

static void tick(lv_timer_t *timer)
{
    (void)timer;
    /* Show when the on-demand Wi-Fi comes up (or drops) while idle. */
    static int shown_online = -1;
    if (s_chat_visible && atomic_load(&s_state) == IDLE && inkorb_cloud_configured()) {
        int online = wifi_sta_status(NULL, 0, NULL, NULL);
        if (online != shown_online) {
            inkorb_chat_status(&s_chat, online ? "按住大键或下方按钮说话" : "正在连接网络…");
            shown_online = online;
        }
    } else if (!s_chat_visible) {
        shown_online = -1;
    }
    if(atomic_load(&s_state)==WAITING && s_wait_progress) {
        uint32_t seconds=lv_tick_elaps(s_wait_started)/1000;
        if(seconds>=3 && seconds!=s_wait_second) {
            char label[140];snprintf(label,sizeof(label),"%s · %u秒",s_wait_label,(unsigned)seconds);
            inkorb_chat_status(&s_chat,label);s_wait_second=seconds;
        }
    }
    if (atomic_load(&s_state) == RECORDING && lv_tick_elaps(s_record_started) >= 12000) release(NULL);
}

bool inkorb_voice_start(inkorb_home_t *home)
{
    s_home = home;
#ifdef CONFIG_INKORB_DUPLEX_TTS
    ESP_LOGI(TAG,"speech synthesis: bidirectional cloud TTS with streaming LLM");
#elif defined(CONFIG_INKORB_LOCAL_TTS)
    ESP_LOGI(TAG,"speech synthesis: local ESP-TTS, no cloud audio request");
#endif
    ESP_LOGI(TAG, "voice backend: %s", inkorb_cloud_configured() ? "direct cloud HTTPS (no Mac)" : "Mac bridge");
    s_queue = xQueueCreate(1, sizeof(recording_t));
    if (!s_queue) return false;
    /* Keep the extra worker stack out of the small internal SPI DMA reserve. */
    if (xTaskCreateWithCaps(worker, "voice", 32768, NULL, 3, NULL,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        vQueueDelete(s_queue); s_queue = NULL; return false;
    }
    /* Voice opens from the large key only; the capsule belongs to Wi-Fi. */
    lv_timer_create(tick, 50, NULL);
    return true;
}

bool inkorb_voice_busy(void)
{
    return atomic_load(&s_state) != IDLE;
}
