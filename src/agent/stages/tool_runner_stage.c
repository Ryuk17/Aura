/* Aura — tool_runner stage（实现） */
#include "agent/stages/tool_runner_stage.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "agent/flow/flow_stage.h"
#include "agent/stages/stage_proto.h"
#include "utils/logger/logger.h"

#define TAG "tool_runner"

#define AURA_TOOL_CACHE_ENTRIES 16
#define AURA_TOOL_CALL_ID_MAX   32

typedef struct tool_cache_entry {
    bool     used;
    char     call_id[AURA_TOOL_CALL_ID_MAX];
    char     status[8];
    char     output[AURA_TOOL_OUTPUT_MAX];
    uint32_t retries;
} tool_cache_entry_t;

struct aura_tool_runner_stage {
    aura_flow_bus_t       *bus;
    aura_tool_registry_t  *tools;
    aura_flow_stage_t     *stage;
    tool_cache_entry_t     cache[AURA_TOOL_CACHE_ENTRIES];
    uint32_t               cache_next;
};

static tool_cache_entry_t *cache_find(aura_tool_runner_stage_t *s, const char *call_id)
{
    for (uint32_t i = 0; i < AURA_TOOL_CACHE_ENTRIES; i++) {
        if (s->cache[i].used && strcmp(s->cache[i].call_id, call_id) == 0) {
            return &s->cache[i];
        }
    }
    return NULL;
}

static tool_cache_entry_t *cache_store(aura_tool_runner_stage_t *s, const char *call_id)
{
    tool_cache_entry_t *e = cache_find(s, call_id);
    if (e == NULL) {
        e = &s->cache[s->cache_next];
        s->cache_next = (s->cache_next + 1) % AURA_TOOL_CACHE_ENTRIES;
        memset(e, 0, sizeof(*e));
        e->used = true;
        snprintf(e->call_id, sizeof(e->call_id), "%.*s", (int)(sizeof(e->call_id) - 1),
                 call_id);
    }
    return e;
}

static void publish_result(aura_tool_runner_stage_t *s, const aura_flow_ctx_t *ctx,
                           const char *call_id, const char *name, const char *status,
                           const char *output, uint32_t retries)
{
    /* 结果协议是行式 key=value：工具输出里的换行压成空格。 */
    char safe[AURA_TOOL_OUTPUT_MAX];
    size_t j = 0;
    for (size_t i = 0; output[i] != '\0' && j + 1 < sizeof(safe); i++) {
        char c   = output[i];
        safe[j++] = (c == '\n' || c == '\r') ? ' ' : c;
    }
    safe[j] = '\0';

    char payload[AURA_TOOL_OUTPUT_MAX + 192];
    snprintf(payload, sizeof(payload), "call_id=%s\nname=%s\nstatus=%s\noutput=%s\nretries=%u",
             call_id, (name != NULL) ? name : "-", status, safe, retries);
    (void)aura_stage_publish(s->bus, AURA_TOPIC_TOOL_RESULT, AURA_FLOW_TOOL_RESULT, ctx,
                             payload);
}

static aura_err_t run_tool(const aura_tool_entry_t *entry, const aura_stage_kv_t *args,
                           uint32_t nargs, char *out, uint32_t cap, uint32_t *attempts)
{
    uint32_t max_retries = entry->idempotent ? entry->max_retries : 0;
    aura_err_t rc        = AURA_ERR_FAIL;
    *attempts            = 0;

    for (uint32_t i = 0; i <= max_retries; i++) {
        (*attempts)++;
        rc = entry->fn(args, nargs, out, cap, entry->user);
        if (rc != AURA_ERR_AGAIN) {
            return rc;
        }
    }
    return rc;
}

