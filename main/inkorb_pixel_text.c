#include "inkorb_pixel_text.h"

#include <stdlib.h>
#include <string.h>

#if defined(ESP_PLATFORM)
#include "esp_heap_caps.h"
#endif

#define GLYPH_COLS 5
#define GLYPH_ROWS 7
#define INKORB_PIXEL_TEXT_MAX 32

typedef struct {
    int cell;
    int gap;
    int radius;
    lv_color_t color;
    char text[INKORB_PIXEL_TEXT_MAX];
    uint8_t *buf;       /* A8 pixels; prefer PSRAM so SPI DMA keeps internal RAM */
    size_t buf_cap;
    int buf_w;
    int buf_h;
    int stride;
    lv_image_dsc_t dsc;
} inkorb_pixel_style_t;

static void *inkorb_px_alloc(size_t n)
{
#if defined(ESP_PLATFORM)
    /* Force PSRAM: SPIRAM_MALLOC_ALWAYSINTERNAL would otherwise put small bufs in DMA RAM. */
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p) {
        return p;
    }
#endif
    return malloc(n);
}

static void inkorb_px_free(void *p)
{
#if defined(ESP_PLATFORM)
    heap_caps_free(p);
#else
    free(p);
#endif
}

/* Each row: bits 4..0 = left..right (bit4 = leftmost column). */
static const uint8_t *glyph_rows(char ch)
{
    static const uint8_t G0[7] = {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E};
    static const uint8_t G1[7] = {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E};
    static const uint8_t G2[7] = {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F};
    static const uint8_t G3[7] = {0x0E, 0x11, 0x01, 0x06, 0x01, 0x11, 0x0E};
    static const uint8_t G4[7] = {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02};
    static const uint8_t G5[7] = {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E};
    static const uint8_t G6[7] = {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E};
    static const uint8_t G7[7] = {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08};
    static const uint8_t G8[7] = {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E};
    static const uint8_t G9[7] = {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C};
    static const uint8_t GCOL[7] = {0x00, 0x04, 0x04, 0x00, 0x04, 0x04, 0x00};
    static const uint8_t GDOT[7] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C};
    static const uint8_t GDASH[7] = {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00};
    static const uint8_t GA[7] = {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11};
    static const uint8_t GB[7] = {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E};
    static const uint8_t GC[7] = {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E};
    static const uint8_t GD[7] = {0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E};
    static const uint8_t GE[7] = {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F};
    static const uint8_t GF[7] = {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10};
    static const uint8_t GG[7] = {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F};
    static const uint8_t GH[7] = {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11};
    static const uint8_t GI[7] = {0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E};
    static const uint8_t GJ[7] = {0x01, 0x01, 0x01, 0x01, 0x11, 0x11, 0x0E};
    static const uint8_t GK[7] = {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11};
    static const uint8_t GL[7] = {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F};
    static const uint8_t GM[7] = {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11};
    static const uint8_t GN[7] = {0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11};
    static const uint8_t GO[7] = {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E};
    static const uint8_t GP[7] = {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10};
    static const uint8_t GQ[7] = {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D};
    static const uint8_t GR[7] = {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11};
    static const uint8_t GS[7] = {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E};
    static const uint8_t GT[7] = {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04};
    static const uint8_t GU[7] = {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E};
    static const uint8_t GV[7] = {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04};
    static const uint8_t GW[7] = {0x11, 0x11, 0x11, 0x15, 0x15, 0x1B, 0x11};
    static const uint8_t GX[7] = {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11};
    static const uint8_t GY[7] = {0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04};
    static const uint8_t GZ[7] = {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F};
    static const uint8_t GSP[7] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    static const uint8_t GUNK[7] = {0x1F, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1F};

    if (ch >= 'a' && ch <= 'z') {
        ch = (char)(ch - 'a' + 'A');
    }

    switch (ch) {
    case '0': return G0;
    case '1': return G1;
    case '2': return G2;
    case '3': return G3;
    case '4': return G4;
    case '5': return G5;
    case '6': return G6;
    case '7': return G7;
    case '8': return G8;
    case '9': return G9;
    case ':': return GCOL;
    case '.': return GDOT;
    case '-': return GDASH;
    case ' ': return GSP;
    case 'A': return GA;
    case 'B': return GB;
    case 'C': return GC;
    case 'D': return GD;
    case 'E': return GE;
    case 'F': return GF;
    case 'G': return GG;
    case 'H': return GH;
    case 'I': return GI;
    case 'J': return GJ;
    case 'K': return GK;
    case 'L': return GL;
    case 'M': return GM;
    case 'N': return GN;
    case 'O': return GO;
    case 'P': return GP;
    case 'Q': return GQ;
    case 'R': return GR;
    case 'S': return GS;
    case 'T': return GT;
    case 'U': return GU;
    case 'V': return GV;
    case 'W': return GW;
    case 'X': return GX;
    case 'Y': return GY;
    case 'Z': return GZ;
    default: return GUNK;
    }
}

