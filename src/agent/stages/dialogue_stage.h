/* Aura — dialogue stage：asr.final → 意图/槽位/turn 上下文
 *
 * 消费：asr.final
 * 发布：dialogue.intent（intent / text / tool / slot.*）、memory.write
 *
 * 意图规则来自 `<config_dir>/prompts.conf`（可回退 `<config_dir>/agent/prompts.conf`），
 * 文件缺失时使用内置规则。真正的对话生成由 orchestrator 在 B2 接 LLM 后端。
 */
#ifndef AURA_AGENT_STAGES_DIALOGUE_STAGE_H
#define AURA_AGENT_STAGES_DIALOGUE_STAGE_H

#include "agent/flow/flow_bus.h"
#include "agent/memory/memory.h"
#include "agent/stages/session_runtime.h"
#include "utils/error.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct aura_dialogue_stage aura_dialogue_stage_t;

aura_dialogue_stage_t *aura_dialogue_stage_create(aura_flow_bus_t *bus,
                                                  aura_session_runtime_t *rt,
                                                  aura_agent_memory_t *memory,
                                                  const char *config_dir);
void                  aura_dialogue_stage_destroy(aura_dialogue_stage_t *stage);
aura_err_t            aura_dialogue_stage_start(aura_dialogue_stage_t *stage);
aura_err_t            aura_dialogue_stage_stop(aura_dialogue_stage_t *stage);

/* 系统提示词（orchestrator 构造 LLM 请求时使用）。 */
const char *aura_dialogue_stage_system_prompt(const aura_dialogue_stage_t *stage);

#ifdef __cplusplus
}
#endif

#endif /* AURA_AGENT_STAGES_DIALOGUE_STAGE_H */