static aura_err_t tool_handler(aura_flow_msg_t *msg, void *user)
{
    aura_tool_runner_stage_t *s = (aura_tool_runner_stage_t *)user;
    if (s == NULL || msg == NULL) {
        return AURA_ERR_INVALID_ARG;
    }
    if (strcmp(msg->topic, AURA_TOPIC_TOOL_CALL) != 0) {
        return AURA_OK;
    }

    aura_stage_kv_t kvs[AURA_STAGE_KV_MAX];
    uint32_t        n       = aura_stage_kv_parse(msg->payload, kvs, AURA_STAGE_KV_MAX);
    const char     *name    = aura_stage_kv_get(kvs, n, "name");
    const char     *call_id = aura_stage_kv_get(kvs, n, "call_id");
    char            generated_id[AURA_TOOL_CALL_ID_MAX];

    if (call_id == NULL || call_id[0] == '\0') {
        snprintf(generated_id, sizeof(generated_id), "seq%llu", (unsigned long long)msg->seq);
        call_id = generated_id;
    }
    if (name == NULL || name[0] == '\0') {
        publish_result(s, &msg->ctx, call_id, "-", "error", "missing tool name", 0);
        return AURA_OK;
    }

    /* 幂等键命中：直接返回上次结果，不重复执行。 */
    tool_cache_entry_t *cached = cache_find(s, call_id);
    if (cached != NULL) {
        AURA_LOGD(TAG, "idempotent hit: call_id=%s tool=%s", call_id, name);
        publish_result(s, &msg->ctx, call_id, name, cached->status, cached->output,
                       cached->retries);
        return AURA_OK;
    }

    const aura_tool_entry_t *entry = aura_tool_registry_find(s->tools, name);
    char output[AURA_TOOL_OUTPUT_MAX];
    output[0] = '\0';

    if (entry == NULL || !entry->enabled) {
        snprintf(output, sizeof(output), "unknown or disabled tool: %s", name);
        publish_result(s, &msg->ctx, call_id, name, "error", output, 0);
        return AURA_OK;
    }

    uint32_t   attempts = 0;
    aura_err_t rc = run_tool(entry, kvs, n, output, sizeof(output), &attempts);
    if (rc != AURA_OK && output[0] == '\0') {
        snprintf(output, sizeof(output), "%s", aura_strerror(rc));
    }

    const char *status = (rc == AURA_OK) ? "ok" : "error";
    tool_cache_entry_t *slot = cache_store(s, call_id);
    snprintf(slot->status, sizeof(slot->status), "%s", status);
    snprintf(slot->output, sizeof(slot->output), "%.*s", (int)(sizeof(slot->output) - 1),
             output);
    slot->retries = attempts - 1;

    AURA_LOGD(TAG, "tool=%s call_id=%s status=%s attempts=%u", name, call_id, status,
              attempts);
    publish_result(s, &msg->ctx, call_id, name, status, output, attempts - 1);
    return AURA_OK;
}

aura_tool_runner_stage_t *aura_tool_runner_stage_create(aura_flow_bus_t *bus,
                                                        aura_tool_registry_t *tools)
{
    if (bus == NULL || tools == NULL) {
        return NULL;
    }
    aura_tool_runner_stage_t *s = (aura_tool_runner_stage_t *)calloc(1, sizeof(*s));
    if (s == NULL) {
        return NULL;
    }
    s->bus   = bus;
    s->tools = tools;
    s->stage = aura_flow_stage_create(bus, "tool_runner", tool_handler, s, 32,
                                      AURA_FLOW_BLOCK_TIMEOUT);
    if (s->stage == NULL) {
        free(s);
        return NULL;
    }
    if (aura_flow_stage_subscribe(s->stage, AURA_TOPIC_TOOL_CALL) != AURA_OK) {
        aura_flow_stage_destroy(s->stage);
        free(s);
        return NULL;
    }
    return s;
}

void aura_tool_runner_stage_destroy(aura_tool_runner_stage_t *stage)
{
    if (stage == NULL) {
        return;
    }
    aura_flow_stage_destroy(stage->stage);
    free(stage);
}

aura_err_t aura_tool_runner_stage_start(aura_tool_runner_stage_t *stage)
{
    if (stage == NULL) {
        return AURA_ERR_INVALID_ARG;
    }
    return aura_flow_stage_start(stage->stage);
}

aura_err_t aura_tool_runner_stage_stop(aura_tool_runner_stage_t *stage)
{
    if (stage == NULL) {
        return AURA_ERR_INVALID_ARG;
    }
    return aura_flow_stage_stop(stage->stage);
}
