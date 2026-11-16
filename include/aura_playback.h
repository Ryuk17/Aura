/* Aura — 播放 HAL 接口
 *
 * 播放 sink 节点把 pipeline 输出交给板级音频设备。Aura 只定义回调契约，
 * ALSA / PulseAudio / 板级 SDK 实现在应用或 platform 层：
 *
 *   open  → 设备按 sample_rate/channels 打开（init/start 期一次）
 *   write → 写一帧 PCM（10ms 一帧；允许阻塞到设备可接收，但必须在
 *           barge-in 时能通过 flush 快速丢弃/停止）
 *   flush → 立即停止当前播放并丢弃未播数据（barge-in / stop）
 *   close → 关闭设备
 *
 * `write` 的 frame_count 是**每通道**样本数；channels 通常为 1（TTS 链）。
 * PCM 一律为 int16 交错。
 */
#ifndef AURA_PLAYBACK_H
#define AURA_PLAYBACK_H

#include <stdint.h>

#include "utils/error.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct aura_playback_backend {
    const char *name;
    void       *user;

    aura_err_t (*open)(void *user, uint32_t sample_rate, uint32_t channels);
    aura_err_t (*write)(void *user, const int16_t *pcm, uint32_t frame_count, uint32_t channels,
                        uint64_t pts_us);
    /* 可为 NULL。必须短小：在 pipeline flush 线程上同步调用。 */
    void (*flush)(void *user);
    void (*close)(void *user);
} aura_playback_backend_t;

#ifdef __cplusplus
}
#endif

#endif /* AURA_PLAYBACK_H */
