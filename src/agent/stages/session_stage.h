/* Aura — session stage：唤醒/声纹/turn 上下文
 *
 * 消费：session.wake、session.voiceprint、vad.start、vad.end、session.state
 * 发布：session.ready、session.speaker、session.turn.start/end、session.closed
 */
#ifndef AURA_AGENT_STAGES_SESSION_STAGE_H
#define AURA_AGENT_STAGES_SESSION_STAGE_H

#include "agent/flow/flow_bus.h"
#include "agent/stages/session_runtime.h"
#include "utils/error.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct aura_session_stage aura_session_stage_t;

aura_session_stage_t *aura_session_stage_create(aura_flow_bus_t *bus,
                                                aura_session_runtime_t *rt);
void                  aura_session_stage_destroy(aura_session_stage_t *stage);
aura_err_t            aura_session_stage_start(aura_session_stage_t *stage);
aura_err_t            aura_session_stage_stop(aura_session_stage_t *stage);

#ifdef __cplusplus
}
#endif

#endif /* AURA_AGENT_STAGES_SESSION_STAGE_H */
