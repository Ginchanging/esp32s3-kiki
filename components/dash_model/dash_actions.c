#include "dash_actions.h"
#include "cJSON.h"

#include <limits.h>
#include <string.h>

static double number(const cJSON *obj, const char *key, double fallback)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsNumber(v) ? v->valuedouble : fallback;
}

static const char *string(const cJSON *obj, const char *key)
{
    return cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(obj, key));
}

static bool matches(const cJSON *obj, const dash_action_query_t *q)
{
    return cJSON_IsObject(obj) && number(obj, "schema", 0) == 1 &&
           number(obj, "run_id", 0) == q->run_id && number(obj, "attempt", 0) == q->attempt &&
           number(obj, "jobs_page", 0) == q->jobs_page && number(obj, "job_id", -1) == q->job_id &&
           number(obj, "steps_page", -1) == q->steps_page;
}

bool dash_actions_parse(dash_action_page_t *out, const dash_action_query_t *q,
                        const char *json, size_t len)
{
    cJSON *root = cJSON_ParseWithLength(json, len);
    const cJSON *obj = root;
    const cJSON *pages = cJSON_GetObjectItemCaseSensitive(root, "action_pages");
    if (cJSON_IsArray(pages)) {
        cJSON_ArrayForEach(obj, pages) {
            if (matches(obj, q)) break;
        }
    }
    bool ok = false;
    if (!matches(obj, q)) goto done;

    const cJSON *rows = cJSON_GetObjectItemCaseSensitive(obj, "rows");
    double total = number(obj, "total", -1);
    if (!cJSON_IsArray(rows) || !(total >= 0 && total <= DASH_ACTION_MAX_PAGE * DASH_ACTION_BATCH) ||
        total != (int)total) goto done;
    int count = cJSON_GetArraySize(rows);
    int page = q->job_id ? q->steps_page : q->jobs_page;
    if (page < 1 || page > DASH_ACTION_MAX_PAGE || count > DASH_ACTION_BATCH) goto done;
    int remaining = (int)total - (page - 1) * DASH_ACTION_BATCH;
    if (count && remaining < count) goto done;

    dash_action_page_t next = {.total = (int)total};
    const cJSON *row;
    cJSON_ArrayForEach(row, rows) {
        double id = number(row, "id", 0), duration = number(row, "duration_s", -1);
        const char *name = string(row, "name");
        dash_action_item_t *item = &next.items[next.count++];
        if (!(id >= 1 && id <= 9007199254740991.0) || id != (int64_t)id ||
            !(duration >= -1 && duration <= INT_MAX) || duration != (int)duration ||
            !name || strlen(name) >= sizeof(item->name)) goto done;
        item->id = (int64_t)id;
        item->duration_s = (int)duration;
        item->state = dash_run_state(string(row, "status"), string(row, "conclusion"));
        strcpy(item->name, name);
    }
    *out = next;
    ok = true;
done:
    cJSON_Delete(root);
    return ok;
}
