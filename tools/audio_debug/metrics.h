/* Aura — audio_debug 的客观指标（能量 / 分贝 / 比值 / 削波）
 *
 * 只做"用耳朵也能判断，但用数字判断更省事"的那几件事。所有指标都按**段**统计
 * （帧号区间），不做逐帧断言 —— 逐帧的抖动里既有算法收敛过程也有调度噪声，
 * 拿它当成功判据只会得到一条时红时绿的测试。
 *
 * 口径（全篇统一，务必与 wavtap 的逐帧能量对齐）：
 *   能量 = 段内所有通道所有样本的均方，**除以 (样本数 × 通道数)**，
 *   即"每通道每个样本的平均平方"。多通道因此不会因为"通道多所以能量大"
 *   而显得占便宜：2 路输入 → 1 路波束的能量比反映的是真实的增益/损耗，
 *   而不是被通道数除掉了 2 倍。
 */
#ifndef AURA_TOOLS_AUDIO_DEBUG_METRICS_H
#define AURA_TOOLS_AUDIO_DEBUG_METRICS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 段内平均能量（每通道每样本的均方）。from/to 为**每通道样本下标**，to 不含。
 * from >= to 或 samples 为 NULL → 0。to > frames 时钳到 frames。 */
double aura_metrics_energy(const int16_t *samples, uint32_t frames, uint32_t channels,
                           uint32_t from, uint32_t to);

/* 10·log10(num/den)。den 小到没意义时返回 +120 dB（"无穷大"的可读近似），
 * 而不是 -inf/NaN —— 那两种值一旦流进 printf 或比较，后面全是谜题。 */
double aura_metrics_db(double num, double den);

/* num/den，den 小到没意义时返回 0。 */
double aura_metrics_ratio(double num, double den);

/* 对逐帧能量数组求段均值（下标为帧号）—— 与 wavtap 的 frame_energy 配套，
 * 用它算出来的段能量与 aura_metrics_energy 是同口径的。 */
double aura_metrics_frames_mean(const double *frame_energy, uint32_t count, uint32_t from,
                                uint32_t to);

/* 统计 |x| ≥ 0.98·32767 的样本数（"削波"），并把全段绝对值峰值写进 *out_peak。
 * out_peak 可为 NULL。全零/空段返回 0。 */
uint64_t aura_metrics_clipped(const int16_t *samples, uint32_t frames, uint32_t channels,
                              int32_t *out_peak);

#ifdef __cplusplus
}
#endif

#endif /* AURA_TOOLS_AUDIO_DEBUG_METRICS_H */
