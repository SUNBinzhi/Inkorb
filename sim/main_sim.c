/**
 * Inkorb Mac preview — software FB + SDL blit (same pixels as --shot).
 */
#include "lvgl.h"
#include "inkorb_ui.h"
#include "inkorb_boot.h"
#include "inkorb_chat.h"
#include "inkorb_wifi_ui.h"
#include "inkorb_settings_ui.h"
#include "inkorb_usage_ui.h"
#include "inkorb_usage.h"
#include "inkorb_device.h"
#include "inkorb_cloud.h"
#include "inkorb_portal.h"
#include "wifi_sta.h"

#include <SDL.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#define HOR 466
#define VER 466
#define FB_STRIDE (HOR * 4)
#define FB_SIZE (FB_STRIDE * VER)

static uint8_t g_fb[FB_SIZE];
static uint32_t g_tick_ms;
static bool g_fb_dirty;
static int g_boot_preview;
static int g_chat_preview;
static int g_wifi_preview;
static int g_settings_preview;
static int g_usage_preview;

static uint32_t tick_cb(void)
{
    if (SDL_WasInit(SDL_INIT_TIMER) || SDL_WasInit(SDL_INIT_VIDEO)) {
        return (uint32_t)SDL_GetTicks();
    }
    return g_tick_ms;
}

static void force_opaque(uint8_t *px, int w, int h, int stride)
{
    for (int y = 0; y < h; y++) {
        uint8_t *row = px + y * stride;
        for (int x = 0; x < w; x++) {
            row[x * 4 + 3] = 0xff;
        }
    }
}

static void apply_circle_mask(uint8_t *px, int w, int h, int stride)
{
    const float cx = (w - 1) * 0.5f;
    const float cy = (h - 1) * 0.5f;
    const float r = (w < h ? w : h) * 0.5f;
    const float r2 = r * r;
    for (int y = 0; y < h; y++) {
        uint8_t *row = px + y * stride;
        float dy = (float)y - cy;
        for (int x = 0; x < w; x++) {
            float dx = (float)x - cx;
            if (dx * dx + dy * dy > r2) {
                row[x * 4 + 0] = 0;
                row[x * 4 + 1] = 0;
                row[x * 4 + 2] = 0;
                row[x * 4 + 3] = 0xff;
            }
        }
    }
}


static void draw_circle_outline(uint8_t *px, int w, int h, int stride)
{
    /* Visible rim so the 466 square window shows the round AMOLED safe area. */
    const float cx = (w - 1) * 0.5f;
    const float cy = (h - 1) * 0.5f;
    const float r = (w < h ? w : h) * 0.5f - 0.5f;
    const float t = 2.0f;
    const float r_in = r - t;
    const float r2 = r * r;
    const float r_in2 = r_in > 0 ? r_in * r_in : 0;
    for (int y = 0; y < h; y++) {
        uint8_t *row = px + y * stride;
        float dy = (float)y - cy;
        for (int x = 0; x < w; x++) {
            float dx = (float)x - cx;
            float d2 = dx * dx + dy * dy;
            if (d2 <= r2 && d2 >= r_in2) {
                /* Soft gray stroke (BGRA) */
                row[x * 4 + 0] = 0x88;
                row[x * 4 + 1] = 0x88;
                row[x * 4 + 2] = 0x88;
                row[x * 4 + 3] = 0xff;
            }
        }
    }
}

static void write_ppm_xrgb(const char *path, const uint8_t *px, int w, int h, int stride)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        perror(path);
        return;
    }
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int y = 0; y < h; y++) {
        const uint8_t *row = px + y * stride;
        for (int x = 0; x < w; x++) {
            const uint8_t *p = row + x * 4;
            fputc(p[2], f);
            fputc(p[1], f);
            fputc(p[0], f);
        }
    }
    fclose(f);
    printf("wrote %s (%dx%d)\n", path, w, h);
}

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    (void)area;
    (void)px_map;
    g_fb_dirty = true;
    lv_display_flush_ready(disp);
}


