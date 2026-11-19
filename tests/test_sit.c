#include <assert.h>
#include <stdio.h>

#include "dash_sit.h"

static int feed(dash_ld2402_t *rx, const char *bytes, uint64_t now_ms)
{
    int cm = -1;
    for (; *bytes; ++bytes) {
        int report = dash_ld2402_feed(rx, (uint8_t)*bytes, now_ms);
        if (report >= 0) cm = report;
    }
    return cm;
}

static void test_receiver(void)
{
    dash_ld2402_t rx;
    dash_ld2402_start(&rx, 0);
    assert(dash_ld2402_baud(&rx) == 115200);
    assert(feed(&rx, "\xff\xfe\nOFF\r\nOFF\r\n", 400) == -1);
    assert(feed(&rx, "distance:", 500) == -1);
    assert(feed(&rx, "120\r", 600) == -1);
    assert(feed(&rx, "\n", 601) == 120);
    assert(feed(&rx, "distance:0\r\ndistance:1001\nOFFx\r\n", 750) == -1);
    assert(feed(&rx, "0000000000000000000000000000000000000000OFF\n", 800) == -1);
    assert(feed(&rx, "OFF\r\n", 950) == 0);
    assert(!dash_ld2402_poll(&rx, 3949));
    assert(dash_ld2402_poll(&rx, 3950));
    assert(dash_ld2402_baud(&rx) == 115200 && !rx.locked);

    assert(feed(&rx, "OF", 4000) == -1);
    assert(!dash_ld2402_poll(&rx, 5949));
    assert(dash_ld2402_poll(&rx, 5950));
    assert(dash_ld2402_baud(&rx) == 921600);
    assert(feed(&rx, "F\r\nOFF\r\nOFF\r\n", 6000) == -1); /* a partial line cannot cross rates */
    assert(feed(&rx, "wrong\r\nOFF\r\nOFF\r\n", 6400) == -1);
    assert(feed(&rx, "distance: 1000\r\n", 6565) == 1000);
    assert(feed(&rx, "distance:-1\n\x80\n", 9000) == -1);
    assert(dash_ld2402_poll(&rx, 9565)); /* malformed data does not refresh the link */
    assert(dash_ld2402_baud(&rx) == 115200 && !rx.locked);
    assert(dash_ld2402_poll(&rx, 11565) && dash_ld2402_baud(&rx) == 921600);
    assert(dash_ld2402_poll(&rx, 13565) && dash_ld2402_baud(&rx) == 115200);
    assert(feed(&rx, "OFF\r\nOFF\r\nOFF\r\n", 13730) == 0);
}

/* Advance with regular reports and also sample exactly at the requested boundary. */
static void report_until(dash_sit_t *seat, uint64_t ms, unsigned cm)
{
    for (uint64_t t = seat->last_ms + 1000; t < ms; t += 1000) {
        dash_sit_report(seat, t, cm, DASH_SIT_RANGE_CM);
    }
    dash_sit_report(seat, ms, cm, DASH_SIT_RANGE_CM);
}

