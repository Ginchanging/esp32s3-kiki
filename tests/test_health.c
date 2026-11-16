#include "dash_model.h"
#include "cJSON.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    assert(f);
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    rewind(f);
    char *data = malloc((size_t)n+1);
    assert(data && fread(data, 1, (size_t)n, f) == (size_t)n);
    data[n] = '\0';
    fclose(f);
    return data;
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    char *json = read_file(argv[1]);
    cJSON *root = cJSON_Parse(json);
    assert(root);
    cJSON *obj = cJSON_GetObjectItemCaseSensitive(root, "health");
    dash_state_t state;
    dash_state_init(&state);
    assert(state.health.values[0] == DASH_HEALTH_NONE);
    assert(dash_model_parse(&state, json, strlen(json)));
    assert(state.health.have && state.health.configured && state.health.ok && !state.health.error);
    assert(state.health.values[DASH_HEALTH_STEPS] == 8360);
    assert(state.health.values[DASH_HEALTH_EXERCISE] == 42);
    assert(state.health.values[DASH_HEALTH_FAT] == 198);
    assert(state.health.weight_delta == -1 && state.health.weight_change == -6);
    assert(state.health.weight_span == 7 && state.health.weight_hist[23] == 0);
    int64_t now = state.health.fetched_at;
    assert(dash_health_value(&state.health, DASH_HEALTH_STEPS, now) == 8360);
    assert(dash_health_value(&state.health, DASH_HEALTH_STEPS, now+86400) == DASH_HEALTH_NONE);
    assert(dash_health_value(&state.health, DASH_HEALTH_SLEEP, now+86400) == 435);
    char *wire = cJSON_PrintUnformatted(obj);
    assert(strlen(wire) <= 2048 && sizeof(dash_health_t) <= 2048);
    free(wire);
    wire = cJSON_PrintUnformatted(root);
    assert(strlen(wire) <= 8192);
    free(wire);

    dash_health_t original = state.health, parsed = original;
    cJSON *values = cJSON_GetObjectItemCaseSensitive(obj, "values");
    cJSON_ReplaceItemInObjectCaseSensitive(values, "steps", cJSON_CreateNumber(0));
    assert(dash_health_read(&parsed, obj) && parsed.values[DASH_HEALTH_STEPS] == 0);
    cJSON_ReplaceItemInObjectCaseSensitive(values, "steps", cJSON_CreateNull());
    assert(dash_health_read(&parsed, obj) && parsed.values[DASH_HEALTH_STEPS] == DASH_HEALTH_NONE);
    cJSON_ReplaceItemInObjectCaseSensitive(values, "steps", cJSON_CreateNumber(-1));
    parsed = original;
    assert(!dash_health_read(&parsed, obj));
    assert(!memcmp(&parsed, &original, sizeof(parsed)));
    wire = cJSON_PrintUnformatted(root);
    assert(dash_model_parse(&state, wire, strlen(wire)));
    assert(state.have_hub && state.health.error);
    assert(state.health.values[DASH_HEALTH_STEPS] == original.values[DASH_HEALTH_STEPS]);
    free(wire);
    cJSON_ReplaceItemInObjectCaseSensitive(values, "steps", cJSON_CreateNumber(8360));
    assert(dash_health_read(&parsed, obj) && !parsed.error);

    cJSON_ReplaceItemInObjectCaseSensitive(obj, "weight_span", cJSON_CreateNumber(30));
    assert(!dash_health_read(&parsed, obj));
    cJSON_DeleteItemFromObjectCaseSensitive(root, "health");
    wire = cJSON_PrintUnformatted(root);
    assert(dash_model_parse(&state, wire, strlen(wire)) && !state.health.configured);
    free(wire);
    const char *disabled = "{\"schema\":1,\"configured\":false,\"ok\":false}";
    assert(dash_health_parse(&state.health, disabled, strlen(disabled)));
    assert(state.health.have && !state.health.configured);
    cJSON_Delete(root);
    free(json);
    puts("health bounds, privacy contract, midnight and optional-state isolation passed");
    return 0;
}
