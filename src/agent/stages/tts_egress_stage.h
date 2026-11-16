/* Aura — tts_egress stage：tts.say → 播放链文本源
 *
 * 消费：tts.say（payload = UTF-8 文本）
 * 回调：把文本注入 playback pipeline 的 TTS 节点（aura_pipeline_feed_text）。
 * 没有配置播放链时回调为空，消息正常丢弃。
 */
#ifndef AURA_AGENT_STAGES_TTS_EGRESS_STAGE_H
#define AURA_AGENT_STAGES_TTS_EGRESS_STAGE_H

#include "agent/flow/flow_bus.h"
#include "agent/flow/flow_types.h"
#include "utils/error.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef aura_err_t (*aura_tts_feed_fn)(void *user, const char *text,
                                       const aura_flow_ctx_t *ctx);

typedef struct aura_tts_egress_stage aura_tts_egress_stage_t;

aura_tts_egress_stage_t *aura_tts_egress_stage_create(aura_flow_bus_t *bus);
void                     aura_tts_egress_stage_destroy(aura_tts_egress_stage_t *stage);
void                     aura_tts_egress_stage_set_feed(aura_tts_egress_stage_t *stage,
                                                        aura_tts_feed_fn fn, void *user);
aura_err_t               aura_tts_egress_stage_start(aura_tts_egress_stage_t *stage);
aura_err_t               aura_tts_egress_stage_stop(aura_tts_egress_stage_t *stage);

#ifdef __cplusplus
}
#endif

#endif /* AURA_AGENT_STAGES_TTS_EGRESS_STAGE_H */
