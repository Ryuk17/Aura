/* Aura — 播放 sink 节点与 HAL 后端注册
 *
 * `playback` 注册名用于 playback_chain：`tts, src, playback`。
 * 后端由应用在 start 前通过 aura_agent_set_playback_backend() 注入；
 * 未注入时使用 null 后端（丢弃音频，仅保留流与统计）。
 */
#ifndef AURA_CORE_PLAYBACK_PLAYBACK_H
#define AURA_CORE_PLAYBACK_PLAYBACK_H

#include "aura_playback.h"
#include "utils/error.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 回采回调：播放 sink 把**实际送播**的 PCM 交给 AEC 参考流。 */
typedef void (*aura_playback_ref_fn)(void *user, const int16_t *pcm, uint32_t frame_count,
                                     uint32_t channels, uint64_t pts_us);

/* 注册 "playback" 算法（幂等：已存在视为成功）。 */
aura_err_t aura_playback_register(void);

/* 设置 HAL 后端（拷贝结构体）。必须在 pipeline start 前调用。 */
void aura_playback_set_backend(const aura_playback_backend_t *backend);
const aura_playback_backend_t *aura_playback_backend(void);

/* 设置 AEC 参考回采回调（agent_core 注入 capture pipeline 的 feed_ref）。 */
void aura_playback_set_ref_sink(aura_playback_ref_fn fn, void *user);

#ifdef __cplusplus
}
#endif

#endif /* AURA_CORE_PLAYBACK_PLAYBACK_H */