static void measure_text(const inkorb_pixel_style_t *st, const char *text, int *out_w, int *out_h)
{
    const int cell = st->cell;
    const int gap = st->gap;
    const int glyph_w = GLYPH_COLS * cell + (GLYPH_COLS - 1) * gap;
    const int glyph_h = GLYPH_ROWS * cell + (GLYPH_ROWS - 1) * gap;
    const int advance = glyph_w + gap;

    int nchars = 0;
    for (const char *p = text; *p; p++) {
        nchars++;
    }

    int total_w = 1;
    if (nchars > 0) {
        total_w = (nchars - 1) * advance + glyph_w;
        if (total_w < 1) {
            total_w = 1;
        }
    }
    *out_w = total_w;
    *out_h = glyph_h > 0 ? glyph_h : 1;
}

/* Soft-paint one rounded square into A8 buffer (CPU, no LVGL draw tasks / masks). */
static void paint_block(uint8_t *buf, int stride, int img_w, int img_h,
                        int x0, int y0, int cell, int radius)
{
    for (int dy = 0; dy < cell; dy++) {
        for (int dx = 0; dx < cell; dx++) {
            const int x = x0 + dx;
            const int y = y0 + dy;
            if (x < 0 || y < 0 || x >= img_w || y >= img_h) {
                continue;
            }

            /* Corner cut ≈ rounded square; radius 1 drops the 4 corner pixels. */
            if (radius > 0) {
                const int r = radius;
                int in = 1;
                if (dx < r && dy < r) {
                    const int ox = r - dx;
                    const int oy = r - dy;
                    if (ox * ox + oy * oy > r * r) {
                        in = 0;
                    }
                } else if (dx >= cell - r && dy < r) {
                    const int ox = dx - (cell - 1 - r);
                    const int oy = r - dy;
                    if (ox * ox + oy * oy > r * r) {
                        in = 0;
                    }
                } else if (dx < r && dy >= cell - r) {
                    const int ox = r - dx;
                    const int oy = dy - (cell - 1 - r);
                    if (ox * ox + oy * oy > r * r) {
                        in = 0;
                    }
                } else if (dx >= cell - r && dy >= cell - r) {
                    const int ox = dx - (cell - 1 - r);
                    const int oy = dy - (cell - 1 - r);
                    if (ox * ox + oy * oy > r * r) {
                        in = 0;
                    }
                }
                if (!in) {
                    continue;
                }
            }

            buf[y * stride + x] = 255;
        }
    }
}

static void rasterize(inkorb_pixel_style_t *st)
{
    const int cell = st->cell;
    const int gap = st->gap;
    const int step = cell + gap;
    const int glyph_w = GLYPH_COLS * cell + (GLYPH_COLS - 1) * gap;
    const int advance = glyph_w + gap;
    const int w = st->buf_w;
    const int h = st->buf_h;
    const int stride = st->stride;

    memset(st->buf, 0, (size_t)stride * (size_t)h);

    int pen_x = 0;
    for (const char *p = st->text; *p; p++) {
        const uint8_t *rows = glyph_rows(*p);
        for (int r = 0; r < GLYPH_ROWS; r++) {
            uint8_t bits = rows[r];
            for (int c = 0; c < GLYPH_COLS; c++) {
                if (bits & (1u << (GLYPH_COLS - 1 - c))) {
                    paint_block(st->buf, stride, w, h,
                                pen_x + c * step, r * step, cell, st->radius);
                }
            }
        }
        pen_x += advance;
    }
}

