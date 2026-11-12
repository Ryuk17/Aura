/* Aura — soundproof TTS → pipeline 节点（Phase C 落地）
 *
 * 本轮只保留编译位：TTS 的业务闭环（文本注入、播放链、回采参考）在
 * Phase C 与 playback 一起实现。这里不注册描述表，避免链上写 `tts` 时
 * 得到一个“看起来能装配、运行期才发现没实现”的节点。
 */
#include "soundproof/ae_register.h"
#include "utils/logger/logger.h"

aura_err_t ae_tts_register_all(void)
{
    AURA_LOGW("ae_tts", "TTS adapter is not implemented yet (Phase C); 'tts' is not registered");
    return AURA_OK;
}