/* Host stand-ins for the ESP Wi-Fi/portal layer; INKORB_SIM_WIFI picks a state. */
static const char *sim_wifi(void) { const char *v = getenv("INKORB_SIM_WIFI"); return v ? v : ""; }
bool wifi_sta_status(char *ssid, size_t len, int *rssi, char *ip)
{
    if (!strcmp(sim_wifi(), "off")) return false;
    if (ssid) snprintf(ssid, len, "MyWiFi");
    if (rssi) *rssi = -48;
    if (ip) snprintf(ip, 16, "192.168.1.20");
    return true;
}
int wifi_sta_saved(char ssids[][33], int max)
{
    static const char *names[] = { "MyWiFi", "CUHK-Lab-2.4G", "Aaron的iPhone" };
    int n = max < 3 ? max : 3;
    for (int i = 0; i < n; i++) snprintf(ssids[i], 33, "%s", names[i]);
    return n;
}
void wifi_sta_forget(const char *ssid) { (void)ssid; }
void wifi_sta_hold(void) {}
void wifi_sta_release(void) {}
void wifi_sta_use(const char *ssid) { (void)ssid; }
wifi_try_state_t wifi_sta_try_state(char *ssid, size_t len, const char **why)
{
    if (ssid) snprintf(ssid, len, "CUHK-Lab-2.4G");
    if (why) *why = "密码可能不正确";
    if (!strcmp(sim_wifi(), "connecting")) return WIFI_TRY_CONNECTING;
    if (!strcmp(sim_wifi(), "ok")) return WIFI_TRY_OK;
    if (!strcmp(sim_wifi(), "fail")) return WIFI_TRY_FAILED;
    return WIFI_TRY_IDLE;
}
int wifi_sta_ap_clients(void) { return !strcmp(sim_wifi(), "phone"); }
static bool s_sim_portal;
bool inkorb_portal_start(void) { s_sim_portal = true; return true; }
void inkorb_portal_stop(void) { s_sim_portal = false; }
bool inkorb_portal_running(void) { return s_sim_portal; }
void inkorb_portal_credentials(char *ssid, size_t a, char *pass, size_t b)
{
    snprintf(ssid, a, "INKORB-7C3A"); snprintf(pass, b, "48291736");
}

/* Host stand-ins for device settings and memory used by the settings page. */
static int s_sim_volume = 85, s_sim_bright = 100, s_sim_off = 300;
int inkorb_device_volume(void) { return s_sim_volume; }
int inkorb_device_brightness(void) { return s_sim_bright; }
void inkorb_device_set_volume(int v) { s_sim_volume = v; }
bool inkorb_device_set_brightness(int v) { s_sim_bright = v; return true; }
void inkorb_device_preview_volume(int v) { (void)v; }
void inkorb_device_preview_brightness(int v) { (void)v; }
int inkorb_device_auto_off(void) { return s_sim_off; }
void inkorb_device_set_auto_off(int s) { s_sim_off = s; }
void inkorb_device_restart(void) {}
void inkorb_device_info(char *out, size_t len)
{
    snprintf(out, len, "固件  v1.0-inkorb\n编译  Oct 29 2026 10:12:03\n电池  97%%  充电中\nWi-Fi  MyWiFi\nIP  192.168.50.88\n"
             "MAC  C4:9E:7E:0B:C7:B8\n已运行  2小时17分\n可用内存  内部41KB  PSRAM 3104KB");
}
void inkorb_cloud_memory_text(char *out, size_t len)
{
    snprintf(out, len, "%s", getenv("INKORB_SIM_EMPTY_MEMORY") ? "" : "用户叫Aaron\n最喜欢蓝色\n在香港读博，研究封闭空腔结构");
}
void inkorb_cloud_forget(bool long_term, bool recent) { (void)long_term; (void)recent; }

/* Host stand-ins for usage tallies and the DeepSeek balance. */
void inkorb_usage_get(inkorb_usage_t *today, inkorb_usage_t *total)
{
    if (today) *today = (inkorb_usage_t){ .turns = 23, .llm_in = 41800, .llm_out = 2650, .searches = 3, .asr_ms = 151000, .tts_chars = 1730 };
    if (total) *total = (inkorb_usage_t){ .turns = 412, .llm_in = 1286000, .llm_out = 51200, .searches = 37, .asr_ms = 3920000, .tts_chars = 30650 };
}
void inkorb_usage_refresh_balance(void) {}
bool inkorb_usage_average(inkorb_usage_t *avg)
{
    *avg = (inkorb_usage_t){ .turns = 18, .llm_in = 30000, .llm_out = 2000, .searches = 2, .asr_ms = 220000, .tts_chars = 1100 };
    return true;
}
uint32_t inkorb_usage_now(void) { return 400; }
void inkorb_usage_balance(inkorb_balance_t *out)
{
    *out = (inkorb_balance_t){ .valid = true, .available = true, .currency = "CNY", .total = "18.62",
                               .granted = "0.00", .topped = "18.62", .fetched_at = 220 };
}

static void sim_scroll_bottom(lv_obj_t *obj)
{
    lv_obj_update_layout(obj);
    if (lv_obj_get_scroll_bottom(obj) > 0) lv_obj_scroll_to_y(obj, LV_COORD_MAX, LV_ANIM_OFF);
    for (uint32_t i = 0; i < lv_obj_get_child_count(obj); i++) sim_scroll_bottom(lv_obj_get_child(obj, i));
}

