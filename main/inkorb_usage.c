#include "inkorb_usage.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "nvs.h"
#include "cJSON.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#if __has_include("voice_cloud_config.h")
#include "voice_cloud_config.h"
#else
#define INKORB_DEEPSEEK_KEY ""
#endif

static const char *TAG = "inkorb_usage";
#define NVS_NS "inkorb_usage"

typedef struct {
    uint32_t day;            /* yyyymmdd of `today` */
    uint32_t days;           /* finished days with any use, for the daily average */
    inkorb_usage_t today, total;
} saved_t;

static saved_t s_u;
static inkorb_balance_t s_balance;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

/* Today's date once SNTP has set the clock; 0 before that. */
static uint32_t today_key(void)
{
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    if (tm.tm_year + 1900 < 2025) return 0;
    return (uint32_t)((tm.tm_year + 1900) * 10000 + (tm.tm_mon + 1) * 100 + tm.tm_mday);
}

/* Caller holds s_lock; `day` comes from today_key(), computed outside the
 * critical section because localtime_r may take its own lock. */
static void roll_day(uint32_t day)
{
    if (day && day != s_u.day) {
        if (s_u.today.turns) s_u.days++;
        memset(&s_u.today, 0, sizeof(s_u.today));
        s_u.day = day;
    }
}

void inkorb_usage_load(void)
{
    nvs_handle_t nvs;
    size_t size = sizeof(s_u);
    if (nvs_open(NVS_NS, NVS_READONLY, &nvs) == ESP_OK) {
        if (nvs_get_blob(nvs, "u", &s_u, &size) != ESP_OK || size != sizeof(s_u)) memset(&s_u, 0, sizeof(s_u));
        nvs_close(nvs);
    }
}

#define ADD(field, n) do { \
    uint32_t day_ = today_key(); \
    taskENTER_CRITICAL(&s_lock); roll_day(day_); \
    s_u.today.field += (n); s_u.total.field += (n); \
    taskEXIT_CRITICAL(&s_lock); } while (0)

void inkorb_usage_add_llm(uint32_t in, uint32_t out) { ADD(llm_in, in); ADD(llm_out, out); }
void inkorb_usage_add_search(void) { ADD(searches, 1); }
void inkorb_usage_add_asr(uint32_t ms) { ADD(asr_ms, ms); }
void inkorb_usage_add_tts(uint32_t chars) { ADD(tts_chars, chars); }

void inkorb_usage_add_turn(void)
{
    ADD(turns, 1);
    saved_t copy;
    taskENTER_CRITICAL(&s_lock);
    copy = s_u;
    taskEXIT_CRITICAL(&s_lock);
    nvs_handle_t nvs;
    if (nvs_open(NVS_NS, NVS_READWRITE, &nvs) != ESP_OK) return;
    if (nvs_set_blob(nvs, "u", &copy, sizeof(copy)) == ESP_OK) nvs_commit(nvs);
    nvs_close(nvs);
}

void inkorb_usage_get(inkorb_usage_t *today, inkorb_usage_t *total)
{
    uint32_t day = today_key();
    taskENTER_CRITICAL(&s_lock);
    roll_day(day);
    if (today) *today = s_u.today;
    if (total) *total = s_u.total;
    taskEXIT_CRITICAL(&s_lock);
}

bool inkorb_usage_average(inkorb_usage_t *avg)
{
    taskENTER_CRITICAL(&s_lock);
    uint32_t days = s_u.days;
    inkorb_usage_t past = s_u.total;
    past.turns -= s_u.today.turns;
    past.llm_in -= s_u.today.llm_in;
    past.llm_out -= s_u.today.llm_out;
    past.searches -= s_u.today.searches;
    past.asr_ms -= s_u.today.asr_ms;
    past.tts_chars -= s_u.today.tts_chars;
    taskEXIT_CRITICAL(&s_lock);
    if (!days) return false;
    avg->turns = past.turns / days;
    avg->llm_in = past.llm_in / days;
    avg->llm_out = past.llm_out / days;
    avg->searches = past.searches / days;
    avg->asr_ms = past.asr_ms / days;
    avg->tts_chars = past.tts_chars / days;
    return true;
}