static void on_delete(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_target(e);
    inkorb_pixel_style_t *st = (inkorb_pixel_style_t *)lv_obj_get_user_data(obj);
    if (!st) {
        return;
    }
    if (st->buf) {
        inkorb_px_free(st->buf);
        st->buf = NULL;
    }
    lv_free(st);
    lv_obj_set_user_data(obj, NULL);
}

lv_obj_t *inkorb_pixel_label_create(lv_obj_t *parent, int cell, int gap, int radius,
                                    lv_color_t color)
{
    /* One lv_image per label — never per-dot objects. */
    lv_obj_t *img = lv_image_create(parent);
    lv_obj_remove_style_all(img);
    lv_obj_set_style_bg_opa(img, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(img, 0, 0);
    lv_obj_set_style_pad_all(img, 0, 0);
    lv_obj_clear_flag(img, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_scrollbar_mode(img, LV_SCROLLBAR_MODE_OFF);

    inkorb_pixel_style_t *st = lv_malloc(sizeof(*st));
    if (!st) {
        return img;
    }
    memset(st, 0, sizeof(*st));
    st->cell = cell > 0 ? cell : 4;
    st->gap = gap >= 0 ? gap : 1;
    if (radius < 0) {
        radius = 1;
    }
    {
        int max_r = st->cell / 2 - 1;
        if (max_r < 0) {
            max_r = 0;
        }
        st->radius = radius > max_r ? max_r : radius;
    }
    st->color = color;
    st->text[0] = '\0';

    lv_obj_set_style_image_recolor(img, color, 0);
    lv_obj_set_style_image_recolor_opa(img, LV_OPA_COVER, 0);

    lv_obj_set_user_data(img, st);
    lv_obj_add_event_cb(img, on_delete, LV_EVENT_DELETE, NULL);
    return img;
}

void inkorb_pixel_label_set_text(lv_obj_t *label, const char *text)
{
    if (!label) {
        return;
    }
    inkorb_pixel_style_t *st = (inkorb_pixel_style_t *)lv_obj_get_user_data(label);
    if (!st) {
        return;
    }
    if (!text) {
        text = "";
    }

    char next[INKORB_PIXEL_TEXT_MAX];
    strncpy(next, text, INKORB_PIXEL_TEXT_MAX - 1);
    next[INKORB_PIXEL_TEXT_MAX - 1] = '\0';

    if (strcmp(st->text, next) == 0) {
        return;
    }
    memcpy(st->text, next, sizeof(next));

    int w = 1;
    int h = 1;
    measure_text(st, st->text, &w, &h);

    uint32_t stride_u = lv_draw_buf_width_to_stride((uint32_t)w, LV_COLOR_FORMAT_A8);
    int stride = (int)stride_u;
    size_t need = (size_t)stride * (size_t)h;

    if (need > st->buf_cap || !st->buf) {
        uint8_t *nb = (uint8_t *)inkorb_px_alloc(need);
        if (!nb) {
            return;
        }
        if (st->buf) {
            inkorb_px_free(st->buf);
        }
        st->buf = nb;
        st->buf_cap = need;
    }

    st->buf_w = w;
    st->buf_h = h;
    st->stride = stride;
    rasterize(st);

    memset(&st->dsc, 0, sizeof(st->dsc));
    st->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    st->dsc.header.cf = LV_COLOR_FORMAT_A8;
    st->dsc.header.w = (uint32_t)w;
    st->dsc.header.h = (uint32_t)h;
    st->dsc.header.stride = (uint32_t)stride;
    st->dsc.data = st->buf;
    st->dsc.data_size = need;

    lv_image_set_src(label, &st->dsc);
    lv_obj_set_size(label, w, h);
    lv_obj_invalidate(label);
}
