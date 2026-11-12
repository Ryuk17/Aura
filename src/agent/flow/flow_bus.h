/* Aura — Agent 数据流总线
 *
 * 与 core/event_bus 的分工：
 *   event_bus：广播、可丢、无上下文，给实时/观察面用；
 *   flow_bus ：按 topic 路由、每 stage 有界队列、可背压/取消/中间件，
 *              给 Agent 语义面用。两者通过 flow_bridge 桥接。
 *
 * publish 非阻塞、线程安全；消息按值拷贝。stage worker 是唯一执行
 * handler/middleware 的上下文。
 */
#ifndef AURA_AGENT_FLOW_FLOW_BUS_H
#define AURA_AGENT_FLOW_FLOW_BUS_H

#include <stdbool.h>
#include <stdint.h>

#include "agent/flow/flow_types.h"
#include "utils/error.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct aura_flow_bus   aura_flow_bus_t;
typedef struct aura_flow_stage aura_flow_stage_t;

aura_flow_bus_t *aura_flow_bus_create(uint32_t default_stage_depth,
                                      aura_flow_policy_t default_policy);
void             aura_flow_bus_destroy(aura_flow_bus_t *bus);

/* 发布一条消息。seq/ts 由总线补全（调用方可留 0）。
 * 没有订阅者的 topic 会被丢弃并计入 dropped（不是错误：Agent 允许旁路不存在）。
 * 返回 AURA_ERR_FULL 表示至少一个 stage 队列满且其策略是 DROP_NEWEST。 */
aura_err_t aura_flow_publish(aura_flow_bus_t *bus, const aura_flow_msg_t *msg);

/* 订阅（由 aura_flow_stage_create 调用）。topic 为空表示订阅所有 topic。 */
aura_err_t aura_flow_subscribe(aura_flow_bus_t *bus, const char *topic,
                               aura_flow_stage_t *stage);

/* 取消某个会话的某个轮次：cancel_gate 中间件据此丢弃后续消息。
 * turn_id 传 0 表示取消该会话所有轮次。 */
aura_err_t aura_flow_cancel_turn(aura_flow_bus_t *bus, uint32_t session_id, uint32_t turn_id);

/* 查询某条消息所属轮次是否已取消（供 stage/worker 主动检查）。 */
bool aura_flow_is_cancelled(const aura_flow_bus_t *bus, const aura_flow_ctx_t *ctx);

void aura_flow_stats(const aura_flow_bus_t *bus, aura_flow_stats_t *out);

/* 总线内部给 stage/middleware 用的计数接口（不需要对使用者暴露语义）。 */
void aura_flow_note_dropped(aura_flow_bus_t *bus);
void aura_flow_note_cancelled(aura_flow_bus_t *bus);
void aura_flow_note_error(aura_flow_bus_t *bus);

#ifdef __cplusplus
}
#endif

#endif /* AURA_AGENT_FLOW_FLOW_BUS_H */
