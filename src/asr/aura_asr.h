/* Aura — ASR 流式识别节点（注册入口）
 *
 * NN 算法族由上层在 agent init 之前注册（与 kws 同向）。
 * 链路写法：chain = kws, asr, ...；参数见 aura_asr.c 的 param_specs。
 */
#ifndef AURA_ASR_AURA_ASR_H
#define AURA_ASR_AURA_ASR_H

#include "utils/error.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 注册 "asr" 算法到全局注册表。幂等：已注册时直接返回 AURA_OK。 */
aura_err_t aura_asr_register(void);

#ifdef __cplusplus
}
#endif

#endif /* AURA_ASR_AURA_ASR_H */
