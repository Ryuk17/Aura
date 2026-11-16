/* Aura — Agent 数据流 stage 组装（实现） */
#include "agent/agent_stages.h"

#include <stdlib.h>

#include "agent/stages/dialogue_stage.h"
#include "agent/stages/memory_stage.h"
#include "agent/stages/orchestrator_stage.h"
#include "agent/stages/session_stage.h"
#include "agent/stages/tool_runner_stage.h"
#include "agent/stages/tts_egress_stage.h"
#include "utils/logger/logger.h"

#define TAG "agent_stages"

struct aura_agent_stages {
    aura_flow_bus_t        *flow;
    aura_flow_store_t      *store;
    aura_session_runtime_t *session;
    aura_agent_memory_t    *memory;
    aura_tool_registry_t   *tools;

    aura_session_stage_t      *session_stage;
    aura_dialogue_stage_t     *dialogue_stage;
    aura_memory_stage_t       *memory_stage;
    aura_tool_runner_stage_t  *tool_stage;
    aura_orchestrator_stage_t *orchestrator_stage;
    aura_tts_egress_stage_t   *tts_egress_stage;
};

aura_agent_stages_t *aura_agent_stages_create(aura_flow_bus_t *flow, aura_flow_store_t *store,
                                              const char *config_dir)
{
    if (flow == NULL) {
        return NULL;
    }

    aura_agent_stages_t *stages = (aura_agent_stages_t *)calloc(1, sizeof(*stages));
    if (stages == NULL) {
        return NULL;
    }
    stages->flow  = flow;
    stages->store = store;

    stages->session = aura_session_runtime_create();
    stages->memory  = aura_agent_memory_create();
    if (stages->session == NULL || stages->memory == NULL) {
        aura_agent_stages_destroy(stages);
        return NULL;
    }

    stages->session_stage = aura_session_stage_create(flow, stages->session);
    stages->dialogue_stage =
        aura_dialogue_stage_create(flow, stages->session, stages->memory, config_dir);
    stages->memory_stage = aura_memory_stage_create(flow, stages->session, store);
    if (stages->session_stage == NULL || stages->dialogue_stage == NULL ||
        stages->memory_stage == NULL) {
        aura_agent_stages_destroy(stages);
        return NULL;
    }

    stages->tools = aura_tool_registry_create();
    if (stages->tools == NULL) {
        aura_agent_stages_destroy(stages);
        return NULL;
    }
    (void)aura_tool_registry_load_config(stages->tools, config_dir);

    stages->tool_stage = aura_tool_runner_stage_create(flow, stages->tools);
    stages->orchestrator_stage =
        aura_orchestrator_stage_create(flow, stages->session, stages->memory,
                                       aura_dialogue_stage_system_prompt(stages->dialogue_stage));
    stages->tts_egress_stage = aura_tts_egress_stage_create(flow);
    if (stages->tool_stage == NULL || stages->orchestrator_stage == NULL ||
        stages->tts_egress_stage == NULL) {
        aura_agent_stages_destroy(stages);
        return NULL;
    }

    AURA_LOGI(TAG, "stages created: session / dialogue / memory / tool_runner / orchestrator");
    return stages;
}

void aura_agent_stages_destroy(aura_agent_stages_t *stages)
{
    if (stages == NULL) {
        return;
    }
    /* 先停 worker，再拆依赖的运行时对象。 */
    (void)aura_agent_stages_stop(stages);
    aura_tts_egress_stage_destroy(stages->tts_egress_stage);
    aura_orchestrator_stage_destroy(stages->orchestrator_stage);
    aura_tool_runner_stage_destroy(stages->tool_stage);
    aura_memory_stage_destroy(stages->memory_stage);
    aura_dialogue_stage_destroy(stages->dialogue_stage);
    aura_session_stage_destroy(stages->session_stage);
    aura_tool_registry_destroy(stages->tools);
    aura_agent_memory_destroy(stages->memory);
    aura_session_runtime_destroy(stages->session);
    free(stages);
}

aura_err_t aura_agent_stages_start(aura_agent_stages_t *stages)
{
    if (stages == NULL) {
        return AURA_ERR_INVALID_ARG;
    }
    aura_err_t rc = aura_session_stage_start(stages->session_stage);
    if (rc == AURA_OK) {
        rc = aura_dialogue_stage_start(stages->dialogue_stage);
    }
    if (rc == AURA_OK) {
        rc = aura_memory_stage_start(stages->memory_stage);
    }
    if (rc == AURA_OK) {
        rc = aura_tool_runner_stage_start(stages->tool_stage);
    }
    if (rc == AURA_OK) {
        rc = aura_orchestrator_stage_start(stages->orchestrator_stage);
    }
    if (rc == AURA_OK) {
        rc = aura_tts_egress_stage_start(stages->tts_egress_stage);
    }
    if (rc != AURA_OK) {
        (void)aura_agent_stages_stop(stages);
    }
    return rc;
}

aura_err_t aura_agent_stages_stop(aura_agent_stages_t *stages)
{
    if (stages == NULL) {
        return AURA_OK;
    }
    (void)aura_tts_egress_stage_stop(stages->tts_egress_stage);
    (void)aura_orchestrator_stage_stop(stages->orchestrator_stage);
    (void)aura_tool_runner_stage_stop(stages->tool_stage);
    (void)aura_memory_stage_stop(stages->memory_stage);
    (void)aura_dialogue_stage_stop(stages->dialogue_stage);
    (void)aura_session_stage_stop(stages->session_stage);
    return AURA_OK;
}

void aura_agent_stages_on_barge_in(aura_agent_stages_t *stages)
{
    if (stages == NULL) {
        return;
    }
    aura_flow_ctx_t ctx;
    if (aura_session_runtime_on_barge_in(stages->session, &ctx)) {
        (void)aura_flow_cancel_turn(stages->flow, ctx.session_id, ctx.turn_id);
        AURA_LOGI(TAG, "cancel flow turn session=%u turn=%u", ctx.session_id, ctx.turn_id);
    }
}

aura_session_runtime_t *aura_agent_stages_session(aura_agent_stages_t *stages)
{
    return (stages != NULL) ? stages->session : NULL;
}

aura_agent_memory_t *aura_agent_stages_memory(aura_agent_stages_t *stages)
{
    return (stages != NULL) ? stages->memory : NULL;
}

aura_tool_registry_t *aura_agent_stages_tools(aura_agent_stages_t *stages)
{
    return (stages != NULL) ? stages->tools : NULL;
}

aura_err_t aura_agent_stages_set_llm_backend(aura_agent_stages_t *stages,
                                             const aura_llm_backend_t *backend)
{
    if (stages == NULL) {
        return AURA_ERR_INVALID_ARG;
    }
    return aura_orchestrator_stage_set_backend(stages->orchestrator_stage, backend);
}

void aura_agent_stages_set_tts_feed(aura_agent_stages_t *stages, aura_tts_feed_fn fn, void *user)
{
    if (stages == NULL) {
        return;
    }
    aura_tts_egress_stage_set_feed(stages->tts_egress_stage, fn, user);
}
