#include "dash_pix.h"

#include <string.h>

#include "sprites/dash_sprites.h"

bool dash_pix_create(dash_pix_t *p, lv_obj_t *parent, int w, int h)
{
    memset(p, 0, sizeof(*p));
    p->px = lv_malloc((size_t)w * h * 4);
    if (!p->px) {
        return false;
    }
    p->w = w;
    p->h = h;
    memset(p->px, 0, (size_t)w * h * 4);
    p->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    p->dsc.header.cf = LV_COLOR_FORMAT_ARGB8888;
    p->dsc.header.w = (uint32_t)w;
    p->dsc.header.h = (uint32_t)h;
    p->dsc.header.stride = (uint32_t)w * 4;
    p->dsc.data_size = (uint32_t)w * h * 4;
    p->dsc.data = (const uint8_t *)p->px;
    p->img = lv_image_create(parent);
    lv_image_set_src(p->img, &p->dsc);
    return true;
}

void dash_pix_destroy(dash_pix_t *p)
{
    if (p->px) {
        lv_free(p->px);
    }
    memset(p, 0, sizeof(*p));
}

void dash_pix_clear(dash_pix_t *p, uint32_t argb)
{
    for (int i = 0; i < p->w * p->h; ++i) {
        p->px[i] = argb;
    }
}

void dash_pix_commit(dash_pix_t *p)
{
    /* LV_CACHE_DEF_SIZE is 0: no decoded-image cache to drop, a redraw is enough */
    lv_obj_invalidate(p->img);
}

void dash_pix_rect(dash_pix_t *p, int x, int y, int w, int h, uint32_t argb)
{
    for (int yy = y < 0 ? 0 : y; yy < y + h && yy < p->h; ++yy) {
        for (int xx = x < 0 ? 0 : x; xx < x + w && xx < p->w; ++xx) {
            p->px[yy * p->w + xx] = argb;
        }
    }
}

void dash_pix_sprite(dash_pix_t *p, const uint8_t *spr, int sw, int sh, int x, int y, int scale)
{
    for (int sy = 0; sy < sh; ++sy) {
        for (int sx = 0; sx < sw; ++sx) {
            int i = sy * sw + sx;
            uint8_t idx = (i & 1) ? (spr[i >> 1] & 0x0F) : (spr[i >> 1] >> 4);
            if (idx) {
                dash_pix_rect(p, x + sx * scale, y + sy * scale, scale, scale, dash_sprite_palette[idx]);
            }
        }
    }
}

/* 5x7 glyphs, one byte per row, bit 4 = leftmost column */
static const uint8_t *glyph(char c)
{
    static const uint8_t digits[10][7] = {
        {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}, {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E},
        {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}, {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E},
        {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}, {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E},
        {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}, {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08},
        {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}, {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C},
    };
    static const uint8_t colon[7] = {0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x0C, 0x00};
    static const uint8_t dash[7] = {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00};
    static const uint8_t pct[7] = {0x18, 0x19, 0x02, 0x04, 0x08, 0x13, 0x03};
    static const uint8_t slash[7] = {0x01, 0x01, 0x02, 0x04, 0x08, 0x10, 0x10};
    static const uint8_t blank[7] = {0};
    if (c >= '0' && c <= '9') {
        return digits[c - '0'];
    }
    switch (c) {
    case ':':
        return colon;
    case '-':
        return dash;
    case '%':
        return pct;
    case '/':
        return slash;
    default:
        return blank;
    }
}

static int glyph_w(char c)
{
    return c == ':' ? 4 : 5;
}

int dash_pix_text_width(const char *s, int scale)
{
    int w = 0;
    for (; *s; ++s) {
        w += (glyph_w(*s) + 1) * scale;
    }
    return w ? w - scale : 0;
}

int dash_pix_text(dash_pix_t *p, const char *s, int x, int y, int scale, uint32_t argb)
{
    int x0 = x;
    for (; *s; ++s) {
        const uint8_t *g = glyph(*s);
        int gw = glyph_w(*s);
        for (int row = 0; row < 7; ++row) {
            for (int col = 0; col < 5; ++col) {
                if (g[row] & (0x10 >> col)) {
                    dash_pix_rect(p, x + col * scale, y + row * scale, scale, scale, argb);
                }
            }
        }
        x += (gw + 1) * scale;
    }
    return x - x0;
}
