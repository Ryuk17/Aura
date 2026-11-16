/* Aura — tool_runner stage：执行工具调用
 *
 * 消费：tool.call（name / call_id / arg.*）
 * 发布：tool.result（call_id / status / output / retries）
 *
 * 重试：handler 返回 AURA_ERR_AGAIN 时按 entry->max_retries 重试（仅 idempotent
 * 工具）。call_id 同时作为幂等键：同 call_id 的重复投递直接返回缓存结果，
 * 不会重复执行副作用。
 */
#ifndef AURA_AGENT_STAGES_TOOL_RUNNER_STAGE_H
#define AURA_AGENT_STAGES_TOOL_RUNNER_STAGE_H

#include "agent/flow/flow_bus.h"
#include "agent/tools/tools.h"
#include "utils/error.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct aura_tool_runner_stage aura_tool_runner_stage_t;

aura_tool_runner_stage_t *aura_tool_runner_stage_create(aura_flow_bus_t *bus,
                                                        aura_tool_registry_t *tools);
void                      aura_tool_runner_stage_destroy(aura_tool_runner_stage_t *stage);
aura_err_t                aura_tool_runner_stage_start(aura_tool_runner_stage_t *stage);
aura_err_t                aura_tool_runner_stage_stop(aura_tool_runner_stage_t *stage);

#ifdef __cplusplus
}
#endif

#endif /* AURA_AGENT_STAGES_TOOL_RUNNER_STAGE_H */
