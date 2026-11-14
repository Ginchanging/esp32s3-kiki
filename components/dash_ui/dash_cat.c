#include "dash_cat.h"

#include <math.h>
#include <string.h>

#include "dash_pix.h"
#include "sprites/dash_sprites.h"

#define SCALE 4
#define SIZE (DASH_SPR_W * SCALE) /* 128 */
#define TICK_MS 50

typedef struct {
    uint8_t look;
    uint16_t ms;
    int8_t dx, dy;
} frame_t;

typedef struct {
    const frame_t *f;
    uint8_t n;
    bool mood_ok; /* mood faces/props may be overlaid */
    bool loop;
} anim_t;

#define A(...) ((const frame_t[]){__VA_ARGS__})
#define N(...) (sizeof((const frame_t[]){__VA_ARGS__}) / sizeof(frame_t))
#define ANIM(mood, loop, ...) {A(__VA_ARGS__), (uint8_t)N(__VA_ARGS__), mood, loop}

/* --- routine (looping) --- */
static const anim_t act_anim[DASH_ACT_COUNT] = {
    [DASH_ACT_SLEEP] = ANIM(false, true, {DASH_LOOK_NAP_SLEEP, 1100}, {DASH_LOOK_SLEEP_2, 1100}),
    [DASH_ACT_EXERCISE] = ANIM(true, true, {DASH_LOOK_ROPE_1, 230, 0, -8}, {DASH_LOOK_ROPE_2, 230}),
    [DASH_ACT_BREAKFAST] = ANIM(true, true, {DASH_LOOK_MILK, 450}, {DASH_LOOK_MILK_2, 450}),
    [DASH_ACT_CLASS] = ANIM(true, true, {DASH_LOOK_READ, 2600}, {DASH_LOOK_READ_2, 350}, {DASH_LOOK_READ, 1800},
                            {DASH_LOOK_BLINK, 140}),
    [DASH_ACT_LUNCH] = ANIM(true, true, {DASH_LOOK_FISH, 380}, {DASH_LOOK_FISH_2, 380}),
    [DASH_ACT_NAP] = ANIM(false, true, {DASH_LOOK_NAP_SLEEP, 1100}, {DASH_LOOK_SLEEP_2, 1100}),
    [DASH_ACT_CODE] = ANIM(true, true, {DASH_LOOK_CODE, 320}, {DASH_LOOK_CODE_2, 320}),
    [DASH_ACT_DINNER] = ANIM(true, true, {DASH_LOOK_DINNER, 400}, {DASH_LOOK_MILK_2, 400}),
    [DASH_ACT_FREE] = ANIM(true, true, {DASH_LOOK_YARN, 450}, {DASH_LOOK_TAIL, 450}, {DASH_LOOK_YARN, 450},
                           {DASH_LOOK_TAIL, 450}, {DASH_LOOK_IDLE, 2400}, {DASH_LOOK_BLINK, 140},
                           {DASH_LOOK_IDLE, 1800}, {DASH_LOOK_HAPPY, 1400}),
    [DASH_ACT_WASH] = ANIM(true, true, {DASH_LOOK_WASH, 420}, {DASH_LOOK_WASH_2, 420}, {DASH_LOOK_WASH, 420},
                           {DASH_LOOK_YAWN, 1200}),
    [DASH_ACT_SUNBATHE] = ANIM(false, true, {DASH_LOOK_SUNBATHE, 1300}, {DASH_LOOK_SUN_2, 1300}),
};

static const anim_t pomo_focus_anim = ANIM(true, true, {DASH_LOOK_FOCUS, 300}, {DASH_LOOK_FOCUS_2, 300});
static const anim_t pomo_break_anim =
    ANIM(true, true, {DASH_LOOK_YAWN, 1400}, {DASH_LOOK_TAIL, 900}, {DASH_LOOK_IDLE, 1600}, {DASH_LOOK_BLINK, 140});

