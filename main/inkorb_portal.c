#include "inkorb_portal.h"
#include "wifi_sta.h"
#include "esp_http_server.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "lwip/sockets.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Never log the hotspot password or the submitted Wi-Fi password. */
static const char *TAG = "inkorb_portal";
static httpd_handle_t s_http;
static volatile bool s_dns_run;
static volatile int s_dns_sock = -1;
static TaskHandle_t s_dns_task;
static char s_ap_ssid[33], s_ap_pass[16];

static const char PAGE[] =
"<!doctype html><html lang=zh><head><meta charset=utf-8>"
"<meta name=viewport content='width=device-width,initial-scale=1'><title>INKORB Wi-Fi</title><style>"
"body{margin:0;background:#000;color:#e8f3ff;font:16px -apple-system,system-ui,sans-serif}"
"main{max-width:420px;margin:auto;padding:28px 20px}h1{font-weight:500;letter-spacing:6px;font-size:20px;margin:0 0 4px;text-align:center}"
"p.sub{color:#89a0b4;text-align:center;margin:0 0 24px;font-size:14px}"
"button,input{font:inherit;border-radius:14px;border:1px solid #1c3a55;background:#050e1c;color:#e8f3ff;padding:14px 16px;width:100%;box-sizing:border-box}"
"#list button{display:flex;justify-content:space-between;margin-bottom:8px;text-align:left}#list button.on{border-color:#4eb4ff;box-shadow:0 0 12px #1e90ff66}"
"small{color:#5d7186}input{margin:14px 0 10px}"
".go{background:#4eb4ff;color:#050e1c;border:0;font-weight:600;margin-top:6px}.go:disabled{opacity:.4}"
".link{background:none;border:0;color:#7ec8ff;padding:10px 0}#msg{margin-top:18px;text-align:center;min-height:24px;color:#7ec8ff}"
"#msg.bad{color:#ffb45e}#msg.ok{color:#62e69a}</style></head><body><main>"
"<h1>INKORB</h1><p class=sub>为设备选择 Wi-Fi（仅支持 2.4GHz）</p>"
"<div id=list><small>正在扫描附近网络…</small></div>"
"<button class=link id=rescan>重新扫描</button> <button class=link id=other>输入其他网络名</button>"
"<input id=ssid placeholder='网络名称' hidden autocomplete=off>"
"<input id=pass type=password placeholder='Wi-Fi 密码（开放网络留空）' autocomplete=off>"
"<button class=go id=go disabled>连接</button><div id=msg></div></main><script>"
"let sel='';const $=i=>document.getElementById(i),msg=(t,c)=>{$('msg').textContent=t;$('msg').className=c||''};"
"function pick(b,s){document.querySelectorAll('#list button').forEach(x=>x.classList.remove('on'));b.classList.add('on');sel=s;$('go').disabled=false;$('pass').focus()}"
"function scan(){$('list').innerHTML='<small>正在扫描附近网络…</small>';fetch('/scan').then(r=>r.json()).then(a=>{const l=$('list');l.textContent='';"
"if(!a.length){l.innerHTML='<small>没有找到 2.4GHz 网络，可稍后重新扫描</small>';return}"
"a.forEach(n=>{const b=document.createElement('button'),s=document.createElement('span'),m=document.createElement('small');"
"s.textContent=n.s;m.textContent=(n.k?'已保存 · ':'')+(n.r>-60?'信号强':n.r>-72?'信号中':'信号弱')+(n.l?' · 加密':' · 开放');"
"b.append(s,m);b.onclick=()=>{$('ssid').hidden=true;pick(b,n.s)};l.append(b)})}).catch(()=>msg('扫描失败，请重新扫描','bad'))}"
"$('rescan').onclick=scan;$('other').onclick=()=>{$('ssid').hidden=false;$('ssid').focus();sel='';$('go').disabled=false};"
"$('go').onclick=()=>{const s=$('ssid').hidden?sel:$('ssid').value.trim(),p=$('pass').value;"
"if(!s)return msg('请选择或输入网络名','bad');if(p&&(p.length<8||p.length>63))return msg('密码应为 8–63 位','bad');"
"$('go').disabled=true;msg('正在连接 '+s+' …');"
"fetch('/save',{method:'POST',body:new URLSearchParams({ssid:s,pass:p})}).then(()=>poll(0)).catch(()=>poll(0))};"
"function poll(n){setTimeout(()=>fetch('/status').then(r=>r.json()).then(j=>{"
"if(j.state=='ok')msg('已连接 '+j.ssid+'，设备已保存此网络，可以关闭本页','ok');"
"else if(j.state=='failed'){msg('连接失败：'+j.why,'bad');$('go').disabled=false}else poll(n+1)})"
".catch(()=>n<40?poll(n+1):msg('页面已断开，请查看设备屏幕上的结果')),1000)}"
"scan()</script></body></html>";

