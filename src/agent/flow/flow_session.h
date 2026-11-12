/* Aura — 会话/轮次状态
 *
 * 一个 session 内有多轮 turn；barge-in 结束当前 turn、开新 turn。
 * speaker_id 由声纹结果绑定，短期记忆按 session/speaker 隔离。
 */
#ifndef AURA_AGENT_FLOW_FLOW_SESSION_H
#define AURA_AGENT_FLOW_FLOW_SESSION_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct aura_flow_session {
    uint32_t id;
    uint32_t turn_id;
    uint32_t speaker_id;
    uint64_t started_us;
    uint64_t turn_started_us;
    bool     cancelled;
} aura_flow_session_t;

void aura_flow_session_init(aura_flow_session_t *s, uint32_t session_id);
void aura_flow_session_begin_turn(aura_flow_session_t *s);
void aura_flow_session_cancel_turn(aura_flow_session_t *s);
void aura_flow_session_set_speaker(aura_flow_session_t *s, uint32_t speaker_id);
bool aura_flow_session_turn_cancelled(const aura_flow_session_t *s);

#ifdef __cplusplus
}
#endif

#endif /* AURA_AGENT_FLOW_FLOW_SESSION_H */
