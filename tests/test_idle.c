#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "dash_model.h"

#define FIRST_DAY INT64_C(1794499200) /* 2026-11-13 00:00 UTC+8 */
#define EVENING (18*3600+40*60)
#define WASH (22*3600+50*60)

static void same_state(dash_idle_state_t a, dash_idle_state_t b)
{
    assert(a.kind == b.kind && a.base == b.base);
    assert(a.started_at == b.started_at && a.base_started_at == b.base_started_at);
    assert(a.elapsed_s == b.elapsed_s && a.duration_s == b.duration_s);
    assert(a.base_duration_s == b.base_duration_s);
}

int main(int argc, char **argv)
{
    bool samples = argc > 1 && !strcmp(argv[1], "--samples");
    bool timeline = argc > 1 && !strcmp(argv[1], "--timeline");
    int seen[DASH_IDLE_COUNT] = {0};
    int64_t sample[DASH_IDLE_COUNT] = {0};
    int quiet = 0, total = 0, yarn_episodes = 0;
    for (int d = 0; d < 14; ++d) {
        int64_t midnight = FIRST_DAY+(int64_t)d*86400;
        dash_idle_state_t prev = dash_idle_at(midnight);
        int64_t last_yarn = 0;
        /* Whole-day sampling also covers date boundaries and morning free
         * time. The evening is checked at one-second resolution below. */
        for (int s = 0; s < 86400; s += 10) {
            dash_idle_state_t a = dash_idle_at(midnight+s);
            assert(a.kind >= 0 && a.kind < DASH_IDLE_COUNT);
            assert(a.base >= DASH_IDLE_SIT && a.base <= DASH_IDLE_LIE);
            assert(a.base_duration_s >= 60 && a.base_duration_s <= 180);
            assert(a.elapsed_s < a.duration_s);
            assert(a.started_at+a.elapsed_s == midnight+s);
            if (a.base_started_at != prev.base_started_at) assert(a.base != prev.base);
            if (a.kind == DASH_IDLE_YARN) assert(s < 21*3600+30*60);
            prev = a;
        }
        prev = dash_idle_at(midnight+EVENING-1);
        for (int s = EVENING; s < WASH; ++s) {
            int64_t t = midnight+s;
            dash_idle_state_t a = dash_idle_at(t);
            same_state(a, dash_idle_at(t));
            assert(dash_schedule_at(t, NULL, NULL) == DASH_ACT_FREE);
            assert(!dash_schedule_is_night(t));
            assert(a.elapsed_s < a.duration_s);
            assert(a.started_at+a.elapsed_s == t);
            assert(a.base_started_at <= t && t < a.base_started_at+a.base_duration_s);
            assert(a.base_duration_s >= 60 && a.base_duration_s <= 180);
            if (a.base_started_at != prev.base_started_at) {
                assert(a.base != prev.base);
                assert(a.base_started_at == prev.base_started_at+prev.base_duration_s);
            }
            if (a.kind <= DASH_IDLE_LIE) {
                assert(a.kind == a.base);
                assert(a.duration_s == a.base_duration_s);
                ++quiet;
            } else if (a.kind == DASH_IDLE_GROOM) {
                assert(a.duration_s >= 6 && a.duration_s <= 10);
            } else if (a.kind == DASH_IDLE_YARN) {
                assert(a.duration_s >= 8 && a.duration_s <= 12);
                assert(a.started_at+a.duration_s <= midnight+21*3600+30*60);
                if (a.started_at != prev.started_at || prev.kind != DASH_IDLE_YARN) {
                    assert(!last_yarn || a.started_at-last_yarn >= 600);
                    last_yarn = a.started_at;
                    ++yarn_episodes;
                }
            } else {
                assert(a.duration_s >= 2 && a.duration_s <= 4);
            }
            /* Brief gestures finish in full before returning to the base. */
            if (prev.kind > DASH_IDLE_LIE && a.kind != prev.kind) {
                assert(prev.elapsed_s+1 == prev.duration_s);
            }
            if (d == 0 && timeline && (s == EVENING || a.kind != prev.kind || a.started_at != prev.started_at)) {
                printf("%02d:%02d:%02d,%d,%u,%d\n", s/3600, s/60%60, s%60,
                       a.kind, a.duration_s, a.base);
            }
            if (!sample[a.kind]) sample[a.kind] = t;
            ++seen[a.kind];
            ++total;
            prev = a;
        }
        assert(dash_schedule_at(midnight+EVENING-1, NULL, NULL) == DASH_ACT_DINNER);
        assert(dash_schedule_at(midnight+WASH, NULL, NULL) == DASH_ACT_WASH);
        assert(dash_schedule_is_night(midnight+23*3600+30*60));
        /* Jump forwards/backwards like a clock correction or CAT time. */
        dash_idle_state_t before = dash_idle_at(midnight+EVENING+1234);
        (void)dash_idle_at(midnight+WASH+2000);
        (void)dash_idle_at(midnight-2000);
        same_state(before, dash_idle_at(midnight+EVENING+1234));
    }
    for (int k = 0; k < DASH_IDLE_COUNT; ++k) {
        assert(seen[k] > 0);
        if (samples) printf("sample %d %" PRId64 "\n", k, sample[k]);
    }
    assert(quiet*100/total >= 90);
    printf("14 evenings: %d%% quiet, %d yarn episodes; duration, cooldown, cutoff, clock jumps and schedule passed\n",
           quiet*100/total, yarn_episodes);
    return 0;
}
