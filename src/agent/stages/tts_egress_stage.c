/* Aura — tts_egress stage（实现） */
#include "agent/stages/tts_egress_stage.h"

#include <stdlib.h>
#include <string.h>

#include "agent/flow/flow_stage.h"
#include "agent/stages/stage_proto.h"
#include "utils/logger/logger.h"

#define TAG "tts_egress"

struct aura_tts_egress_stage {
    aura_flow_bus_t       *bus;
    aura_flow_stage_t     *stage;
    aura_tts_feed_fn       feed;
    void                  *feed_user;
};

static aura_err_t tts_egress_handler(aura_flow_msg_t *msg, void *user)
{
    aura_tts_egress_stage_t *s = (aura_tts_egress_stage_t *)user;
    if (s == NULL || msg == NULL) {
        return AURA_ERR_INVALID_ARG;
    }
    if (strcmp(msg->topic, AURA_TOPIC_TTS_SAY) != 0) {
        return AURA_OK;
    }
    if (s->feed == NULL) {
        return AURA_OK; /* 没配播放链：安静丢弃 */
    }
    aura_err_t rc = s->feed(s->feed_user, msg->payload, &msg->ctx);
    if (rc != AURA_OK) {
        AURA_LOGW(TAG, "feed failed: %s", aura_strerror(rc));
    }
    return AURA_OK;
}

aura_tts_egress_stage_t *aura_tts_egress_stage_create(aura_flow_bus_t *bus)
{
    if (bus == NULL) {
        return NULL;
    }
    aura_tts_egress_stage_t *s = (aura_tts_egress_stage_t *)calloc(1, sizeof(*s));
    if (s == NULL) {
        return NULL;
    }
    s->bus   = bus;
    s->stage = aura_flow_stage_create(bus, "tts_egress", tts_egress_handler, s, 16,
                                      AURA_FLOW_DROP_OLDEST);
    if (s->stage == NULL) {
        free(s);
        return NULL;
    }
    if (aura_flow_stage_subscribe(s->stage, AURA_TOPIC_TTS_SAY) != AURA_OK) {
        aura_flow_stage_destroy(s->stage);
        free(s);
        return NULL;
    }
    return s;
}

void aura_tts_egress_stage_destroy(aura_tts_egress_stage_t *stage)
{
    if (stage == NULL) {
        return;
    }
    aura_flow_stage_destroy(stage->stage);
    free(stage);
}

void aura_tts_egress_stage_set_feed(aura_tts_egress_stage_t *stage, aura_tts_feed_fn fn,
                                    void *user)
{
    if (stage == NULL) {
        return;
    }
    stage->feed      = fn;
    stage->feed_user = user;
}

aura_err_t aura_tts_egress_stage_start(aura_tts_egress_stage_t *stage)
{
    if (stage == NULL) {
        return AURA_ERR_INVALID_ARG;
    }
    return aura_flow_stage_start(stage->stage);
}

aura_err_t aura_tts_egress_stage_stop(aura_tts_egress_stage_t *stage)
{
    if (stage == NULL) {
        return AURA_ERR_INVALID_ARG;
    }
    return aura_flow_stage_stop(stage->stage);
}
