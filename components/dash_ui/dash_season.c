/* Small code-native pixel decorations. Kiki's generated art is unchanged. */
#include "dash_season.h"
#include "dash_flight_ui.h"
#include "dash_pix.h"
#include "dash_theme.h"
#include <string.h>

static struct {
    dash_pix_t wall, sky, floor;
    lv_obj_t *plant;
    lv_timer_t *timer;
    uint16_t flags;
    unsigned phase;
    bool night, drawn;
} season;

static void r(dash_pix_t *p, int x, int y, int w, int h, uint32_t c)
{
    dash_pix_rect(p, x, y, w, h, dash_argb(c));
}

static void star(dash_pix_t *p, int x, int y, uint32_t c)
{
    r(p, x+2, y, 2, 6, c);
    r(p, x, y+2, 6, 2, c);
}

static void lantern(dash_pix_t *p, int x, int y, bool rabbit)
{
    uint32_t c = rabbit ? 0xF7E6CA : 0xE94C54;
    r(p, x+7, y-4, 2, 5, T_FRAME);
    if (rabbit) {
        r(p, x+3, y-7, 3, 9, c); r(p, x+10, y-7, 3, 9, c);
        r(p, x+4, y-5, 1, 5, 0xE994A3); r(p, x+11, y-5, 1, 5, 0xE994A3);
    }
    r(p, x+3, y, 10, 3, 0xFFD166);
    r(p, x+1, y+3, 14, 14, c); r(p, x, y+6, 16, 8, c);
    if (rabbit) {
        r(p, x+4, y+8, 2, 2, T_BG); r(p, x+11, y+8, 2, 2, T_BG);
        r(p, x+7, y+11, 2, 2, 0xE994A3);
    } else {
        r(p, x+4, y+4, 2, 12, 0xFF8472); r(p, x+10, y+4, 2, 12, 0xC83650);
    }
    r(p, x+3, y+17, 10, 3, 0xFFD166); r(p, x+7, y+20, 2, 7, 0xFFD166);
    r(p, x+5, y+26, 6, 3, 0xE94C54);
}

