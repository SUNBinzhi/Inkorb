/* SPDX-License-Identifier: Apache-2.0
 * Application-owned setup using the Waveshare BSP / Espressif adapter APIs.
 * Keep managed components untouched; bound SPI bounce-buffer size for audio.
 */
#include "inkorb_display.h"
#include <string.h>
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "bsp/touch.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io_interface.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"

/* LVGL renders this many rows per pass into PSRAM; SPI still moves 4-row chunks. */
#ifndef INKORB_DISPLAY_ROWS
#define INKORB_DISPLAY_ROWS 40
#endif
#define INKORB_DISPLAY_STATS 1 /* periodic summary at debug level; slow frames warn */

#if INKORB_DISPLAY_STATS
static struct { int64_t start, window; uint32_t frames, flushes, pixels, total_us, max_us, frame_px, frame_flushes; bool rendering; } s_stats;

static void frame_stats(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    int64_t now = esp_timer_get_time();
    if (code == LV_EVENT_RENDER_START) { s_stats.start = now; s_stats.rendering = true; s_stats.frame_px = s_stats.frame_flushes = 0; return; }
    if (code == LV_EVENT_FLUSH_START) {
        const lv_area_t *area = lv_event_get_param(event);
        s_stats.flushes++; s_stats.frame_flushes++;
        if (area) { s_stats.pixels += lv_area_get_size(area); s_stats.frame_px += lv_area_get_size(area); }
        return;
    }
    if (s_stats.rendering) {
        uint32_t us = (uint32_t)(now - s_stats.start);
        s_stats.rendering = false;
        s_stats.frames++;
        s_stats.total_us += us;
        if (us > s_stats.max_us) s_stats.max_us = us;
        if (us > 120000) ESP_LOGW("inkorb_display", "slow frame %u ms px=%u flushes=%u at %lld ms",
                                  (unsigned)(us / 1000), (unsigned)s_stats.frame_px, (unsigned)s_stats.frame_flushes, now / 1000);
    }
    if (!s_stats.window) s_stats.window = now;
    if (now - s_stats.window >= 3000000) {
        if (s_stats.frames)
            ESP_LOGD("inkorb_display", "%u rows: %u frames/3s avg=%u ms max=%u ms flushes=%u px=%uk",
                     INKORB_DISPLAY_ROWS, (unsigned)s_stats.frames, (unsigned)(s_stats.total_us / s_stats.frames / 1000),
                     (unsigned)(s_stats.max_us / 1000), (unsigned)s_stats.flushes, (unsigned)(s_stats.pixels / 1000));
        memset(&s_stats, 0, sizeof(s_stats));
        s_stats.window = now;
    }
}
#endif

/* Display bounce buffers. LVGL renders into PSRAM, and color data must reach
 * the SPI DMA from internal RAM:
 *  - letting the SPI driver allocate a temporary internal copy per transfer
 *    failed (ESP_ERR_NO_MEM) once voice turns fragmented internal RAM: flicker;
 *  - DMA straight from PSRAM (esp_lcd psram_dma_direct) underflowed while TLS,
 *    WebSocket and audio buffering loaded PSRAM, and the SPI pipeline jammed:
 *    a frozen screen.
 * So two small internal-DMA buffers (2 rows each, 3.7 KB total) are reserved
 * at boot and flushes copy through them in turn. With TLS AES in software no
 * longer borrowing internal DMA RAM, this small reservation leaves Wi-Fi room. */
#define BOUNCE_ROWS  2
#define BOUNCE_BYTES (BSP_LCD_H_RES * BOUNCE_ROWS * BSP_LCD_BITS_PER_PIXEL / 8)
static uint8_t *s_bounce[2];
static unsigned s_bounce_next;

static esp_err_t draw_through_bounce(lv_display_t *disp, esp_lcd_panel_handle_t panel, int x1, int y1,
                                     int x2, int y2, const void *color_map, void *user_ctx)
{
    (void)disp;
    (void)user_ctx;
    const int row_bytes = (x2 - x1) * (BSP_LCD_BITS_PER_PIXEL / 8);
    int rows = BOUNCE_BYTES / row_bytes;
    rows &= ~1;                       /* CO5300 wants even row windows */
    if (rows < 2) rows = 2;
    const uint8_t *src = color_map;
    esp_err_t err = ESP_OK;
    for (int y = y1; y < y2 && err == ESP_OK; y += rows) {
        int n = y2 - y < rows ? y2 - y : rows;
        /* Queue depth is 1: the transfer that used this buffer two chunks ago
         * finished before the previous chunk could be queued. */
        uint8_t *buf = s_bounce[s_bounce_next];
        s_bounce_next ^= 1;
        memcpy(buf, src, (size_t)n * row_bytes);
        err = esp_lcd_panel_draw_bitmap(panel, x1, y, x2, y + n, buf);
        src += (size_t)n * row_bytes;
    }
    return err;
}

static void align_area(lv_event_t *event)
{
    lv_area_t *area = lv_event_get_param(event);
    area->x1 = (area->x1 >> 1) << 1;
    area->y1 = (area->y1 >> 1) << 1;
    area->x2 |= 1;
    area->y2 |= 1;
}

