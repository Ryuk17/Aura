/* Aura — soundproof 算法注册入口
 *
 * aura_agent_init() 在解析链配置之前调用一次：把 Aura 适配层支持的算法
 * 描述表注册进 core/algorithm 的注册表。注册名就是 .conf 里 chain 写的名字。
 */
#ifndef AURA_SOUNDPROOF_AE_REGISTER_H
#define AURA_SOUNDPROOF_AE_REGISTER_H

#include "utils/error.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 注册所有已编译进来的 soundproof 适配节点。重复调用返回 AURA_ERR_EXIST，
 * 由调用方决定是否忽略（agent init 只调用一次）。 */
aura_err_t ae_register_all(void);

/* 各族的注册函数（供单测/裁剪组合单独使用）。
 * 未编译进来的族在 ae_register.c 里不会被调用；这里统一声明，避免头文件
 * 依赖 aura_config.h 的编译宏。 */
aura_err_t ae_audio_register_all(void);
aura_err_t ae_vad_register_all(void);
aura_err_t ae_kws_register_all(void);
aura_err_t ae_sr_register_all(void);
aura_err_t ae_asr_register_all(void);
aura_err_t ae_tts_register_all(void);

#ifdef __cplusplus
}
#endif

#endif /* AURA_SOUNDPROOF_AE_REGISTER_H */