static void wall(void)
{
    dash_pix_t *p = &season.wall;
    if (!p->px) return;
    dash_pix_clear(p, 0);
    uint16_t f = season.flags;
    if (f & DASH_HOL_SPRING) {
        lantern(p, 5, 9, false); lantern(p, 85, 9, false);
        r(p, 2, 44, 6, 39, 0xC83650); r(p, 103, 44, 6, 39, 0xC83650);
        for (int y = 48; y < 78; y += 7) {
            r(p, 3, y, 4, 2, 0xFFD166); r(p, 104, y, 4, 2, 0xFFD166);
        }
    }
    if (f & DASH_HOL_LANTERN) {
        lantern(p, 7, 11, false); lantern(p, 87, 11, false);
        r(p, 41, 137, 2, 20, 0xC78A62); r(p, 29, 157, 27, 4, T_FRAME);
        lantern(p, 34, 116, true);
    }
    if (f & DASH_HOL_DRAGON) {
        r(p, 3, 38, 2, 28, 0x9A794A);
        for (int y = 39; y < 63; y += 6) {
            r(p, 0, y, 4, 3, 0x6CBB79); r(p, 4, y+2, 4, 3, 0x4C9963);
        }
        r(p, 1, 64, 7, 3, 0xE99472);
    }
    if (f & DASH_HOL_CHONGYANG) {
        r(p, 32, 148, 25, 13, 0x9C6B4E); r(p, 29, 145, 31, 5, 0x7A5240);
        for (int i = 0; i < 3; ++i) {
            int x = 27+i*12, y = 120+(i%2)*10;
            r(p, x+6, y+8, 2, 20, 0x589769);
            r(p, x+3, y+3, 10, 10, 0xF4C95D); r(p, x, y+6, 16, 4, 0xFFD77D);
            r(p, x+6, y, 4, 16, 0xFFD77D); r(p, x+6, y+6, 4, 4, 0xD58B47);
        }
    } else if (f & DASH_HOL_CHRISTMAS) {
        r(p, 40, 141, 6, 20, 0x87593D); r(p, 31, 158, 24, 6, 0xB7755A);
        for (int i = 0; i < 3; ++i) {
            int y = 112+i*12;
            for (int j = 0; j < 8; ++j) r(p, 40-j*2, y+j*2, 6+j*4, 2, i%2 ? 0x3E8962 : 0x4C9D71);
        }
        star(p, 40, 106, 0xFFD166);
        static const int ornaments[][2] = {{40,122},{49,134},{34,140},{43,147},{27,150},{54,150}};
        for (int i = 0; i < 6; ++i) r(p, ornaments[i][0], ornaments[i][1], 3, 3, i%2 ? 0xFFD166 : 0xF27879);
    } else if (f & DASH_HOL_THANKSGIVING) {
        r(p, 39, 127, 3, 27, 0xA77B50);
        for (int i = 0; i < 5; ++i) {
            int x = 24+(i%3)*12, y = 113+(i/3)*17;
            r(p, x+4, y, 4, 16, i%2 ? 0xE8AF58 : 0xD87C52);
            r(p, x, y+4, 12, 7, i%2 ? 0xE8AF58 : 0xD87C52);
        }
        r(p, 29, 151, 27, 11, 0x9C6B4E);
    }
    if (f & DASH_HOL_NATIONAL) {
        r(p, 20, 3, 63, 4, 0xBD354B);
        for (int i = 0; i < 5; ++i) {
            int x = 21+i*13;
            r(p, x, 6, 9, 7, 0xE95156); r(p, x+3, 8, 2, 2, 0xFFD166);
        }
    }
    dash_pix_commit(p);
    bool replace = (f & (DASH_HOL_CHONGYANG | DASH_HOL_CHRISTMAS | DASH_HOL_THANKSGIVING | DASH_HOL_LANTERN)) != 0;
    lv_obj_set_hidden(season.plant, replace);
    lv_obj_set_hidden(p->img, !f);
}

static void sky(void)
{
    dash_pix_t *p = &season.sky;
    if (!p->px) return;
    dash_pix_clear(p, 0);
    uint16_t f = season.flags;
    if (f & DASH_HOL_CHONGYANG) {
        for (int x = 0; x < 84; x += 2) {
            int hill = 44+((x/2)%20 < 10 ? (x/2)%10 : 10-(x/2)%10);
            r(p, x, hill, 2, 60-hill, season.night ? 0x39465B : 0x83ABA0);
            int front = 50+((x/2+9)%16 < 8 ? (x/2+9)%8 : 8-(x/2+9)%8);
            r(p, x, front, 2, 60-front, season.night ? 0x283949 : 0x609C8C);
        }
    }
    if (f & DASH_HOL_QIXI) {
        star(p, 16, 13, season.phase%2 ? 0xFFE5A0 : 0xFFF5DA);
        star(p, 62, 8, season.phase%2 ? 0xCFD9FF : 0xF3D8F1);
        for (int i = 0; i < 14; ++i) {
            int y = 47+(i < 7 ? 7-i : i-7);
            r(p, 14+i*4, y, 3, 2, season.night ? 0xC498C1 : 0xA5789B);
        }
        r(p, 37, 34, 3, 3, 0xEF99B0); r(p, 42, 34, 3, 3, 0xEF99B0);
        r(p, 38, 37, 6, 3, 0xEF99B0); r(p, 40, 40, 2, 2, 0xEF99B0);
    }
    if (f & DASH_HOL_HALLOWEEN) {
        for (int i = 0; i < 2; ++i) {
            int x = 12+i*38, y = 16+i*13;
            uint32_t c = season.night ? 0x9183B5 : 0x504767;
            r(p, x, y, 4, 2, c); r(p, x+10, y, 4, 2, c);
            r(p, x+3, y+2, 8, 2, c); r(p, x+5, y+4, 4, 3, c);
        }
    }
    if (f & DASH_HOL_SOLSTICE) {
        for (int i = 0; i < 12; ++i) {
            int x = (i*29+(int)season.phase*2)%82, y = (i*17+(int)season.phase*5)%58;
            r(p, x, y, 2, 2, 0xF2F0E8);
            if (i%3 == 0) star(p, x-2, y-2, 0xF2F0E8);
        }
    }
    if ((f & DASH_HOL_NATIONAL) && season.night) {
        static const int burst[][2] = {{20,19},{61,29}};
        for (int i = 0; i < 2; ++i) {
            int radius = 3+(int)(season.phase+i*2)%4*3;
            uint32_t c = i ? 0xF0C567 : 0xEB9DA8;
            for (int d = -1; d <= 1; ++d) for (int e = -1; e <= 1; ++e) {
                if (!d && !e) continue;
                r(p, burst[i][0]+d*radius, burst[i][1]+e*radius, 2, 2, c);
            }
            star(p, burst[i][0]-2, burst[i][1]-2, 0xFFE5A0);
        }
    }
    dash_pix_commit(p);
    lv_obj_set_hidden(p->img, !f);
}

