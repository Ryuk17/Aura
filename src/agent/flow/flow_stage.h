/* Aura — Agent 数据流 stage
 *
 * stage = 一个命名 worker + 一个有界输入队列 + 一组 topic 订阅。
 * publish 只把消息拷进队列；handler 在 stage 自己的任务里执行，因此
 * 慢的 dialogue/tool/LLM stage 不会拖住 event_bus 或音频线程。
 */
#ifndef AURA_AGENT_FLOW_FLOW_STAGE_H
#define AURA_AGENT_FLOW_FLOW_STAGE_H

#include <stdint.h>

#include "agent/flow/flow_bus.h"
#include "agent/flow/flow_types.h"
#include "utils/error.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef aura_err_t (*aura_flow_handler_t)(aura_flow_msg_t *msg, void *user);

typedef struct aura_flow_stage_stats {
    uint64_t received;
    uint64_t dropped;
    uint64_t delivered;
    uint64_t errors;
} aura_flow_stage_stats_t;

aura_flow_stage_t *aura_flow_stage_create(aura_flow_bus_t *bus, const char *name,
                                          aura_flow_handler_t handler, void *user,
                                          uint32_t queue_depth, aura_flow_policy_t policy);
void aura_flow_stage_destroy(aura_flow_stage_t *stage);

/* 订阅一个 topic（空串 = 所有 topic）。必须在 start 前调用。 */
aura_err_t aura_flow_stage_subscribe(aura_flow_stage_t *stage, const char *topic);

aura_err_t aura_flow_stage_start(aura_flow_stage_t *stage);
aura_err_t aura_flow_stage_stop(aura_flow_stage_t *stage);

const char *aura_flow_stage_name(const aura_flow_stage_t *stage);
void        aura_flow_stage_stats(const aura_flow_stage_t *stage, aura_flow_stage_stats_t *out);

/* 内部接口：bus 发布时把消息拷进 stage 队列。 */
aura_err_t aura_flow_stage_enqueue(aura_flow_stage_t *stage, const aura_flow_msg_t *msg);

/* 内部接口：中间件链走完后把消息交给 stage handler。 */
aura_err_t aura_flow_stage_deliver(aura_flow_stage_t *stage, aura_flow_msg_t *msg);

#ifdef __cplusplus
}
#endif

#endif /* AURA_AGENT_FLOW_FLOW_STAGE_H */