/* --- reactions (play once). dy for jumps is computed as an arc, see tick() --- */
static const anim_t react_anim[DASH_CAT_REACT_COUNT] = {
    [DASH_CAT_REACT_CI_PASS] = ANIM(false, false, {DASH_LOOK_CI_PASS, 160}, {DASH_LOOK_CI_PASS_2, 160},
                                    {DASH_LOOK_CI_PASS, 160}, {DASH_LOOK_CI_PASS_2, 160}, {DASH_LOOK_CI_PASS, 160},
                                    {DASH_LOOK_CI_PASS_2, 160}, {DASH_LOOK_HAPPY, 1400}),
    [DASH_CAT_REACT_CI_FAIL] = ANIM(false, false, {DASH_LOOK_PUFFED, 120, -2}, {DASH_LOOK_PUFFED_2, 120, 2},
                                    {DASH_LOOK_PUFFED, 120, -2}, {DASH_LOOK_PUFFED_2, 120, 2},
                                    {DASH_LOOK_PUFFED, 400}, {DASH_LOOK_WORRIED, 1600}),
    [DASH_CAT_REACT_CI_START] = ANIM(false, false, {DASH_LOOK_ALERT, 700}, {DASH_LOOK_TAIL, 500},
                                     {DASH_LOOK_ALERT, 600}),
    [DASH_CAT_REACT_SERVER_DOWN] = ANIM(false, false, {DASH_LOOK_PUFFED, 100, -3}, {DASH_LOOK_PUFFED_2, 100, 3},
                                        {DASH_LOOK_PUFFED, 100, -3}, {DASH_LOOK_PUFFED_2, 100, 3},
                                        {DASH_LOOK_PUFFED, 100, -3}, {DASH_LOOK_PUFFED_2, 100, 3},
                                        {DASH_LOOK_ALERT, 900}, {DASH_LOOK_WORRIED, 1200}),
    [DASH_CAT_REACT_SERVER_UP] = ANIM(false, false, {DASH_LOOK_MEOW, 1600}, {DASH_LOOK_HAPPY, 1000}),
    [DASH_CAT_REACT_MUSE] = ANIM(false, false, {DASH_LOOK_MUSE, 3500}, {DASH_LOOK_MUSE, 3500, 0, -2}),
    [DASH_CAT_REACT_TOUCH] = ANIM(false, false, {DASH_LOOK_MEOW, 1300}),
    [DASH_CAT_REACT_MORNING] = ANIM(false, false, {DASH_LOOK_YAWN, 1300}, {DASH_LOOK_HAPPY, 1200}),
    [DASH_CAT_REACT_POMO_DONE] = ANIM(false, false, {DASH_LOOK_CI_PASS, 160}, {DASH_LOOK_CI_PASS_2, 160},
                                      {DASH_LOOK_CI_PASS, 160}, {DASH_LOOK_CI_PASS_2, 160},
                                      {DASH_LOOK_HAPPY, 1500}),
};

static struct {
    dash_pix_t pix;
    lv_obj_t *shadow;
    lv_timer_t *timer;
    int x, y;
    dash_cat_ctx_t ctx;
    const anim_t *anim; /* currently playing */
    int frame;
    uint32_t frame_ms; /* time spent in the current frame */
    dash_cat_react_t react;
    uint32_t react_ms;
    int drawn_look, drawn_mood;
    int forced; /* -1 = auto */
} cat = {.drawn_look = -1, .forced = -1};

static bool is_face(int id)
{
    return id >= DASH_SPR_FACE_BLINK && id <= DASH_SPR_FACE_YAWN;
}

static void compose(int look, dash_cat_mood_t mood)
{
    const dash_look_t *L = &dash_looks[look];
    dash_pix_clear(&cat.pix, 0);
    int mood_face = -1, mood_prop = -1;
    if (mood == DASH_CAT_MOOD_WORRIED) {
        mood_face = DASH_SPR_FACE_WORRIED, mood_prop = DASH_SPR_PROP_SWEAT;
    } else if (mood == DASH_CAT_MOOD_NERVOUS) {
        mood_face = DASH_SPR_FACE_SHOCKED, mood_prop = DASH_SPR_PROP_BANG;
    } else if (mood == DASH_CAT_MOOD_CONFUSED) {
        mood_face = DASH_SPR_FACE_CONFUSED, mood_prop = DASH_SPR_PROP_QUESTION;
    }
    for (int i = 0; i < L->n; ++i) {
        int id = L->layer[i];
        if (mood_face >= 0 && is_face(id)) {
            id = mood_face;
        }
        dash_pix_sprite(&cat.pix, dash_sprites[id], DASH_SPR_W, DASH_SPR_H, 0, 0, SCALE);
    }
    if (mood_prop >= 0) {
        dash_pix_sprite(&cat.pix, dash_sprites[mood_prop], DASH_SPR_W, DASH_SPR_H, 0, 0, SCALE);
    }
    dash_pix_commit(&cat.pix);
}

static const anim_t *routine_anim(void)
{
    if (cat.ctx.pomo == DASH_POMO_FOCUS) {
        return &pomo_focus_anim;
    }
    if (cat.ctx.pomo == DASH_POMO_BREAK) {
        return &pomo_break_anim;
    }
    return &act_anim[cat.ctx.activity < DASH_ACT_COUNT ? cat.ctx.activity : DASH_ACT_FREE];
}

