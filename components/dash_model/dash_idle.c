#include "dash_model.h"

static uint32_t mix(uint32_t n)
{
    n ^= n >> 16;
    n *= 0x7feb352du;
    n ^= n >> 15;
    n *= 0x846ca68bu;
    return n ^ (n >> 16);
}

/* One shared offset per day keeps successive ball episodes at least ten
 * minutes apart, even across different base states. Only some twenty-minute
 * slots are eligible, and all ball play ends well before 21:30. */
static int yarn_start(uint32_t seed, int block, int *duration)
{
    if (block < 0 || block >= 129 || (block & 1) != (int)(seed & 1) ||
        mix(seed ^ (uint32_t)(block/2)) % 3 == 0) return -1;
    *duration = 8+(int)(mix(seed+(uint32_t)block) % 5);
    return block*600+120+(int)((seed >> 4) % 240);
}

dash_idle_state_t dash_idle_at(int64_t utc)
{
    int32_t day = dash_local_day(utc);
    int64_t midnight = (int64_t)day*86400-DASH_TZ_OFFSET_S;
    int sec = (int)(utc-midnight), start = 0, sequence = 0, duration;
    uint32_t seed = mix((uint32_t)day ^ 0x4b696b69u), choice;
    for (;;) {
        choice = mix(seed+(uint32_t)sequence*0x9e3779b9u);
        duration = 60+(int)(choice % 13)*10;
        if (sec < start+duration) break;
        start += duration;
        ++sequence;
    }
    int base = (int)(seed % 3)+(seed & 8 ? sequence : -sequence);
    base = (base % 3+3) % 3;
    dash_idle_state_t out = {
        .kind = (dash_idle_kind_t)base, .base = (dash_idle_kind_t)base,
        .started_at = midnight+start, .base_started_at = midnight+start,
        .elapsed_s = (uint16_t)(sec-start), .duration_s = (uint16_t)duration,
        .base_duration_s = (uint16_t)duration,
    };
    int ball_duration = 0, ball = yarn_start(seed, sec/600, &ball_duration);
    if (ball >= 0 && sec >= ball && sec < ball+ball_duration) {
        out.kind = DASH_IDLE_YARN;
        out.started_at = midnight+ball;
        out.elapsed_s = (uint16_t)(sec-ball);
        out.duration_s = (uint16_t)ball_duration;
        return out;
    }
    /* No extra gesture in a base segment that already contains ball play. */
    for (int block = start/600; block <= (start+duration-1)/600; ++block) {
        ball = yarn_start(seed, block, &ball_duration);
        if (ball >= 0 && ball < start+duration && ball+ball_duration > start) return out;
    }
    if ((choice >> 8) % 4 == 0) {
        int gesture = DASH_IDLE_GROOM+(int)((choice >> 12) % 2);
        int length = gesture == DASH_IDLE_GROOM ? 6+(int)((choice >> 16) % 5)
                                               : 2+(int)((choice >> 16) % 3);
        int offset = 20+(int)((choice >> 20) % 20);
        if (sec >= start+offset && sec < start+offset+length) {
            out.kind = (dash_idle_kind_t)gesture;
            out.started_at = midnight+start+offset;
            out.elapsed_s = (uint16_t)(sec-start-offset);
            out.duration_s = (uint16_t)length;
        }
    }
    return out;
}
