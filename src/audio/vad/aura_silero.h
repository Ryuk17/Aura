/* Aura — silero VAD（注册入口）
 *
 * 模型路径（相对 model_root）：vad/silero_vad.mnn/silero_vad.mnn
 * （权重不入库，见 models/download.sh）。链路写法：chain = silero_vad, ...
 */
#ifndef AURA_AUDIO_VAD_AURA_SILERO_H
#define AURA_AUDIO_VAD_AURA_SILERO_H

#include "utils/error.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 注册 "silero_vad" 算法到全局注册表。幂等：已注册时直接返回 AURA_OK。 */
aura_err_t aura_silero_register(void);

#ifdef __cplusplus
}
#endif

#endif /* AURA_AUDIO_VAD_AURA_SILERO_H */
