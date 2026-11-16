/* Aura — orchestrator stage：对话编排 / LLM ↔ tool 循环
 *
 * 消费：dialogue.intent、tool.result、session.turn.start、session.closed
 * 发布：tool.call、tts.say、memory.write、error.runtime
 *
 * LLM 通过 aura_llm_backend_t 抽象；默认 mock，Phase C 替换为 soundproof AE_LLM。
 * generate() 在 stage worker 内同步执行，不阻塞音频/事件线程。
 */
#ifndef AURA_AGENT_STAGES_ORCHESTRATOR_STAGE_H
#define AURA_AGENT_STAGES_ORCHESTRATOR_STAGE_H

#include "agent/flow/flow_bus.h"
#include "agent/llm/llm.h"
#include "agent/memory/memory.h"
#include "agent/stages/session_runtime.h"
#include "utils/error.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct aura_orchestrator_stage aura_orchestrator_stage_t;

aura_orchestrator_stage_t *aura_orchestrator_stage_create(aura_flow_bus_t *bus,
                                                          aura_session_runtime_t *rt,
                                                          aura_agent_memory_t *memory,
                                                          const char *system_prompt);
void aura_orchestrator_stage_destroy(aura_orchestrator_stage_t *stage);

/* 必须在 start 前调用；backend 结构体内容会被拷贝。 */
aura_err_t aura_orchestrator_stage_set_backend(aura_orchestrator_stage_t *stage,
                                               const aura_llm_backend_t *backend);

aura_err_t aura_orchestrator_stage_start(aura_orchestrator_stage_t *stage);
aura_err_t aura_orchestrator_stage_stop(aura_orchestrator_stage_t *stage);

#ifdef __cplusplus
}
#endif

#endif /* AURA_AGENT_STAGES_ORCHESTRATOR_STAGE_H */
