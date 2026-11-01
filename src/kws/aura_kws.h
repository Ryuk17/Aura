/* Aura — KWS 唤醒词节点（注册入口）
 *
 * 按 agent 的约定，NN 算法族由**上层在 agent init 之前**注册
 * （trickroom 是编译期自注册，NN 族留给了装配层/应用 —— 见 agent_core.c 的
 * 注释）。链路写法：chain = kws, ...，参数见 aura_kws.c 的 param_specs。
 */
#ifndef AURA_KWS_AURA_KWS_H
#define AURA_KWS_AURA_KWS_H

#include "utils/error.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 注册 "kws" 算法到全局注册表。幂等：已注册时直接返回 AURA_OK
 * （"谁先注册谁生效"，与 trickroom 注册同向）。 */
aura_err_t aura_kws_register(void);

#ifdef __cplusplus
}
#endif

#endif /* AURA_KWS_AURA_KWS_H */