static void pumpkin(dash_pix_t *p, int x, int y, bool face)
{
    r(p, x+12, y, 4, 5, 0x6E9867);
    r(p, x+3, y+5, 23, 19, 0xF5A04D); r(p, x, y+9, 29, 11, 0xE78A42);
    r(p, x+7, y+6, 3, 17, 0xFFB75E); r(p, x+19, y+6, 2, 17, 0xCC6A39);
    if (face) {
        r(p, x+6, y+10, 4, 4, 0xFFDF87); r(p, x+19, y+10, 4, 4, 0xFFDF87);
        r(p, x+11, y+17, 8, 3, 0x5B3542); r(p, x+9, y+16, 2, 2, 0x5B3542);
        r(p, x+19, y+16, 2, 2, 0x5B3542);
    }
}

static void floor_art(void)
{
    dash_pix_t *p = &season.floor;
    if (!p->px) return;
    dash_pix_clear(p, 0);
    uint16_t f = season.flags;
    if (f & DASH_HOL_BIRTHDAY) {
        r(p, 6, 37, 47, 4, 0xB6A9C7); r(p, 9, 22, 41, 16, 0xE9A2A1);
        r(p, 9, 19, 41, 7, 0xFFF0D2); r(p, 9, 31, 41, 3, 0xC7788A);
        for (int i = 0; i < 5; ++i) r(p, 12+i*8, 24, 3, 5+i%2*2, 0xFFF0D2);
        for (int i = 0; i < 3; ++i) {
            int x = 17+i*12;
            r(p, x, 9, 3, 10, i%2 ? 0x88BED1 : 0xF1CD75);
            r(p, x, 4+(int)(season.phase+i)%2, 3, 4, 0xFFD166);
            r(p, x+1, 3+(int)(season.phase+i)%2, 1, 2, 0xFFF0D2);
        }
    } else if (f & DASH_HOL_LANTERN) {
        r(p, 7, 22, 45, 6, 0xC1A5BD); r(p, 12, 28, 35, 10, 0x866B9E);
        r(p, 17, 38, 25, 3, 0x5E4C6E);
        for (int i = 0; i < 4; ++i) {
            int x = 11+i*10, y = 20-(i%2)*3;
            r(p, x, y, 9, 7, 0xFFF0D2); r(p, x+2, y-2, 5, 2, 0xFFF0D2);
        }
        r(p, 20, 8, 2, 6, 0xB6A9C7); r(p, 33, 5, 2, 7, 0xB6A9C7);
    } else if (f & DASH_HOL_DRAGON) {
        r(p, 8, 28, 44, 11, 0xB68A56); r(p, 11, 37, 38, 3, 0x805D45);
        for (int i = 0; i < 3; ++i) {
            int x = 10+i*13, y = 11+(i%2)*4;
            for (int j = 0; j < 8; ++j) r(p, x+7-j, y+j*2, 2+j*2, 2, j%2 ? 0x74AD75 : 0x5B9768);
            r(p, x+7, y+3, 2, 14, 0xD7C593); r(p, x+1, y+13, 14, 2, 0xD7C593);
        }
    } else if (f & DASH_HOL_HALLOWEEN) {
        pumpkin(p, 15, 13, true);
    } else if (f & DASH_HOL_THANKSGIVING) {
        pumpkin(p, 6, 8, false);
        r(p, 38, 10, 7, 20, 0xF2C85E); r(p, 36, 19, 2, 14, 0x7B9F67);
        r(p, 8, 29, 45, 10, 0xA8774D); r(p, 8, 29, 45, 3, 0xD7AA6F);
        for (int x = 12; x < 50; x += 8) r(p, x, 33, 3, 5, 0x815B43);
    } else if (f & DASH_HOL_CHRISTMAS) {
        r(p, 9, 16, 24, 23, 0xC95869); r(p, 18, 16, 4, 23, 0xF5D18A);
        r(p, 9, 23, 24, 4, 0xF5D18A); r(p, 18, 10, 4, 5, 0xF5D18A);
        r(p, 13, 10, 6, 3, 0xF5D18A); r(p, 22, 10, 6, 3, 0xF5D18A);
        r(p, 35, 24, 18, 15, 0x78B9A5); r(p, 42, 24, 3, 15, 0xF1E5C7);
    } else if (f & DASH_HOL_NATIONAL) {
        r(p, 14, 5, 2, 32, 0xC9B7B1); r(p, 16, 5, 30, 19, 0xDC5260);
        star(p, 19, 9, 0xFFD166);
        r(p, 29, 8, 2, 2, 0xFFD166); r(p, 32, 11, 2, 2, 0xFFD166);
        r(p, 32, 15, 2, 2, 0xFFD166); r(p, 29, 19, 2, 2, 0xFFD166);
        r(p, 9, 37, 15, 3, T_FRAME);
    } else if (f & DASH_HOL_SPRING) {
        r(p, 12, 13, 18, 25, 0xD84B59); r(p, 17, 20, 8, 8, 0xFFD166);
        r(p, 34, 20, 15, 18, 0xB9364B); r(p, 38, 25, 7, 5, 0xFFD166);
    }
    dash_pix_commit(p);
    lv_obj_set_hidden(p->img, !f);
}

