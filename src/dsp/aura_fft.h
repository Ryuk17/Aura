/* Aura — 实数 FFT（NE10 后端）
 *
 * fbank 的底层：每 10ms 对一帧加窗信号做 FFT 取功率谱，是整条语音链上最热的一步。
 * 后端刻意与 TrickRoom 的 AEC3 用**同一个库、同一份源码**（cmake/neon_fft.cmake），
 * 免得语音前端出现"两套 FFT 算出的特征对不上"这种最难查的偏差。
 *
 * 只暴露"实数 → 功率谱"这一件事：fbank 只要功率，复数输出的中间态没有第二个
 * 使用者，多一个 API 就多一份要维护的契约。将来真有模块要复数谱，在这里加一个
 * aura_fft_spectrum() 即可（NE10 的 cfg 是现成的），不用改调用方。
 *
 * 数值约定（与 kaldi / kaldi-native-fbank 对齐，也就是模型训练侧的约定）：
 *   - 正变换**不缩放**，输出即朴素 DFT（NE10: is_forward_scaled = 0）
 *   - out[0] = |X[0]|²（DC），out[k] = |X[k]|², k = 1 .. nfft/2-1
 *   - **不含 Nyquist**：kaldi 的 num_fft_bins = nfft/2，第 nfft/2 个 bin 从不参与
 *     mel 滤波，所以这里也直接丢掉 —— 留在数组里只会让下游以为它有意义
 */
#ifndef AURA_DSP_AURA_FFT_H
#define AURA_DSP_AURA_FFT_H

#include <stdint.h>

#include "utils/error.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct aura_fft aura_fft_t;

/* nfft 必须是 >= 2 的 2 的幂（NE10 r2c 的约束）。失败返回 NULL。 */
aura_fft_t *aura_fft_create(int32_t nfft);

void aura_fft_destroy(aura_fft_t *fft);

/* 可用的功率谱点数 = nfft/2。fft 为 NULL 时返回 0。 */
int32_t aura_fft_bins(const aura_fft_t *fft);

/* 一次实数 FFT 的功率谱：in 读 nfft 个 float，out 写 nfft/2 个 float。
 * 两个缓冲都由调用方提供（算法层运行期不分配内存）。in 不会被改写。 */
aura_err_t aura_fft_power(aura_fft_t *fft, const float *in, float *out);

#ifdef __cplusplus
}
#endif

#endif /* AURA_DSP_AURA_FFT_H */