static void json_string(char *out, size_t cap, size_t *len, const char *s)
{
    if (*len < cap) out[(*len)++] = '"';
    for (; *s && *len + 7 < cap; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { out[(*len)++] = '\\'; out[(*len)++] = c; }
        else if (c < 0x20) *len += snprintf(out + *len, cap - *len, "\\u%04x", c);
        else out[(*len)++] = c;
    }
    if (*len < cap) out[(*len)++] = '"';
}

static esp_err_t send_json(httpd_req_t *req, const char *body)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, body);
}

static esp_err_t page_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, PAGE, sizeof(PAGE) - 1);
}

static esp_err_t scan_get(httpd_req_t *req)
{
    const size_t cap = 3072;
    uint16_t count = 20;
    wifi_ap_record_t *aps = heap_caps_calloc(count, sizeof(*aps), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    char *out = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    char (*saved)[33] = heap_caps_calloc(WIFI_STA_MAX_SAVED, 33, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!aps || !out || !saved) { free(aps); free(out); free(saved); return send_json(req, "[]"); }
    wifi_scan_config_t scan = {0};
    esp_err_t err = ESP_FAIL;
    for (int i = 0; i < 3 && err != ESP_OK; i++) {
        err = esp_wifi_scan_start(&scan, true);
        if (err != ESP_OK) vTaskDelay(pdMS_TO_TICKS(700)); /* station busy connecting */
    }
    if (err != ESP_OK || esp_wifi_scan_get_ap_records(&count, aps) != ESP_OK) count = 0;
    int nsaved = wifi_sta_saved(saved, WIFI_STA_MAX_SAVED);
    size_t len = 0;
    out[len++] = '[';
    for (unsigned i = 0; i < count && len + 120 < cap; i++) {
        const char *ssid = (const char *)aps[i].ssid;
        bool dup = !ssid[0];
        for (unsigned j = 0; j < i && !dup; j++) dup = !strcmp(ssid, (const char *)aps[j].ssid);
        if (dup) continue; /* results come strongest first */
        bool known = false;
        for (int k = 0; k < nsaved && !known; k++) known = !strcasecmp(ssid, saved[k]);
        if (len > 1) out[len++] = ',';
        len += snprintf(out + len, cap - len, "{\"s\":");
        json_string(out, cap, &len, ssid);
        len += snprintf(out + len, cap - len, ",\"r\":%d,\"l\":%d,\"k\":%d}", aps[i].rssi,
                        aps[i].authmode != WIFI_AUTH_OPEN, known);
    }
    out[len++] = ']';
    out[len] = '\0';
    esp_err_t res = send_json(req, out);
    free(aps); free(out); free(saved);
    return res;
}

static int hex(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Extracts and URL-decodes one field of an x-www-form-urlencoded body. */
static bool form_field(const char *body, const char *key, char *out, size_t cap)
{
    size_t klen = strlen(key);
    for (const char *p = body; p && *p; p = strchr(p, '&') ? strchr(p, '&') + 1 : NULL) {
        if (strncmp(p, key, klen) || p[klen] != '=') continue;
        size_t n = 0;
        for (p += klen + 1; *p && *p != '&'; p++) {
            char c = *p;
            if (c == '+') c = ' ';
            else if (c == '%' && hex(p[1]) >= 0 && hex(p[2]) >= 0) { c = (char)(hex(p[1]) * 16 + hex(p[2])); p += 2; }
            if (n + 1 >= cap) return false;
            out[n++] = c;
        }
        out[n] = '\0';
        return true;
    }
    return false;
}

static esp_err_t save_post(httpd_req_t *req)
{
    char body[512], ssid[33], pass[65];
    if (req->content_len >= sizeof(body)) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "too long");
    int got = 0;
    while (got < (int)req->content_len) {
        int n = httpd_req_recv(req, body + got, req->content_len - got);
        if (n <= 0) return ESP_FAIL;
        got += n;
    }
    body[got] = '\0';
    bool ok = form_field(body, "ssid", ssid, sizeof(ssid)) && ssid[0] &&
              form_field(body, "pass", pass, sizeof(pass));
    memset(body, 0, sizeof(body));
    size_t plen = ok ? strlen(pass) : 0;
    if (!ok || (plen && (plen < 8 || plen > 63))) {
        memset(pass, 0, sizeof(pass));
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid");
    }
    wifi_sta_try(ssid, pass);
    memset(pass, 0, sizeof(pass));
    return send_json(req, "{\"ok\":true}");
}

static esp_err_t status_get(httpd_req_t *req)
{
    char ssid[33], out[256];
    const char *why = NULL;
    static const char *names[] = { "idle", "connecting", "ok", "failed" };
    wifi_try_state_t st = wifi_sta_try_state(ssid, sizeof(ssid), &why);
    size_t len = snprintf(out, sizeof(out), "{\"state\":\"%s\",\"ssid\":", names[st]);
    json_string(out, sizeof(out), &len, ssid);
    len += snprintf(out + len, sizeof(out) - len, ",\"why\":");
    json_string(out, sizeof(out), &len, why ? why : "");
    if (len + 2 < sizeof(out)) { out[len++] = '}'; out[len] = '\0'; }
    return send_json(req, out);
}

/* Any other URL (OS connectivity checks included) lands on the form. */
static esp_err_t redirect(httpd_req_t *req, httpd_err_code_t err)
{
    (void)err;
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    return httpd_resp_send(req, NULL, 0);
}

/* Minimal DNS: every A query resolves to the hotspot address. */
static void dns_task(void *arg)
{
    (void)arg;
    uint8_t buf[512];
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in addr = { .sin_family = AF_INET, .sin_port = htons(53), .sin_addr.s_addr = htonl(INADDR_ANY) };
    struct timeval tv = { .tv_sec = 1 };
    if (sock < 0 || bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGW(TAG, "DNS socket unavailable");
        if (sock >= 0) close(sock);
        s_dns_task = NULL;
        vTaskDeleteWithCaps(NULL);
        return;
    }
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    s_dns_sock = sock;
    while (s_dns_run) {
        struct sockaddr_in from;
        socklen_t flen = sizeof(from);
        int n = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&from, &flen);
        if (n < 12 || (buf[2] & 0x80)) continue;
        int q = 12;
        while (q < n && buf[q]) q += buf[q] + 1;
        if (q + 5 > n) continue;
        bool type_a = buf[q + 1] == 0 && buf[q + 2] == 1;
        int end = q + 5;
        if (end + 16 > (int)sizeof(buf)) continue;
        buf[2] = 0x84 | (buf[2] & 0x01);   /* response, authoritative, keep RD */
        buf[3] = 0x80;                     /* RA, no error */
        buf[4] = 0; buf[5] = 1;            /* one question */
        buf[6] = 0; buf[7] = type_a;       /* answer only for A */
        memset(buf + 8, 0, 4);
        if (type_a) {
            static const uint8_t answer[16] = { 0xC0, 0x0C, 0, 1, 0, 1, 0, 0, 0, 60, 0, 4, 192, 168, 4, 1 };
            memcpy(buf + end, answer, sizeof(answer));
            end += sizeof(answer);
        }
        sendto(sock, buf, end, 0, (struct sockaddr *)&from, flen);
    }
    close(sock);
    s_dns_sock = -1;
    s_dns_task = NULL;
    vTaskDeleteWithCaps(NULL);
}

