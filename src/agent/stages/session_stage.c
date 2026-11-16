/* Aura — session stage（实现） */
#include "agent/stages/session_stage.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "agent/flow/flow_stage.h"
#include "agent/stages/stage_proto.h"
#include "utils/logger/logger.h"

#define TAG "session_stage"

struct aura_session_stage {
    aura_flow_bus_t        *bus;
    aura_session_runtime_t *rt;
    aura_flow_stage_t      *stage;
};

static void publish_ready(aura_session_stage_t *s, bool is_new)
{
    aura_flow_ctx_t ctx;
    if (!aura_session_runtime_current(s->rt, &ctx)) {
        return;
    }
    char payload[64];
    snprintf(payload, sizeof(payload), "new=%d\nsession=%u", is_new ? 1 : 0, ctx.session_id);
    (void)aura_stage_publish(s->bus, AURA_TOPIC_SESSION_READY, AURA_FLOW_EVENT, &ctx, payload);
}

static void on_wake(aura_session_stage_t *s)
{
    uint32_t sid    = 0;
    bool     is_new = false;
    if (!aura_session_runtime_on_wake(s->rt, &sid, &is_new)) {
        return;
    }
    publish_ready(s, is_new);
}

static void on_voiceprint(aura_session_stage_t *s, const aura_flow_msg_t *msg)
{
    if (!aura_session_runtime_is_awake(s->rt)) {
        return;
    }

    aura_stage_kv_t kvs[AURA_STAGE_KV_MAX];
    uint32_t        n = aura_stage_kv_parse(msg->payload, kvs, AURA_STAGE_KV_MAX);

    int32_t passed_i = 0;
    int32_t score    = 0;
    (void)aura_stage_kv_get_int(kvs, n, "passed", &passed_i);
    (void)aura_stage_kv_get_int(kvs, n, "score", &score);

    uint32_t speaker = 0;
    (void)aura_session_runtime_on_voiceprint(s->rt, passed_i != 0, score, &speaker);

    aura_flow_ctx_t ctx;
    if (!aura_session_runtime_current(s->rt, &ctx)) {
        return;
    }
    char payload[96];
    snprintf(payload, sizeof(payload), "speaker=%u\npassed=%d\nscore=%d", speaker,
             passed_i != 0 ? 1 : 0, score);
    (void)aura_stage_publish(s->bus, AURA_TOPIC_SESSION_SPEAKER, AURA_FLOW_EVENT, &ctx,
                             payload);
}

static void on_vad_start(aura_session_stage_t *s)
{
    aura_flow_ctx_t ctx;
    if (!aura_session_runtime_begin_turn(s->rt, &ctx)) {
        return; /* 未唤醒：静音期的 VAD 不产生 turn */
    }
    char payload[64];
    snprintf(payload, sizeof(payload), "turn=%u", ctx.turn_id);
    (void)aura_stage_publish(s->bus, AURA_TOPIC_TURN_START, AURA_FLOW_EVENT, &ctx, payload);
}

static void on_vad_end(aura_session_stage_t *s)
{
    if (!aura_session_runtime_is_awake(s->rt)) {
        return;
    }
    aura_flow_ctx_t ctx;
    if (!aura_session_runtime_current(s->rt, &ctx)) {
        return;
    }
    char payload[64];
    snprintf(payload, sizeof(payload), "turn=%u", ctx.turn_id);
    (void)aura_stage_publish(s->bus, AURA_TOPIC_TURN_END, AURA_FLOW_EVENT, &ctx, payload);
}

static void on_session_state(aura_session_stage_t *s, const aura_flow_msg_t *msg)
{
    /* 状态机 payload 形如 "Listening->Idle"；回到 Idle 即关闭会话。 */
    if (strstr(msg->payload, "->Idle") == NULL) {
        return;
    }
    aura_flow_ctx_t ctx;
    bool            had = aura_session_runtime_current(s->rt, &ctx);
    aura_session_runtime_on_idle(s->rt);
    if (had) {
        (void)aura_stage_publish(s->bus, AURA_TOPIC_SESSION_CLOSED, AURA_FLOW_EVENT, &ctx,
                                 "reason=idle");
    }
}

static aura_err_t session_handler(aura_flow_msg_t *msg, void *user)
{
    aura_session_stage_t *s = (aura_session_stage_t *)user;
    if (s == NULL || msg == NULL) {
        return AURA_ERR_INVALID_ARG;
    }

    if (strcmp(msg->topic, AURA_TOPIC_SESSION_WAKE) == 0) {
        on_wake(s);
    } else if (strcmp(msg->topic, AURA_TOPIC_SESSION_VOICEPRINT) == 0) {
        on_voiceprint(s, msg);
    } else if (strcmp(msg->topic, AURA_TOPIC_VAD_START) == 0) {
        on_vad_start(s);
    } else if (strcmp(msg->topic, AURA_TOPIC_VAD_END) == 0) {
        on_vad_end(s);
    } else if (strcmp(msg->topic, AURA_TOPIC_SESSION_STATE) == 0) {
        on_session_state(s, msg);
    }
    return AURA_OK;
}

aura_session_stage_t *aura_session_stage_create(aura_flow_bus_t *bus,
                                                aura_session_runtime_t *rt)
{
    if (bus == NULL || rt == NULL) {
        return NULL;
    }

    aura_session_stage_t *s = (aura_session_stage_t *)calloc(1, sizeof(*s));
    if (s == NULL) {
        return NULL;
    }
    s->bus = bus;
    s->rt  = rt;
    s->stage = aura_flow_stage_create(bus, "session", session_handler, s, 64,
                                      AURA_FLOW_DROP_OLDEST);
    if (s->stage == NULL) {
        free(s);
        return NULL;
    }

    const char *topics[] = {
        AURA_TOPIC_SESSION_WAKE, AURA_TOPIC_SESSION_VOICEPRINT, AURA_TOPIC_VAD_START,
        AURA_TOPIC_VAD_END, AURA_TOPIC_SESSION_STATE,
    };
    for (size_t i = 0; i < sizeof(topics) / sizeof(topics[0]); i++) {
        if (aura_flow_stage_subscribe(s->stage, topics[i]) != AURA_OK) {
            aura_flow_stage_destroy(s->stage);
            free(s);
            return NULL;
        }
    }
    return s;
}

void aura_session_stage_destroy(aura_session_stage_t *stage)
{
    if (stage == NULL) {
        return;
    }
    aura_flow_stage_destroy(stage->stage);
    free(stage);
}

aura_err_t aura_session_stage_start(aura_session_stage_t *stage)
{
    if (stage == NULL) {
        return AURA_ERR_INVALID_ARG;
    }
    return aura_flow_stage_start(stage->stage);
}

aura_err_t aura_session_stage_stop(aura_session_stage_t *stage)
{
    if (stage == NULL) {
        return AURA_ERR_INVALID_ARG;
    }
    return aura_flow_stage_stop(stage->stage);
}
