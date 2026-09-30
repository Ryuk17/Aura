/* Aura — host_sim 专用：最小 WAV 读写（16-bit PCM）
 *
 * 只服务仿真（Phase 1），不做重采样/多格式 —— 真正的音频 IO 在 platform/audio
 * （Phase 2）。16 kHz 单声道 16-bit 是 host_sim 的输入约定；立体声输入自动下混。
 */
#ifndef AURA_TESTS_HOST_SIM_WAV_IO_H
#define AURA_TESTS_HOST_SIM_WAV_IO_H

#include <stdint.h>

#include "utils/error.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 读 WAV：*samples 由内部 malloc（调用方 free）。返回样本数与采样率。 */
aura_err_t wav_read(const char *path, int16_t **samples, uint32_t *sample_count,
                    uint32_t *sample_rate);

/* 写 WAV：16 kHz 单声道 16-bit。 */
aura_err_t wav_write(const char *path, const int16_t *samples, uint32_t sample_count,
                     uint32_t sample_rate);

#ifdef __cplusplus
}
#endif

#endif /* AURA_TESTS_HOST_SIM_WAV_IO_H */
