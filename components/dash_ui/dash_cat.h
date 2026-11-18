/* Kiki, the pixel cat on the home page: routine animations, moods and reactions. */
#pragma once

#include <stdbool.h>

#include "dash_model.h"
#include "lvgl.h"

typedef enum {
    DASH_CAT_REACT_NONE = 0,
    DASH_CAT_REACT_CI_PASS,     /* jumps for joy with confetti */
    DASH_CAT_REACT_CI_FAIL,     /* startled, fur puffs up */
    DASH_CAT_REACT_CI_START,    /* ears up */
    DASH_CAT_REACT_SERVER_DOWN, /* panics */
    DASH_CAT_REACT_SERVER_UP,   /* relieved, heart */
    DASH_CAT_REACT_MUSE,        /* holds up a sign */
    DASH_CAT_REACT_TOUCH,       /* meow */
    DASH_CAT_REACT_MORNING,     /* yawn and stretch */
    DASH_CAT_REACT_POMO_DONE,   /* cheers */
    DASH_CAT_REACT_PET,         /* A: leans over, shows tummy, settles and gets up */
    DASH_CAT_REACT_COUNT
} dash_cat_react_t;

typedef enum {
    DASH_CAT_MOOD_HAPPY = 0,
    DASH_CAT_MOOD_WORRIED,  /* latest CI failed */
    DASH_CAT_MOOD_NERVOUS,  /* a server is offline */
    DASH_CAT_MOOD_CONFUSED, /* data stale / offline */
} dash_cat_mood_t;

typedef struct {
    dash_activity_t activity;
    dash_cat_mood_t mood;
    dash_pomo_phase_t pomo;
    dash_idle_state_t idle;
} dash_cat_ctx_t;

/* Creates the 128x128 cat at (x, y) inside parent. Returns the clickable image object. */
lv_obj_t *dash_cat_create(lv_obj_t *parent, int x, int y);
void dash_cat_destroy(void);
void dash_cat_set_ctx(const dash_cat_ctx_t *ctx);
void dash_cat_react(dash_cat_react_t r);
bool dash_cat_pet(void); /* low priority, with a two-second cooldown after finish/interruption */
dash_cat_react_t dash_cat_reacting(void); /* NONE when back to the routine */
/* Testing: force a look by name ("fish", "rope 1", ...) or NULL/"auto" to resume. */
bool dash_cat_force_look(const char *name);
/* Draw one look into a separate small image (pomodoro page), scale 1..4. */
void dash_cat_draw_look_into(void *dash_pix, int look, int x, int y, int scale);
