/* Aura — soundproof LLM → pipeline/agent 节点（Phase C 落地）
 *
 * soundproof 的 LLM 是同步、非流式的 AudioEngine_Llm_Process，且需要
 * WITH_MNN_VISION。Aura 侧要把它包成 agent 数据流 stage（或异步节点），
 * 并补齐 barge-in 的取消语义。本轮不注册，避免半成品进入链配置。
 */
#include "soundproof/ae_register.h"
#include "utils/logger/logger.h"

aura_err_t ae_llm_register_all(void)
{
    AURA_LOGW("ae_llm", "LLM adapter is not implemented yet (Phase C); 'llm' is not registered");
    return AURA_OK;
}
