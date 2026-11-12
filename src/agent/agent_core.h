/* Aura — Agent 组装层（内部头，不对外）
 *
 * 对外接口只有 include/agent_export.h。本头供两类内部使用者：
 *   - soundproof 适配节点注册时使用；
 *   - tests/host_sim 通过本头注入 mock node 验证全链路。
 *
 * agent_core 只做"骨架组装"：config → soundproof 适配层 → pipeline /
 * event_bus / 状态机 → 事件转发到上层回调。真正的对话编排（orchestrator /
 * dialogue / memory / tools）在 Phase C 填充，数据面见 agent/flow/。
 */
#ifndef AURA_AGENT_AGENT_CORE_H
#define AURA_AGENT_AGENT_CORE_H

#include "agent/flow/flow_bus.h"
#include "core/node.h"
#include "core/pipeline/pipeline.h"
#include "core/state_machine/state_machine.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 以下句柄只在 agent 启动后有效，供内部模块在组装阶段取用。 */

/* 向 agent 的 pipeline 注册节点（必须在 init 之后、start 之前调用）。 */
aura_err_t aura_agent_add_node(aura_node_t *node);

/* 取 agent 的事件总线（算法节点发事件用；或直接经 aura_node_post_event）。 */
aura_event_bus_t *aura_agent_bus(void);

/* 取 Agent 数据流总线（会话/轮次/工具/记忆的 stage 用）。 */
aura_flow_bus_t *aura_agent_flow(void);

/* 取 agent 的状态机（只读查询；触发器经事件总线流动）。 */
aura_sm_t *aura_agent_sm(void);

/* 按装配名取由配置链建出的节点（名字即 chain 里的算法名/实例名）。
 * 仿真与自检用：拿到句柄后读它的状态或统计。
 * 找不到返回 NULL。**句柄归 agent 所有**，deinit 时随链一起释放。 */
aura_node_t *aura_agent_chain_node(const char *name);

#ifdef __cplusplus
}
#endif

#endif /* AURA_AGENT_AGENT_CORE_H */