static void build_demo_ui(inkorb_home_t *ui)
{
    if (g_chat_preview) {
        static inkorb_chat_t chat; /* animation timer keeps a pointer */
        const char *q = "今天香港天气怎么样？适合出门跑步吗？";
        inkorb_chat_create(&chat, NULL, NULL, NULL);
        switch (g_chat_preview) {
        case 2:
            break;
        case 3:
            inkorb_chat_text(&chat, "正在听你说话…", "");
            inkorb_chat_status(&chat, "正在录音，松开发送，最多12秒");
            break;
        case 4:
            inkorb_chat_text(&chat, q, NULL);
            inkorb_chat_status(&chat, "AI 正在思考 · 4秒");
            break;
        case 5:
            inkorb_chat_text(&chat, q, "今天香港多云，气温大约 22 到 27 度，湿度偏高，傍晚有一点东风。整体适合出门跑步，建议避开中午，选择傍晚六点以后，记得多补水。"
                             "\n\n联网来源：\n香港天文台 九天天气预报\nhko.gov.hk");
            inkorb_chat_status(&chat, "AI 正在回答");
            break;
        case 6:
            inkorb_chat_text(&chat, q, "");
            inkorb_chat_status(&chat, "语音识别连接失败，请检查网络");
            break;
        default:
            inkorb_chat_text(&chat, "你好，你能做什么？", "你好，我是你的 INKORB 语音伙伴。你可以和我聊天、问问题，或者让我帮你整理一个想法。");
            inkorb_chat_status(&chat, "按住大键，继续聊");
        }
        return;
    }
    if (g_boot_preview) {
        static inkorb_boot_t boot; /* animation timer keeps a pointer */
        inkorb_boot_create(&boot, NULL, NULL, NULL);
        inkorb_boot_set_status(&boot, g_boot_preview == 3 ? "Ready" : "Connecting to Wi-Fi",
                               g_boot_preview == 3);
        if (g_boot_preview == 2) inkorb_boot_allow_offline(&boot);
        return;
    }
    inkorb_home_create(ui);
    inkorb_home_set_demo_clock(ui, "14:32", "2026-10-28");
    inkorb_home_set_ip(ui, "192.168.1.20");
    /* INKORB_SIM_BATT="73" or "73,charging" or "100,plugged" previews the capsule battery. */
    const char *batt = getenv("INKORB_SIM_BATT");
    if (batt) {
        inkorb_home_set_battery(ui, atoi(batt));
        inkorb_home_set_charging(ui, strstr(batt, "charging") || strstr(batt, "plugged"),
                                 strstr(batt, "charging") != NULL);
    }
    if (g_usage_preview) {
        static inkorb_home_t home;
        home = *ui;
        inkorb_usage_ui_init(&home);
        inkorb_usage_ui_open();
        if (getenv("INKORB_SIM_SCROLL")) sim_scroll_bottom(lv_screen_active());
        return;
    }
    if (g_settings_preview) {
        static inkorb_home_t home;
        home = *ui;
        inkorb_settings_ui_init(&home);
        inkorb_settings_ui_open_page(g_settings_preview - 1);
        /* INKORB_SIM_SCROLL=1 previews the bottom of scrolling lists. */
        if (getenv("INKORB_SIM_SCROLL")) sim_scroll_bottom(lv_screen_active());
        return;
    }
    if (g_wifi_preview) {
        static inkorb_home_t home;
        home = *ui;
        inkorb_wifi_ui_init(&home);
        inkorb_wifi_ui_open(g_wifi_preview == 2);
        return;
    }
    printf("UI: modern sans + glow + capsule (INKORB / time / date / weekday / WiFi|IP)\n");
    fflush(stdout);
    (void)ui;
}

static lv_display_t *make_sw_display(void)
{
    memset(g_fb, 0, sizeof(g_fb));
    lv_display_t *disp = lv_display_create(HOR, VER);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_ARGB8888);
    lv_display_set_flush_cb(disp, flush_cb);
    lv_display_set_buffers(disp, g_fb, NULL, FB_SIZE, LV_DISPLAY_RENDER_MODE_FULL);
    return disp;
}

static void present(SDL_Renderer *ren, SDL_Texture *tex)
{
    force_opaque(g_fb, HOR, VER, FB_STRIDE);
    apply_circle_mask(g_fb, HOR, VER, FB_STRIDE);
    draw_circle_outline(g_fb, HOR, VER, FB_STRIDE);
    if (SDL_UpdateTexture(tex, NULL, g_fb, FB_STRIDE) != 0) {
        fprintf(stderr, "SDL_UpdateTexture: %s\n", SDL_GetError());
    }
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
    SDL_RenderClear(ren);
    SDL_RenderCopy(ren, tex, NULL, NULL);
    SDL_RenderPresent(ren);
    g_fb_dirty = false;
}

