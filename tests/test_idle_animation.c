/* Test the rendered shared LVGL cat, including interruption and recreation. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "dash_cat.h"
#include "dash_pix.h"
#include "sprites/dash_sprites.h"

static uint32_t now_ms;
static lv_obj_t *image;
static uint32_t pixels[128*128];

static uint32_t clock_ms(void) { return now_ms; }
static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *px)
{
    (void)area;
    (void)px;
    lv_display_flush_ready(display);
}

static void run(unsigned ms)
{
    for (unsigned i = 0; i < ms; i += 50) {
        now_ms += 50;
        lv_timer_handler();
    }
}

static bool shows(int look)
{
    dash_pix_t expected = {.px = pixels, .w = 128, .h = 128};
    memset(pixels, 0, sizeof(pixels));
    dash_cat_draw_look_into(&expected, look, 0, 0, 4);
    const lv_image_dsc_t *actual = lv_image_get_src(image);
    return !memcmp(actual->data, pixels, sizeof(pixels));
}

int main(void)
{
    lv_init();
    lv_tick_set_cb(clock_ms);
    lv_display_t *display = lv_display_create(240, 320);
    static uint8_t buf[240*16*2];
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display, buf, NULL, sizeof(buf), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush);
    lv_obj_t *root = lv_screen_active();
    image = dash_cat_create(root, 0, 0);
    assert(image);
    dash_cat_ctx_t ctx = {.activity = DASH_ACT_FREE, .mood = DASH_CAT_MOOD_HAPPY,
        .pomo = DASH_POMO_IDLE,
        .idle = {.kind = DASH_IDLE_SIT, .started_at = 10000, .elapsed_s = 16}};
    dash_cat_set_ctx(&ctx);
    run(50);
    assert(shows(DASH_LOOK_BLINK)); /* Rejoins the blink, not frame zero. */
    run(150);
    assert(shows(DASH_LOOK_IDLE));

    ctx.idle.kind = DASH_IDLE_WINDOW;
    ctx.idle.started_at += 120;
    ctx.idle.elapsed_s = 21;
    dash_cat_set_ctx(&ctx);
    run(50);
    assert(shows(DASH_LOOK_WINDOW_TAIL));
    /* Screen/language rebuilding recreates the cat from the same wall time. */
    dash_cat_destroy();
    lv_obj_clean(root);
    image = dash_cat_create(root, 0, 0);
    dash_cat_set_ctx(&ctx);
    run(50);
    assert(shows(DASH_LOOK_WINDOW_TAIL));
    run(600);
    assert(shows(DASH_LOOK_WINDOW));

    ctx.idle.kind = DASH_IDLE_YARN;
    ctx.idle.started_at += 120;
    ctx.idle.elapsed_s = 1;
    dash_cat_set_ctx(&ctx);
    run(50);
    assert(shows(DASH_LOOK_YARN_2));
    dash_cat_react(DASH_CAT_REACT_MUSE);
    run(50);
    assert(shows(DASH_LOOK_MUSE));
    /* The real clock changes the underlying state during the notification. */
    ctx.idle.kind = DASH_IDLE_LIE;
    ctx.idle.started_at += 120;
    ctx.idle.elapsed_s = 12;
    dash_cat_set_ctx(&ctx);
    run(7000);
    assert(dash_cat_reacting() == DASH_CAT_REACT_NONE);
    assert(shows(DASH_LOOK_REST) || shows(DASH_LOOK_REST_2) || shows(DASH_LOOK_REST_BLINK));

    assert(dash_cat_pet());
    run(50);
    assert(shows(DASH_LOOK_PET_LEAN));
    assert(!dash_cat_pet()); /* a repeated pet cannot restart its first frame */
    run(650);
    assert(shows(DASH_LOOK_PET_BELLY));
    dash_cat_react(DASH_CAT_REACT_CI_FAIL);
    assert(!dash_cat_pet());
    run(50);
    assert(shows(DASH_LOOK_PUFFED)); /* real notifications preempt the belly rub */
    run(2700);
    assert(dash_cat_pet());
    run(4000);
    assert(dash_cat_reacting() == DASH_CAT_REACT_NONE);
    assert(shows(DASH_LOOK_REST) || shows(DASH_LOOK_REST_2) || shows(DASH_LOOK_REST_BLINK));
    assert(!dash_cat_pet());
    run(2050);
    assert(dash_cat_pet());
    run(700);
    dash_cat_destroy();
    lv_obj_clean(root);
    image = dash_cat_create(root, 0, 0);
    dash_cat_set_ctx(&ctx);
    run(50);
    assert(shows(DASH_LOOK_PET_BELLY)); /* rebuilding retains the reaction phase */
    run(3400);

    ctx.pomo = DASH_POMO_FOCUS;
    dash_cat_set_ctx(&ctx);
    run(50);
    assert(shows(DASH_LOOK_FOCUS));
    ctx.pomo = DASH_POMO_BREAK;
    dash_cat_set_ctx(&ctx);
    run(50);
    assert(shows(DASH_LOOK_YAWN));
    ctx.pomo = DASH_POMO_IDLE;
    ctx.activity = DASH_ACT_SLEEP;
    dash_cat_set_ctx(&ctx);
    run(50);
    assert(shows(DASH_LOOK_NAP_SLEEP));
    ctx.activity = DASH_ACT_WASH;
    dash_cat_set_ctx(&ctx);
    run(50);
    assert(shows(DASH_LOOK_WASH));

    ctx.activity = DASH_ACT_FREE;
    ctx.idle = (dash_idle_state_t){0}; /* no valid clock: quiet only */
    dash_cat_set_ctx(&ctx);
    for (int i = 0; i < 120; ++i) {
        run(1000);
        assert(shows(DASH_LOOK_IDLE) || shows(DASH_LOOK_BLINK) || shows(DASH_LOOK_TAIL));
    }
    assert(dash_cat_force_look("fish"));
    run(50);
    assert(shows(DASH_LOOK_FISH));
    assert(dash_cat_force_look("wash"));
    run(50);
    assert(shows(DASH_LOOK_WASH));
    assert(dash_cat_force_look("auto"));
    dash_cat_destroy();
    puts("clock phase, recreation, reaction return, pomodoro/sleep priority, no-clock fallback and legacy looks passed");
    return 0;
}