/* ---------- DeepSeek balance ---------- */

static void copy_field(char *out, size_t len, const cJSON *obj, const char *name)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, name);
    snprintf(out, len, "%s", cJSON_IsString(v) ? v->valuestring : "--");
}

static void balance_task(void *arg)
{
    (void)arg;
    inkorb_balance_t result = {0};
    char *body = heap_caps_calloc(1, 2048, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    esp_http_client_config_t cfg = {
        .url = "https://api.deepseek.com/user/balance", .method = HTTP_METHOD_GET, .timeout_ms = 10000,
        .buffer_size = 1024, .buffer_size_tx = 1024, .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t h = body ? esp_http_client_init(&cfg) : NULL;
    int code = -1;
    if (h) {
        char auth[160];
        snprintf(auth, sizeof(auth), "Bearer %s", INKORB_DEEPSEEK_KEY);
        esp_http_client_set_header(h, "Authorization", auth);
        if (esp_http_client_open(h, 0) == ESP_OK && esp_http_client_fetch_headers(h) >= 0) {
            code = esp_http_client_get_status_code(h);
            int n = esp_http_client_read_response(h, body, 2047);
            if (n > 0) body[n] = '\0';
        }
        esp_http_client_cleanup(h);
    }
    cJSON *json = code == 200 ? cJSON_Parse(body) : NULL;
    const cJSON *infos = cJSON_GetObjectItemCaseSensitive(json, "balance_infos");
    const cJSON *pick = NULL, *info;
    cJSON_ArrayForEach(info, infos) {
        const cJSON *cur = cJSON_GetObjectItemCaseSensitive(info, "currency");
        if (!pick || (cJSON_IsString(cur) && !strcmp(cur->valuestring, "CNY"))) pick = info; /* prefer CNY */
    }
    if (pick) {
        result.valid = true;
        result.available = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(json, "is_available"));
        copy_field(result.currency, sizeof(result.currency), pick, "currency");
        copy_field(result.total, sizeof(result.total), pick, "total_balance");
        copy_field(result.granted, sizeof(result.granted), pick, "granted_balance");
        copy_field(result.topped, sizeof(result.topped), pick, "topped_up_balance");
    } else {
        snprintf(result.error, sizeof(result.error), code < 0 ? "网络连接失败" : "查询失败（HTTP %d）", code);
    }
    cJSON_Delete(json);
    free(body);
    result.fetched_at = inkorb_usage_now();
    taskENTER_CRITICAL(&s_lock);
    if (!result.valid && s_balance.valid) {
        /* Keep the last good numbers; just report the failed refresh. */
        snprintf(s_balance.error, sizeof(s_balance.error), "%s", result.error);
    } else {
        s_balance = result;
    }
    s_balance.fetching = false;
    taskEXIT_CRITICAL(&s_lock);
    ESP_LOGI(TAG, "DeepSeek balance fetch: HTTP %d %s", code, result.valid ? "ok" : result.error);
    vTaskDeleteWithCaps(NULL);
}

void inkorb_usage_refresh_balance(void)
{
    taskENTER_CRITICAL(&s_lock);
    bool busy = s_balance.fetching;
    s_balance.fetching = true;
    taskEXIT_CRITICAL(&s_lock);
    if (busy) return;
    if (xTaskCreateWithCaps(balance_task, "balance", 6144, NULL, 2, NULL,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        taskENTER_CRITICAL(&s_lock);
        s_balance.fetching = false;
        taskEXIT_CRITICAL(&s_lock);
    }
}

void inkorb_usage_balance(inkorb_balance_t *out)
{
    taskENTER_CRITICAL(&s_lock);
    *out = s_balance;
    taskEXIT_CRITICAL(&s_lock);
}

uint32_t inkorb_usage_now(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000000);
}
