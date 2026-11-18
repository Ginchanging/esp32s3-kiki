/* Pure button state machine; input is already debounced. Rotation wins on release. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool down, turned;
    uint32_t pressed_at;
} knob_button_t;
typedef enum { KNOB_BUTTON_NONE = -1, KNOB_BUTTON_CLICK, KNOB_BUTTON_LONG,
               KNOB_BUTTON_RELEASE } knob_button_action_t;

static inline knob_button_action_t knob_button_update(knob_button_t *b, bool down,
                                                      int steps, uint32_t now)
{
    bool was_down = b->down;
    if (down && !was_down) {
        b->pressed_at = now;
        b->turned = false;
    }
    if (steps && (down || was_down)) b->turned = true;
    b->down = down;
    if (was_down && !down) {
        if (b->turned) return KNOB_BUTTON_RELEASE;
        return now - b->pressed_at >= 600 ? KNOB_BUTTON_LONG : KNOB_BUTTON_CLICK;
    }
    return KNOB_BUTTON_NONE;
}