static void test_session(void)
{
    dash_sit_t seat = {0};
    assert(!dash_sit_fresh(&seat, 0));
    report_until(&seat, 0, 80);
    report_until(&seat, 2500, 80);
    assert(seat.phase == DASH_SIT_ENTERING && dash_sit_elapsed(&seat) == 0);
    report_until(&seat, 2600, 121);
    assert(seat.phase == DASH_SIT_IDLE);
    report_until(&seat, 2700, 120);
    report_until(&seat, 5700, 120);
    assert(seat.phase == DASH_SIT_PRESENT && dash_sit_elapsed(&seat) == 0);

    report_until(&seat, 5700 + 1800000, 80);
    uint64_t left = seat.last_ms + 1;
    report_until(&seat, left, 0);
    report_until(&seat, left + DASH_SIT_MISS_GRACE_MS - 1, 150);
    assert(seat.phase == DASH_SIT_PRESENT && dash_sit_elapsed(&seat) == 1810000);
    report_until(&seat, left + DASH_SIT_MISS_GRACE_MS, 0);
    assert(seat.phase == DASH_SIT_AWAY && dash_sit_elapsed(&seat) == 1810001);
    assert(!dash_sit_take(&seat, seat.last_ms));

    uint64_t back = left + DASH_SIT_MISS_GRACE_MS + 1000;
    report_until(&seat, back - 1, 0);
    report_until(&seat, back, 80);
    uint64_t due = seat.last_ms + (DASH_SIT_REMIND_MS - dash_sit_elapsed(&seat));
    report_until(&seat, due - 1, 80);
    assert(dash_sit_elapsed(&seat) == 3599999);
    assert(!dash_sit_take(&seat, due)); /* only a fresh in-zone report can prove the full hour */
    report_until(&seat, due, 80);
    assert(dash_sit_elapsed(&seat) == DASH_SIT_REMIND_MS);
    assert(dash_sit_take(&seat, due));
    assert(!dash_sit_take(&seat, due));

    report_until(&seat, due + 1, 0);
    report_until(&seat, due + 30000, 0);
    assert(seat.phase == DASH_SIT_AWAY && seat.reminded);
    report_until(&seat, due + 30001, 80);
    assert(seat.phase == DASH_SIT_ENTERING && !seat.reminded && dash_sit_elapsed(&seat) == 0);
}

static void test_short_misses(void)
{
    dash_sit_t seat = {0};
    report_until(&seat, 0, 110);
    report_until(&seat, DASH_SIT_ENTER_MS, 110);

    report_until(&seat, DASH_SIT_ENTER_MS + 1200, 145);
    report_until(&seat, DASH_SIT_ENTER_MS + 2900, 144);
    assert(seat.phase == DASH_SIT_PRESENT && dash_sit_elapsed(&seat) == 2900);
    report_until(&seat, DASH_SIT_ENTER_MS + 3000, 98);

    report_until(&seat, DASH_SIT_ENTER_MS + 4000, 0);
    report_until(&seat, DASH_SIT_ENTER_MS + 12000, 0);
    assert(seat.phase == DASH_SIT_PRESENT && dash_sit_elapsed(&seat) == 12000);
    report_until(&seat, DASH_SIT_ENTER_MS + 12500, 106);
    assert(seat.phase == DASH_SIT_PRESENT && dash_sit_elapsed(&seat) == 12500);
}

static void test_deferred_and_disconnected(void)
{
    dash_sit_t seat = {0};
    report_until(&seat, 0, 70);
    report_until(&seat, DASH_SIT_ENTER_MS + DASH_SIT_REMIND_MS, 70);
    uint64_t due = seat.last_ms;
    report_until(&seat, due + 200, 0);
    assert(seat.phase == DASH_SIT_PRESENT);
    assert(!dash_sit_take(&seat, due + 200));
    report_until(&seat, due + 1000, 70);
    assert(dash_sit_take(&seat, due + 1000));
    report_until(&seat, due + 2000, 70);
    assert(dash_sit_fresh(&seat, due + 4999));
    assert(!dash_sit_fresh(&seat, due + 5000));
    assert(!dash_sit_take(&seat, due + 5000));
    assert(seat.phase == DASH_SIT_IDLE && dash_sit_elapsed(&seat) == 0);
    dash_sit_report(&seat, due + 5000, 70, DASH_SIT_RANGE_CM);
    report_until(&seat, due + 8000, 70);
    assert(seat.phase == DASH_SIT_PRESENT && !seat.reminded);
    dash_sit_report(&seat, due + 8100, 70, 0);
    assert(seat.phase == DASH_SIT_IDLE && dash_sit_fresh(&seat, due + 8100) && seat.distance_cm == 70);
}

int main(void)
{
    test_receiver();
    test_session();
    test_short_misses();
    test_deferred_and_disconnected();
    puts("LD2402 decoding, baud recovery and desk-reminder session timing passed");
    return 0;
}
