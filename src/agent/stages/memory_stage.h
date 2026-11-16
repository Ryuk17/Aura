/* Aura — memory stage：把 memory.write 落到长期存储
 *
 * 消费：memory.write（role / text）
 * 发布：memory.stored
 *
 * 短期环由 dialogue / orchestrator 同步写入 aura_agent_memory；本 stage 只负责
 * 异步持久化（Phase B 是 flow_store 内存后端，Phase D 换 sqlite + 批量刷写）。
 */
#ifndef AURA_AGENT_STAGES_MEMORY_STAGE_H
#define AURA_AGENT_STAGES_MEMORY_STAGE_H

#include "agent/flow/flow_bus.h"
#include "agent/flow/flow_store.h"
#include "agent/stages/session_runtime.h"
#include "utils/error.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct aura_memory_stage aura_memory_stage_t;

aura_memory_stage_t *aura_memory_stage_create(aura_flow_bus_t *bus, aura_session_runtime_t *rt,
                                              aura_flow_store_t *store);
void                 aura_memory_stage_destroy(aura_memory_stage_t *stage);
aura_err_t           aura_memory_stage_start(aura_memory_stage_t *stage);
aura_err_t           aura_memory_stage_stop(aura_memory_stage_t *stage);

#ifdef __cplusplus
}
#endif

#endif /* AURA_AGENT_STAGES_MEMORY_STAGE_H */
