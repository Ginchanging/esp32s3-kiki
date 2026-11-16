#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dash_model.h"

static char *read_all(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb"); assert(f);
    fseek(f, 0, SEEK_END); *len = (size_t)ftell(f); rewind(f);
    char *s = malloc(*len+1); assert(s);
    assert(fread(s, 1, *len, f) == *len); s[*len] = 0; fclose(f);
    return s;
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    size_t len;
    char *s = read_all(argv[1], &len);
    dash_state_t st;
    dash_state_init(&st);
    assert(dash_model_parse(&st, s, len));
    assert(dash_flights_parse(&st.flights, s, len));
    int64_t now = st.flights.fetched_at+2;
    assert(st.flights.count == 1);
    assert(st.flights.aircraft[0].altitude_m == 10800);
    assert(st.flights.aircraft[0].distance_m == 3200);
    assert(dash_flights_status(&st.flights, now) == DASH_FLIGHT_LIVE);
    assert(dash_flights_status(&st.flights, now+31) == DASH_FLIGHT_STALE);
    dash_flights_t before = st.flights;
    const char *bad[] = {"{", "{\"schema\":2,\"configured\":true}",
                        "{\"schema\":1,\"configured\":true,\"fetched_at\":1794544200000}",
                        "{\"schema\":1,\"configured\":true,\"radius_m\":0}"};
    for (size_t i = 0; i < sizeof(bad)/sizeof(bad[0]); ++i) {
        assert(!dash_flights_parse(&st.flights, bad[i], strlen(bad[i])));
        assert(!memcmp(&st.flights, &before, sizeof(before)));
    }
    dash_flight_cursor_t c = {0};
    dash_flyover_t out[3];
    assert(dash_flights_take_new(&c, &st.flights, now, out, 3) == 0);
    st.flights.event_count = 3;
    for (int i = 0; i < 3; ++i) {
        st.flights.events[i].id = 100-i;
        st.flights.events[i].at = now;
        st.flights.events[i].flight = st.flights.aircraft[0];
    }
    assert(dash_flights_take_new(&c, &st.flights, now, out, 3) == 3);
    assert(out[0].id == 98 && out[2].id == 100);
    assert(dash_flights_take_new(&c, &st.flights, now, out, 3) == 0);
    st.flights.events[0].id = 101;
    st.flights.fetch_failures = 1;
    assert(dash_flights_take_new(&c, &st.flights, now, out, 3) == 0);
    assert(dash_flights_status(&st.flights, now) == DASH_FLIGHT_ERROR);
    st.flights.fetch_failures = 0;
    assert(dash_flights_take_new(&c, &st.flights, now, out, 3) == 0);
    /* Dashboard parsing preserves the independent flight state. */
    before = st.flights;
    assert(dash_model_parse(&st, s, len));
    assert(!memcmp(&st.flights, &before, sizeof(before)));
    const char *disabled = "{\"schema\":1,\"configured\":false}";
    assert(dash_flights_parse(&st.flights, disabled, strlen(disabled)));
    assert(st.flights.count == 0 && dash_flights_status(&st.flights, now) == DASH_FLIGHT_DISABLED);
    free(s);
    puts("flight parser, timestamps, freshness, independent state and replay checks passed");
    return 0;
}
