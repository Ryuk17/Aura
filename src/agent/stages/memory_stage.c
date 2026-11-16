/* Aura — memory stage（实现） */
#include "agent/stages/memory_stage.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "agent/flow/flow_stage.h"
#include "agent/stages/stage_proto.h"
#include "utils/logger/logger.h"

#define TAG "memory_stage"

struct aura_memory_stage {
    aura_flow_bus_t        *bus;
    aura_session_runtime_t *rt;
    aura_flow_store_t      *store;
    aura_flow_stage_t      *stage;
};

static aura_err_t memory_handler(aura_flow_msg_t *msg, void *user)
{
    aura_memory_stage_t *s = (aura_memory_stage_t *)user;
    if (s == NULL || msg == NULL) {
        return AURA_ERR_INVALID_ARG;
    }
    if (strcmp(msg->topic, AURA_TOPIC_MEMORY_WRITE) != 0) {
        return AURA_OK;
    }

    aura_stage_kv_t kvs[AURA_STAGE_KV_MAX];
    uint32_t        n    = aura_stage_kv_parse(msg->payload, kvs, AURA_STAGE_KV_MAX);
    const char     *role = aura_stage_kv_get(kvs, n, "role");
    const char     *text = aura_stage_kv_get(kvs, n, "text");
    if (role == NULL || text == NULL || role[0] == '\0' || strlen(role) > 16) {
        AURA_LOGW(TAG, "malformed memory.write");
        return AURA_ERR_INVALID_ARG;
    }

    aura_flow_ctx_t ctx;
    if (msg->ctx.session_id != 0) {
        ctx = msg->ctx;
    } else if (!aura_session_runtime_current(s->rt, &ctx)) {
        return AURA_OK;
    }
    if (ctx.session_id == 0) {
        return AURA_OK;
    }

    if (s->store != NULL) {
        char key[64];
        snprintf(key, sizeof(key), "mem.%.16s.last", role);
        aura_err_t rc = aura_flow_store_put(s->store, ctx.session_id, key, text);
        if (rc != AURA_OK) {
            AURA_LOGW(TAG, "store put failed: %s", aura_strerror(rc));
        }
    }

    char payload[128];
    snprintf(payload, sizeof(payload), "role=%s\nsession=%u", role, ctx.session_id);
    (void)aura_stage_publish(s->bus, AURA_TOPIC_MEMORY_STORED, AURA_FLOW_MEMORY_OP, &ctx,
                             payload);
    return AURA_OK;
}

aura_memory_stage_t *aura_memory_stage_create(aura_flow_bus_t *bus, aura_session_runtime_t *rt,
                                              aura_flow_store_t *store)
{
    if (bus == NULL || rt == NULL) {
        return NULL;
    }

    aura_memory_stage_t *s = (aura_memory_stage_t *)calloc(1, sizeof(*s));
    if (s == NULL) {
        return NULL;
    }
    s->bus   = bus;
    s->rt    = rt;
    s->store = store;

    s->stage = aura_flow_stage_create(bus, "memory", memory_handler, s, 32,
                                      AURA_FLOW_DROP_OLDEST);
    if (s->stage == NULL) {
        free(s);
        return NULL;
    }
    if (aura_flow_stage_subscribe(s->stage, AURA_TOPIC_MEMORY_WRITE) != AURA_OK) {
        aura_flow_stage_destroy(s->stage);
        free(s);
        return NULL;
    }
    return s;
}

void aura_memory_stage_destroy(aura_memory_stage_t *stage)
{
    if (stage == NULL) {
        return;
    }
    aura_flow_stage_destroy(stage->stage);
    free(stage);
}

aura_err_t aura_memory_stage_start(aura_memory_stage_t *stage)
{
    if (stage == NULL) {
        return AURA_ERR_INVALID_ARG;
    }
    return aura_flow_stage_start(stage->stage);
}

aura_err_t aura_memory_stage_stop(aura_memory_stage_t *stage)
{
    if (stage == NULL) {
        return AURA_ERR_INVALID_ARG;
    }
    return aura_flow_stage_stop(stage->stage);
}
