#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "dash_model.h"
#include "cJSON.h"

static char *read_all(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb"); assert(f);
    fseek(f, 0, SEEK_END); *len = (size_t)ftell(f); rewind(f);
    char *s = malloc(*len+1); assert(s);
    assert(fread(s, 1, *len, f) == *len); s[*len] = 0; fclose(f);
    return s;
}

static int64_t at(int y, int m, int d, int h)
{
    struct tm t = {.tm_year=y-1900, .tm_mon=m-1, .tm_mday=d, .tm_hour=h};
    return (int64_t)mktime(&t)-DASH_TZ_OFFSET_S;
}

static void reject(dash_calendar_t *st, const cJSON *root)
{
    dash_calendar_t before = *st;
    char *json = cJSON_PrintUnformatted(root); assert(json);
    assert(!dash_calendar_parse(st, json, strlen(json)));
    assert(!memcmp(st, &before, sizeof(before)));
    cJSON_free(json);
}

int main(int argc, char **argv)
{
    assert(argc == 3);
    setenv("TZ", "UTC0", 1); tzset();
    size_t len;
    char *json = read_all(argv[1], &len);
    dash_state_t state; dash_state_init(&state);
    assert(dash_calendar_parse(&state.calendar, json, len));
    assert(state.calendar.count == 8 && state.calendar.total > 8);
    assert(state.calendar.events[0].all_day);
    int64_t now = state.calendar.fetched_at+10;
    assert(dash_calendar_fresh(&state.calendar, now));
    assert(!dash_calendar_fresh(&state.calendar, now+1900));
    state.calendar.fetch_failures = 1;
    assert(!dash_calendar_fresh(&state.calendar, now));
    state.calendar.fetch_failures = 0;
    dash_calendar_t before = state.calendar;
    assert(dash_model_parse(&state, json, len));
    assert(!memcmp(&before, &state.calendar, sizeof(before)));
    assert(!dash_calendar_parse(&state.calendar, "{", 1));
    assert(!memcmp(&before, &state.calendar, sizeof(before)));

    cJSON *root = cJSON_Parse(json);
    cJSON *cal = cJSON_GetObjectItemCaseSensitive(root, "calendar");
    cJSON *fetched = cJSON_GetObjectItemCaseSensitive(cal, "fetched_at");
    double stamp = fetched->valuedouble;
    cJSON_SetNumberValue(fetched, stamp*1000); reject(&state.calendar, root);
    cJSON_SetNumberValue(fetched, stamp);
    cJSON *counts = cJSON_GetObjectItemCaseSensitive(cal, "counts");
    cJSON *first = cJSON_GetArrayItem(counts, 0);
    double count = first->valuedouble;
    cJSON_SetNumberValue(first, count+1); reject(&state.calendar, root);
    cJSON_SetNumberValue(first, count);
    cJSON *events = cJSON_GetObjectItemCaseSensitive(cal, "events");
    cJSON_AddItemToArray(events, cJSON_Duplicate(cJSON_GetArrayItem(events, 0), true));
    reject(&state.calendar, root);
    cJSON_DeleteItemFromArray(events, 8);
    cJSON *day = cJSON_GetObjectItemCaseSensitive(cal, "day");
    double day_n = day->valuedouble;
    cJSON_SetNumberValue(day, day_n-1); reject(&state.calendar, root);
    cJSON_SetNumberValue(day, day_n);
    cJSON *offset = cJSON_GetObjectItemCaseSensitive(cal, "offset");
    cJSON_SetNumberValue(offset, 1); reject(&state.calendar, root);
    cJSON_SetNumberValue(offset, 0);
    cJSON *title = cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(events, 0), "title");
    char long_title[130]; memset(long_title, 'a', sizeof(long_title)-1); long_title[129] = 0;
    cJSON_SetValuestring(title, long_title); reject(&state.calendar, root);
    cJSON_Delete(root); free(json);

    /* Compare every supported lunar date against the independently downloaded
     * Hong Kong Observatory source snapshot, not a second calendar algorithm. */
    json = read_all(argv[2], &len);
    root = cJSON_Parse(json); assert(root);
    const cJSON *years = cJSON_GetObjectItemCaseSensitive(root, "years");
    for (int year = 2026; year <= 2099; ++year) {
        char key[8]; snprintf(key, sizeof(key), "%d", year);
        const cJSON *dates = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(years, key), "dates");
        assert(cJSON_GetArraySize(dates) == 6);
        for (int i = 0; i < 6; ++i) {
            int y, m, d;
            assert(sscanf(cJSON_GetArrayItem(dates, i)->valuestring, "%d-%d-%d", &y, &m, &d) == 3);
            int64_t t = at(y, m, d, 12);
            assert(dash_holidays_at(t) & (1u<<i));
            if (i) {
                assert(!(dash_holidays_at(t-86400) & (1u<<i)));
                assert(!(dash_holidays_at(t+86400) & (1u<<i)));
            } else {
                for (int j = 0; j < 7; ++j) assert(dash_holidays_at(t+j*86400) & DASH_HOL_SPRING);
                assert(!(dash_holidays_at(t-86400) & DASH_HOL_SPRING));
                assert(!(dash_holidays_at(t+7*86400) & DASH_HOL_SPRING));
            }
        }
        int thanksgiving = 0;
        for (int d = 1; d <= 30; ++d) if (dash_holidays_at(at(year, 11, d, 12)) & DASH_HOL_THANKSGIVING) {
            assert(d >= 22 && d <= 28); ++thanksgiving;
        }
        assert(thanksgiving == 1);
    }
    cJSON_Delete(root); free(json);
    assert(dash_holidays_at(at(2026, 8, 7, 0)) & DASH_HOL_BIRTHDAY);
    assert(!(dash_holidays_at(at(2026, 8, 7, 0)-1) & DASH_HOL_BIRTHDAY));
    assert(!(dash_holidays_at(at(2026, 8, 8, 0)) & DASH_HOL_BIRTHDAY));
    assert(dash_holidays_at(at(2028, 12, 21, 12)) & DASH_HOL_SOLSTICE);
    assert(!(dash_holidays_at(at(2028, 6, 27, 12)) & DASH_HOL_DRAGON)); /* leap May */
    assert(dash_holidays_at(at(2030, 10, 5, 12)) == (DASH_HOL_CHONGYANG|DASH_HOL_NATIONAL));
    assert(dash_holidays_at(at(2026, 10, 31, 12)) & DASH_HOL_HALLOWEEN);
    assert(dash_holidays_at(at(2026, 12, 25, 12)) & DASH_HOL_CHRISTMAS);
    assert(dash_holidays_at(at(2100, 8, 7, 12)) & DASH_HOL_BIRTHDAY);
    assert(!(dash_holidays_at(at(2100, 2, 9, 12)) & DASH_HOL_SPRING));
    assert(!strcmp(getenv("TZ"), "UTC0"));
    puts("calendar contract, atomic parsing, freshness, all 74 years, leap months and holiday boundaries passed");
    return 0;
}
