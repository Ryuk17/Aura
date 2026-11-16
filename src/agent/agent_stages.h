/* Aura — Agent 数据流 stage 组装
 *
 * 生命周期与 agent_core 对齐：
 *   init  → create（只建队列/线程对象，不启动 worker）
 *   start → start（session → dialogue → memory）
 *   stop  → stop（逆序）
 *   deinit→ destroy（必须早于 flow bus 销毁，stage 的订阅表持有 bus 指针）
 *
 * Phase B2 在此追加 tool_runner / orchestrator。
 */
#ifndef AURA_AGENT_AGENT_STAGES_H
#define AURA_AGENT_AGENT_STAGES_H

#include "agent/flow/flow_bus.h"
#include "agent/flow/flow_store.h"
#include "agent/llm/llm.h"
#include "agent/memory/memory.h"
#include "agent/stages/session_runtime.h"
#include "agent/stages/tts_egress_stage.h"
#include "agent/tools/tools.h"
#include "utils/error.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct aura_agent_stages aura_agent_stages_t;

aura_agent_stages_t *aura_agent_stages_create(aura_flow_bus_t *flow, aura_flow_store_t *store,
                                              const char *config_dir);
void                 aura_agent_stages_destroy(aura_agent_stages_t *stages);

aura_err_t aura_agent_stages_start(aura_agent_stages_t *stages);
aura_err_t aura_agent_stages_stop(aura_agent_stages_t *stages);

/* barge-in：取消当前 turn（下游消息由 cancel_gate 丢弃）。 */
void aura_agent_stages_on_barge_in(aura_agent_stages_t *stages);

aura_session_runtime_t *aura_agent_stages_session(aura_agent_stages_t *stages);
aura_agent_memory_t    *aura_agent_stages_memory(aura_agent_stages_t *stages);
aura_tool_registry_t   *aura_agent_stages_tools(aura_agent_stages_t *stages);

/* 替换 LLM 后端（默认 mock）。必须在 start 前调用。 */
aura_err_t aura_agent_stages_set_llm_backend(aura_agent_stages_t *stages,
                                             const aura_llm_backend_t *backend);

/* tts.say 的出口（agent_core 注入：写入 playback pipeline）。 */
void aura_agent_stages_set_tts_feed(aura_agent_stages_t *stages, aura_tts_feed_fn fn,
                                    void *user);

#ifdef __cplusplus
}
#endif

#endif /* AURA_AGENT_AGENT_STAGES_H */
