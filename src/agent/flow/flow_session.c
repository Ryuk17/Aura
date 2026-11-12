/* Aura — 会话/轮次状态（实现） */
#include "agent/flow/flow_session.h"

#include <string.h>

#include "osal/osal.h"

void aura_flow_session_init(aura_flow_session_t *s, uint32_t session_id)
{
    if (s == NULL) {
        return;
    }
    memset(s, 0, sizeof(*s));
    s->id         = session_id;
    s->started_us = aura_osal_time_us();
}

void aura_flow_session_begin_turn(aura_flow_session_t *s)
{
    if (s == NULL) {
        return;
    }
    s->turn_id++;
    if (s->turn_id == 0) {
        s->turn_id = 1;
    }
    s->turn_started_us = aura_osal_time_us();
    s->cancelled       = false;
}

void aura_flow_session_cancel_turn(aura_flow_session_t *s)
{
    if (s != NULL) {
        s->cancelled = true;
    }
}

void aura_flow_session_set_speaker(aura_flow_session_t *s, uint32_t speaker_id)
{
    if (s != NULL) {
        s->speaker_id = speaker_id;
    }
}

bool aura_flow_session_turn_cancelled(const aura_flow_session_t *s)
{
    return (s != NULL) && s->cancelled;
}
