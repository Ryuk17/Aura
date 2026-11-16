/* Aura — LLM 后端抽象
 *
 * Phase B 只定义接口 + 内置 mock：orchestrator 在 stage worker 里同步调用
 * backend->generate()。Phase C 接 soundproof AE_LLM 时实现同一个回调即可，
 * 音频/事件线程不受影响。
 */
#ifndef AURA_AGENT_LLM_LLM_H
#define AURA_AGENT_LLM_LLM_H

#include <stdint.h>

#include "agent/flow/flow_types.h"
#include "utils/error.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AURA_LLM_TEXT_MAX   512
#define AURA_LLM_TOOL_MAX   32
#define AURA_LLM_ARGS_MAX   256

typedef struct aura_llm_request {
    const aura_flow_ctx_t *ctx;
    const char            *system_prompt;
    const char            *user_text;
    const char            *intent;
    const char            *memory;      /* 最近轮次文本，可为空串 */
    const char            *tool_name;   /* 上一轮工具名，NULL = 无 */
    const char            *tool_result; /* 上一轮工具输出，NULL = 无 */
} aura_llm_request_t;

typedef struct aura_llm_response {
    char text[AURA_LLM_TEXT_MAX];     /* 最终回复 */
    char tool_name[AURA_LLM_TOOL_MAX];/* 非空 = 请求调用工具 */
    char tool_args[AURA_LLM_ARGS_MAX];/* "arg.k=v\n" 行格式 */
} aura_llm_response_t;

typedef aura_err_t (*aura_llm_generate_fn)(void *user, const aura_llm_request_t *req,
                                           aura_llm_response_t *resp);

typedef struct aura_llm_backend {
    const char          *name;
    aura_llm_generate_fn generate;
    void                *user;
} aura_llm_backend_t;

/* 内置 mock：无工具结果时回显用户文本；有工具结果时按 intent 组织回复。 */
void aura_llm_mock_backend(aura_llm_backend_t *out);

#ifdef __cplusplus
}
#endif

#endif /* AURA_AGENT_LLM_LLM_H */
