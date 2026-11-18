/* One page of jobs or steps from the independent Actions detail endpoint. */
#pragma once

#include "dash_model.h"

#define DASH_ACTION_BATCH 6
#define DASH_ACTION_MAX_PAGE 65535

typedef struct {
    int64_t run_id, job_id; /* job_id = 0 for the list of jobs */
    int attempt, jobs_page, steps_page; /* 1-based; steps_page = 0 for jobs */
} dash_action_query_t;

typedef struct {
    int64_t id; /* GitHub job ID or step number */
    int duration_s; /* -1 when GitHub has no timing */
    dash_run_state_t state;
    char name[96];
} dash_action_item_t;

typedef struct {
    int count, total;
    dash_action_item_t items[DASH_ACTION_BATCH];
} dash_action_page_t;

/* Also accepts a demo fixture containing an "action_pages" array. */
bool dash_actions_parse(dash_action_page_t *out, const dash_action_query_t *query,
                        const char *json, size_t len);