static void animate(lv_timer_t *timer)
{
    if (!season.flags || dash_ui_get_page() != DASH_PAGE_ROOM || dash_flight_ui_scene_active()) return;
    season.phase = (season.phase+1)%16;
    if (season.flags & (DASH_HOL_SOLSTICE | DASH_HOL_QIXI | DASH_HOL_NATIONAL)) sky();
    if (season.flags & DASH_HOL_BIRTHDAY) floor_art();
}

void dash_season_create(lv_obj_t *home, lv_obj_t *sky_obj, lv_obj_t *plant)
{
    memset(&season, 0, sizeof(season));
    season.plant = plant;
    if (dash_pix_create(&season.wall, home, 112, 176)) lv_obj_set_pos(season.wall.img, 0, 0);
    if (dash_pix_create(&season.sky, sky_obj, 84, 60)) lv_obj_set_pos(season.sky.img, 0, 0);
    if (dash_pix_create(&season.floor, home, 60, 42)) lv_obj_set_pos(season.floor.img, 80, 136);
    season.timer = lv_timer_create(animate, 500, NULL);
}

void dash_season_foreground(void)
{
    if (season.floor.img) lv_obj_move_to_index(season.floor.img, -1);
}

void dash_season_update(int64_t now, bool clock_valid, bool night)
{
    uint16_t flags = clock_valid ? dash_holidays_at(now) : 0;
    if (season.drawn && flags == season.flags && night == season.night) return;
    season.flags = flags;
    season.night = night;
    season.drawn = true;
    wall(); sky(); floor_art();
}

void dash_season_destroy(void)
{
    if (season.timer) lv_timer_delete(season.timer);
    dash_pix_destroy(&season.wall);
    dash_pix_destroy(&season.sky);
    dash_pix_destroy(&season.floor);
    memset(&season, 0, sizeof(season));
}