static esp_lcd_panel_io_handle_t s_io;

/* CO5300 over QSPI: command byte in bits 15:8, write opcode 0x02 in 31:24. */
static void panel_cmd(uint8_t cmd)
{
    esp_lcd_panel_io_tx_param(s_io, (0x02 << 24) | (cmd << 8), NULL, 0);
}

void inkorb_display_panel_power(bool on)
{
    if (!s_io) return;
    if (on) {
        panel_cmd(0x11);                 /* sleep out; panel needs 120 ms */
        vTaskDelay(pdMS_TO_TICKS(120));
        panel_cmd(0x29);                 /* display on */
    } else {
        panel_cmd(0x28);                 /* display off */
        panel_cmd(0x10);                 /* sleep in */
    }
}

void inkorb_display_request_wake_from_isr(void)
{
    esp_lv_adapter_request_wake_from_isr();
}

lv_display_t *inkorb_display_start(esp_err_t (*enter)(void *), esp_err_t (*exit)(void *))
{
    esp_lv_adapter_config_t adapter = ESP_LV_ADAPTER_DEFAULT_CONFIG();
    adapter.task_core_id = 1; /* Wi-Fi and the mic task live on core 0. */
    /* Pause LVGL (and its 1 ms tick) when idle; `enter` only agrees once the
     * screen is already off, so the user's auto screen-off decides timing. */
    adapter.auto_sleep.enable = enter != NULL;
    adapter.auto_sleep.mode = ESP_LV_ADAPTER_AUTO_SLEEP_MODE_PAUSE;
    adapter.auto_sleep.idle_timeout_ms = 10000;
    adapter.auto_sleep.callbacks.on_enter_sleep = enter;
    adapter.auto_sleep.callbacks.on_exit_sleep = exit;
    /* `enter` refusing while the screen is on is expected; the adapter logs
     * each refusal as an error, every idle period. */
    esp_log_level_set("esp_lvgl:adapter", ESP_LOG_NONE);
    ESP_ERROR_CHECK(esp_lv_adapter_init(&adapter));
    esp_lcd_panel_handle_t panel;
    esp_lcd_panel_io_handle_t io;
    const bsp_display_config_t panel_cfg = {
        .max_transfer_sz = BOUNCE_BYTES,   /* one bounce buffer per SPI transfer */
    };
    ESP_ERROR_CHECK(bsp_display_new(&panel_cfg, &panel, &io));
    s_io = io;
    ESP_ERROR_CHECK(bsp_display_brightness_set(0));
    const esp_lv_adapter_display_config_t cfg = {
        .panel = panel, .panel_io = io,
        .profile = {
            .interface = ESP_LV_ADAPTER_PANEL_IF_OTHER,
            .rotation = ESP_LV_ADAPTER_ROTATE_0,
            .hor_res = BSP_LCD_H_RES, .ver_res = BSP_LCD_V_RES,
            .buffer_height = INKORB_DISPLAY_ROWS, .use_psram = true,
            .enable_ppa_accel = false, .require_double_buffer = true,
        },
        .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_NONE,
    };
    lv_display_t *display = esp_lv_adapter_register_display(&cfg);
    if (!display) return NULL;
    lv_display_add_event_cb(display, align_area, LV_EVENT_INVALIDATE_AREA, NULL);
    for (int i = 0; i < 2; i++)
        s_bounce[i] = heap_caps_aligned_alloc(4, BOUNCE_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (s_bounce[0] && s_bounce[1]) {
        const esp_lv_adapter_draw_bitmap_callbacks_t cbs = { .custom_draw_bitmap = draw_through_bounce };
        esp_lv_adapter_set_draw_bitmap_callbacks(display, &cbs, NULL);
        ESP_LOGI("inkorb_display", "display bounce buffers: 2 x %d B internal DMA", BOUNCE_BYTES);
    } else {
        ESP_LOGW("inkorb_display", "bounce buffers unavailable; SPI driver allocates per transfer");
    }
#if INKORB_DISPLAY_STATS
    lv_display_add_event_cb(display, frame_stats, LV_EVENT_RENDER_START, NULL);
    lv_display_add_event_cb(display, frame_stats, LV_EVENT_FLUSH_START, NULL);
    lv_display_add_event_cb(display, frame_stats, LV_EVENT_REFR_READY, NULL);
#endif
    bsp_display_cfg_t touch_cfg = { .touch_flags = {.mirror_x = 1, .mirror_y = 1} };
    esp_lcd_touch_handle_t touch;
    ESP_ERROR_CHECK(bsp_touch_new(&touch_cfg, &touch));
    const esp_lv_adapter_touch_config_t input = ESP_LV_ADAPTER_TOUCH_DEFAULT_CONFIG(display, touch);
    if (!esp_lv_adapter_register_touch(&input)) return NULL;
    ESP_ERROR_CHECK(esp_lv_adapter_start());
    ESP_LOGI("inkorb_display", "%d-row PSRAM buffers", INKORB_DISPLAY_ROWS);
    return display;
}
