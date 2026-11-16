/* Aura — 会话/轮次运行时状态
 *
 * 板端同一时刻只有一个"活跃语音会话"：唤醒词开启会话，Idle 关闭会话；
 * 会话内每次 VAD 起点开启一个新 turn。speaker 由声纹结果绑定（当前只有
 * passed/score，说话人 ID 在 SR 提供身份后替换为真实 ID）。
 *
 * 所有 stage 通过本对象取当前 ctx，而不是各自维护一份全局状态。
 */
#ifndef AURA_AGENT_STAGES_SESSION_RUNTIME_H
#define AURA_AGENT_STAGES_SESSION_RUNTIME_H

#include <stdbool.h>
#include <stdint.h>

#include "agent/flow/flow_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct aura_session_runtime aura_session_runtime_t;

aura_session_runtime_t *aura_session_runtime_create(void);
void                    aura_session_runtime_destroy(aura_session_runtime_t *rt);

/* 唤醒：未唤醒时开启新会话（session_id 单调递增）。 */
bool aura_session_runtime_on_wake(aura_session_runtime_t *rt, uint32_t *session_id,
                                  bool *is_new);

/* 声纹结果：passed 时 speaker_id 记为 1（默认用户）；Phase D 换成真实身份。 */
bool aura_session_runtime_on_voiceprint(aura_session_runtime_t *rt, bool passed,
                                        int32_t score, uint32_t *speaker_id);

/* 开启新 turn（VAD 起点调用）。未唤醒返回 false。 */
bool aura_session_runtime_begin_turn(aura_session_runtime_t *rt, aura_flow_ctx_t *ctx);

/* 确保有可用 turn：已有 turn 直接返回当前 ctx，否则开新 turn。 */
bool aura_session_runtime_ensure_turn(aura_session_runtime_t *rt, aura_flow_ctx_t *ctx);

/* 当前 ctx（不改变状态）。无会话返回 false。 */
bool aura_session_runtime_current(aura_session_runtime_t *rt, aura_flow_ctx_t *ctx);

/* 回到 Idle：关闭会话（保留 last session_id 供日志/记忆收尾）。 */
void aura_session_runtime_on_idle(aura_session_runtime_t *rt);

bool     aura_session_runtime_is_awake(aura_session_runtime_t *rt);
uint32_t aura_session_runtime_session_id(aura_session_runtime_t *rt);

/* barge-in：取消当前 turn（ctx 返回被取消的轮次）。返回 false 表示没有可取消的轮次。 */
bool aura_session_runtime_on_barge_in(aura_session_runtime_t *rt, aura_flow_ctx_t *ctx);

#ifdef __cplusplus
}
#endif

#endif /* AURA_AGENT_STAGES_SESSION_RUNTIME_H */
