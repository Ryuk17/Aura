/* Aura — event_bus ↔ flow 桥（实现） */
#include "agent/flow/flow_bridge.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "utils/logger/logger.h"

#define TAG "flow_bridge"

struct aura_flow_bridge {
    aura_flow_bus_t  *flow_bus;
    aura_event_bus_t *event_bus;
    uint32_t          event_mask;
};

static uint32_t all_event_mask(void)
{
    uint32_t mask = 0;
    for (int i = 1; i < AURA_EVENT_TYPE_COUNT; i++) {
        mask |= (1u << i);
    }
    return mask;
}

static void bridge_on_event(const aura_event_t *event, void *user)
{
    aura_flow_bridge_t *b = (aura_flow_bridge_t *)user;
    if (b == NULL || event == NULL) {
        return;
    }

    aura_flow_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.kind          = AURA_FLOW_EVENT;
    bool custom_payload = false;

    switch (event->type) {
    case AURA_EVENT_ASR_PARTIAL:
        msg.kind = AURA_FLOW_ASR_PARTIAL;
        snprintf(msg.topic, sizeof(msg.topic), "asr.partial");
        break;
    case AURA_EVENT_ASR_FINAL:
        msg.kind  = AURA_FLOW_ASR_FINAL;
        msg.flags = AURA_FLOW_FINAL | AURA_FLOW_EOS;
        snprintf(msg.topic, sizeof(msg.topic), "asr.final");
        break;
    case AURA_EVENT_KWS_HIT:
        snprintf(msg.topic, sizeof(msg.topic), "session.wake");
        break;
    case AURA_EVENT_VOICEPRINT_RESULT:
        snprintf(msg.topic, sizeof(msg.topic), "session.voiceprint");
        /* passed/score 在事件 union 里，payload 通常为空；显式编码给 flow 侧。 */
        snprintf(msg.payload, sizeof(msg.payload), "passed=%d\nscore=%d",
                 event->u.voiceprint.passed ? 1 : 0, event->u.voiceprint.score);
        custom_payload = true;
        break;
    case AURA_EVENT_VAD_SPEECH_START:
        snprintf(msg.topic, sizeof(msg.topic), "vad.start");
        break;
    case AURA_EVENT_VAD_SPEECH_END:
        snprintf(msg.topic, sizeof(msg.topic), "vad.end");
        break;
    case AURA_EVENT_STATE_CHANGED:
        snprintf(msg.topic, sizeof(msg.topic), "session.state");
        break;
    case AURA_EVENT_BARGE_IN:
        msg.kind  = AURA_FLOW_CONTROL;
        msg.flags = AURA_FLOW_ABORT;
        snprintf(msg.topic, sizeof(msg.topic), "control.interrupt");
        break;
    case AURA_EVENT_ERROR:
        msg.kind = AURA_FLOW_ERROR;
        snprintf(msg.topic, sizeof(msg.topic), "error.runtime");
        break;
    case AURA_EVENT_LLM_TOKEN:
        snprintf(msg.topic, sizeof(msg.topic), "llm.token");
        break;
    case AURA_EVENT_LLM_DONE:
        msg.flags = AURA_FLOW_EOS;
        snprintf(msg.topic, sizeof(msg.topic), "llm.done");
        break;
    case AURA_EVENT_TTS_DONE:
        msg.flags = AURA_FLOW_EOS;
        snprintf(msg.topic, sizeof(msg.topic), "tts.done");
        break;
    default:
        return; /* 不关心的底层事件不进入 Agent 数据面 */
    }

    if (!custom_payload) {
        snprintf(msg.payload, sizeof(msg.payload), "%s", event->payload);
    }
    msg.len = (uint32_t)strlen(msg.payload);
    if (msg.len == 0) {
        snprintf(msg.payload, sizeof(msg.payload), "%s", aura_event_type_name(event->type));
        msg.len = (uint32_t)strlen(msg.payload);
    }
    (void)aura_flow_publish(b->flow_bus, &msg);
}

aura_flow_bridge_t *aura_flow_bridge_create(aura_flow_bus_t *flow_bus,
                                            aura_event_bus_t *event_bus)
{
    if (flow_bus == NULL || event_bus == NULL) {
        return NULL;
    }
    aura_flow_bridge_t *b = (aura_flow_bridge_t *)calloc(1, sizeof(*b));
    if (b == NULL) {
        return NULL;
    }
    b->flow_bus   = flow_bus;
    b->event_bus  = event_bus;
    b->event_mask = all_event_mask();
    aura_err_t rc = aura_event_bus_subscribe(event_bus, b->event_mask, bridge_on_event, b);
    if (rc != AURA_OK) {
        free(b);
        return NULL;
    }
    AURA_LOGI(TAG, "bridge attached (event_bus -> flow)");
    return b;
}

void aura_flow_bridge_destroy(aura_flow_bridge_t *bridge)
{
    if (bridge == NULL) {
        return;
    }
    aura_event_bus_unsubscribe(bridge->event_bus, bridge_on_event);
    free(bridge);
}