static void start_anim(const anim_t *a)
{
    if (cat.anim != a) {
        cat.anim = a;
        cat.frame = 0;
        cat.frame_ms = 0;
    }
}

static void place(int dx, int dy)
{
    lv_obj_set_pos(cat.pix.img, cat.x + dx, cat.y + dy);
}

static void tick(lv_timer_t *t)
{
    if (!cat.anim) {
        start_anim(routine_anim());
    }
    cat.frame_ms += TICK_MS;
    if (cat.react) {
        cat.react_ms += TICK_MS;
    }
    const frame_t *f = &cat.anim->f[cat.frame];
    if (cat.frame_ms >= f->ms) {
        cat.frame_ms = 0;
        if (++cat.frame >= cat.anim->n) {
            if (cat.anim->loop) {
                cat.frame = 0;
            } else { /* reaction finished: back to the routine */
                cat.react = DASH_CAT_REACT_NONE;
                cat.anim = NULL;
                start_anim(routine_anim());
            }
        }
        f = &cat.anim->f[cat.frame];
    }
    int look = cat.forced >= 0 ? cat.forced : f->look;
    int mood = cat.react || !cat.anim->mood_ok || cat.forced >= 0 ? DASH_CAT_MOOD_HAPPY : cat.ctx.mood;
    if (look != cat.drawn_look || mood != cat.drawn_mood) {
        compose(look, (dash_cat_mood_t)mood);
        cat.drawn_look = look;
        cat.drawn_mood = mood;
    }
    int dx = f->dx, dy = f->dy;
    if (cat.react == DASH_CAT_REACT_CI_PASS || cat.react == DASH_CAT_REACT_POMO_DONE) {
        uint32_t jump_ms = 320, jumps = cat.react == DASH_CAT_REACT_CI_PASS ? 3 : 2;
        if (cat.react_ms < jump_ms * jumps) { /* three hops: |sin| arc, 22 px high */
            dy = -(int)(16.0f * fabsf(sinf(3.14159265f * (float)(cat.react_ms % jump_ms) / jump_ms)));
        }
    }
    place(dx, dy);
}

lv_obj_t *dash_cat_create(lv_obj_t *parent, int x, int y)
{
    cat.x = x;
    cat.y = y;
    cat.drawn_look = -1;
    cat.anim = NULL;
    cat.shadow = lv_obj_create(parent);
    lv_obj_remove_style_all(cat.shadow);
    lv_obj_set_size(cat.shadow, 84, 12);
    lv_obj_set_pos(cat.shadow, x + (SIZE - 84) / 2, y + SIZE - 10);
    lv_obj_set_style_radius(cat.shadow, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(cat.shadow, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(cat.shadow, LV_OPA_30, 0);
    if (!dash_pix_create(&cat.pix, parent, SIZE, SIZE)) {
        return NULL;
    }
    lv_obj_set_pos(cat.pix.img, x, y);
    lv_obj_set_clickable(cat.pix.img, true);
    cat.timer = lv_timer_create(tick, TICK_MS, NULL);
    tick(cat.timer);
    return cat.pix.img;
}

void dash_cat_destroy(void)
{
    if (cat.timer) {
        lv_timer_delete(cat.timer);
        cat.timer = NULL;
    }
    dash_pix_destroy(&cat.pix); /* the image object itself is deleted with its screen */
    cat.anim = NULL;
}

void dash_cat_set_ctx(const dash_cat_ctx_t *ctx)
{
    cat.ctx = *ctx;
    if (!cat.react) {
        start_anim(routine_anim());
    }
}

void dash_cat_react(dash_cat_react_t r)
{
    if (r <= DASH_CAT_REACT_NONE || r >= DASH_CAT_REACT_COUNT) {
        return;
    }
    cat.react = r;
    cat.react_ms = 0;
    cat.anim = NULL;
    start_anim(&react_anim[r]);
}

dash_cat_react_t dash_cat_reacting(void)
{
    return cat.react;
}

bool dash_cat_force_look(const char *name)
{
    if (!name || !strcmp(name, "auto")) {
        cat.forced = -1;
        return true;
    }
    for (int i = 0; i < DASH_LOOK_COUNT; ++i) {
        if (!strcmp(dash_look_names[i], name)) {
            cat.forced = i;
            return true;
        }
    }
    return false;
}

void dash_cat_draw_look_into(void *pix, int look, int x, int y, int scale)
{
    const dash_look_t *L = &dash_looks[look];
    for (int i = 0; i < L->n; ++i) {
        dash_pix_sprite((dash_pix_t *)pix, dash_sprites[L->layer[i]], DASH_SPR_W, DASH_SPR_H, x, y, scale);
    }
}