bool inkorb_portal_running(void) { return s_http != NULL; }

void inkorb_portal_credentials(char *ssid, size_t ssid_len, char *pass, size_t pass_len)
{
    snprintf(ssid, ssid_len, "%s", s_ap_ssid);
    snprintf(pass, pass_len, "%s", s_ap_pass);
}

bool inkorb_portal_start(void)
{
    if (s_http) return true;
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(s_ap_ssid, sizeof(s_ap_ssid), "INKORB-%02X%02X", mac[4], mac[5]);
    /* Fresh 8-digit password per session, shown only on the device screen. */
    snprintf(s_ap_pass, sizeof(s_ap_pass), "%08lu", (unsigned long)(esp_random() % 100000000UL));
    if (wifi_sta_set_ap(true, s_ap_ssid, s_ap_pass) != ESP_OK) {
        ESP_LOGE(TAG, "hotspot start failed");
        return false;
    }
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 6144;
    cfg.task_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    cfg.max_open_sockets = 4;
    cfg.lru_purge_enable = true;
    if (httpd_start(&s_http, &cfg) != ESP_OK) {
        s_http = NULL;
        wifi_sta_set_ap(false, NULL, NULL);
        return false;
    }
    const httpd_uri_t uris[] = {
        { .uri = "/", .method = HTTP_GET, .handler = page_get },
        { .uri = "/scan", .method = HTTP_GET, .handler = scan_get },
        { .uri = "/save", .method = HTTP_POST, .handler = save_post },
        { .uri = "/status", .method = HTTP_GET, .handler = status_get },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) httpd_register_uri_handler(s_http, &uris[i]);
    httpd_register_err_handler(s_http, HTTPD_404_NOT_FOUND, redirect);
    s_dns_run = true;
    if (xTaskCreateWithCaps(dns_task, "portal_dns", 3072, NULL, 4, &s_dns_task,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        s_dns_run = false;
        ESP_LOGW(TAG, "DNS task unavailable; page still at 192.168.4.1");
    }
    ESP_LOGI(TAG, "setup hotspot %s up at 192.168.4.1", s_ap_ssid);
    return true;
}

void inkorb_portal_stop(void)
{
    if (!s_http) return;
    s_dns_run = false;
    for (int i = 0; i < 30 && s_dns_task; i++) vTaskDelay(pdMS_TO_TICKS(50));
    httpd_stop(s_http);
    s_http = NULL;
    wifi_sta_set_ap(false, NULL, NULL);
    memset(s_ap_pass, 0, sizeof(s_ap_pass));
    ESP_LOGI(TAG, "setup hotspot closed");
}
