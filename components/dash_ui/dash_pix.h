/* Tiny pixel-art helpers: an lv_image backed by an ARGB8888 buffer we draw
 * into directly (sprites scaled by whole numbers, a 5x7 pixel font, blocks). */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"

typedef struct {
    lv_obj_t *img;
    lv_image_dsc_t dsc;
    uint32_t *px; /* w*h ARGB8888 */
    int w, h;
} dash_pix_t;

/* Creates an image object of w x h pixels (buffer from lv_malloc; PSRAM on the board). */
bool dash_pix_create(dash_pix_t *p, lv_obj_t *parent, int w, int h);
void dash_pix_destroy(dash_pix_t *p);
void dash_pix_clear(dash_pix_t *p, uint32_t argb);
/* Call after drawing so LVGL redraws the image. */
void dash_pix_commit(dash_pix_t *p);

void dash_pix_rect(dash_pix_t *p, int x, int y, int w, int h, uint32_t argb);
/* 4-bpp palette sprite (dash_sprites.h), scaled; index 0 is transparent. */
void dash_pix_sprite(dash_pix_t *p, const uint8_t *spr, int sw, int sh, int x, int y, int scale);
/* 5x7 pixel font: digits, ':', '-', ' ', '%', '/'. Returns the drawn width. */
int dash_pix_text(dash_pix_t *p, const char *s, int x, int y, int scale, uint32_t argb);
int dash_pix_text_width(const char *s, int scale);

static inline uint32_t dash_argb(uint32_t rgb)
{
    return 0xFF000000u | rgb;
}