static int run_shot(const char *path)
{
    lv_init();
    lv_tick_set_cb(tick_cb);
    make_sw_display();
    inkorb_home_t ui;
    build_demo_ui(&ui);
    /* Chat previews advance ~4.5s so recording/thinking animations are mid-way. */
    int frames = g_chat_preview > 1 ? 150 : 8;
    const char *ms = getenv("INKORB_SIM_MS");
    if (ms) frames = atoi(ms) / 30;
    for (int i = 0; i < frames; i++) {
        g_tick_ms += 30;
        lv_timer_handler();
    }
    force_opaque(g_fb, HOR, VER, FB_STRIDE);
    apply_circle_mask(g_fb, HOR, VER, FB_STRIDE);
    draw_circle_outline(g_fb, HOR, VER, FB_STRIDE);
    write_ppm_xrgb(path, g_fb, HOR, VER, FB_STRIDE);
    return 0;
}

static int run_sdl(void)
{
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }

    lv_init();
    lv_tick_set_cb(tick_cb);
    make_sw_display();

    inkorb_home_t ui;
    build_demo_ui(&ui);

    SDL_Window *win = SDL_CreateWindow(
        "Inkorb sim 466",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        HOR, VER, SDL_WINDOW_SHOWN | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!win) {
        fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
        return 1;
    }

    /* Software renderer is most predictable on Mac agent sessions. */
    SDL_Renderer *ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
    if (!ren) {
        fprintf(stderr, "SDL_CreateRenderer: %s\n", SDL_GetError());
        return 1;
    }
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
    SDL_RenderSetLogicalSize(ren, HOR, VER);

    /* LVGL ARGB8888 LE memory is B,G,R,A — matches SDL_PIXELFORMAT_ARGB8888. */
    SDL_Texture *tex = SDL_CreateTexture(
        ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, HOR, VER);
    if (!tex) {
        fprintf(stderr, "SDL_CreateTexture: %s\n", SDL_GetError());
        return 1;
    }
    SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_NONE);

    for (int i = 0; i < 8; i++) {
        lv_timer_handler();
        SDL_Delay(16);
    }
    present(ren, tex);

    /* Count non-black pixels as a sanity log */
    int nz = 0;
    for (int i = 0; i < HOR * VER; i++) {
        uint8_t *p = g_fb + i * 4;
        if (p[0] | p[1] | p[2]) nz++;
    }
    printf("Inkorb SDL sim 466 — nonzero px=%d. Close window to exit.\n", nz);
    fflush(stdout);
    if (nz < 100) {
        fprintf(stderr, "WARNING: framebuffer almost empty — UI may not have rendered\n");
    }

    bool running = true;
    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) running = false;
            if (e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_CLOSE) running = false;
            if (e.type == SDL_WINDOWEVENT &&
                (e.window.event == SDL_WINDOWEVENT_EXPOSED ||
                 e.window.event == SDL_WINDOWEVENT_SHOWN)) {
                g_fb_dirty = true;
            }
        }
        uint32_t delay = lv_timer_handler();
        if (g_fb_dirty) {
            present(ren, tex);
        }
        if (delay > 33) delay = 33;
        SDL_Delay(delay ? delay : 8);
    }

    SDL_DestroyTexture(tex);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}

int main(int argc, char **argv)
{
    const char *shot = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--shot") == 0 && i + 1 < argc) shot = argv[++i];
        else if (strcmp(argv[i], "--boot") == 0) g_boot_preview = 1;
        else if (strcmp(argv[i], "--boot-offline") == 0) g_boot_preview = 2;
        else if (strcmp(argv[i], "--boot-ready") == 0) g_boot_preview = 3;
        else if (strcmp(argv[i], "--chat") == 0) g_chat_preview = 1;
        else if (strcmp(argv[i], "--chat-empty") == 0) g_chat_preview = 2;
        else if (strcmp(argv[i], "--chat-listen") == 0) g_chat_preview = 3;
        else if (strcmp(argv[i], "--chat-think") == 0) g_chat_preview = 4;
        else if (strcmp(argv[i], "--chat-speak") == 0) g_chat_preview = 5;
        else if (strcmp(argv[i], "--chat-error") == 0) g_chat_preview = 6;
        else if (strcmp(argv[i], "--wifi") == 0) g_wifi_preview = 1;
        else if (strcmp(argv[i], "--wifi-setup") == 0) g_wifi_preview = 2;
        else if (strcmp(argv[i], "--settings") == 0) g_settings_preview = 1;
        else if (strcmp(argv[i], "--usage") == 0) g_usage_preview = 1;
        else if (strcmp(argv[i], "--settings-memory") == 0) g_settings_preview = 2;
        else if (strcmp(argv[i], "--settings-about") == 0) g_settings_preview = 3;
        else if (strcmp(argv[i], "--settings-screen") == 0) g_settings_preview = 4;
    }
    if (shot) return run_shot(shot);
    return run_sdl();
}
