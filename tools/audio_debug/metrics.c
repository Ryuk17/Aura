/* Aura — audio_debug 的客观指标（实现，口径见 metrics.h） */
#include "metrics.h"

#include <math.h>
#include <stddef.h>

/* 削波门限：满量程的 98%。留 2% 是因为很多实现（含 TrickRoom 的限幅器）
 * 会用 32767 做"贴顶但不溢出"的输出，硬取 ==32767 会把正常限幅也算成削波。 */
#define CLIP_LEVEL 32112 /* ≈ 0.98 × 32767 */

double aura_metrics_energy(const int16_t *samples, uint32_t frames, uint32_t channels,
                           uint32_t from, uint32_t to)
{
    if (samples == NULL || channels == 0 || from >= to) {
        return 0.0;
    }
    if (to > frames) {
        to = frames;
    }
    if (from >= to) {
        return 0.0;
    }
    const uint32_t n = to - from;
    double         sum = 0.0;
    for (uint32_t i = 0; i < n; i++) {
        const int16_t *p = samples + (size_t)(from + i) * channels;
        for (uint32_t c = 0; c < channels; c++) {
            const double v = (double)p[c];
            sum += v * v;
        }
    }
    return sum / ((double)n * (double)channels);
}

double aura_metrics_db(double num, double den)
{
    if (den <= 1e-12) {
        return 120.0;
    }
    if (num <= 1e-12) {
        return -120.0;
    }
    return 10.0 * log10(num / den);
}

double aura_metrics_ratio(double num, double den)
{
    if (den <= 1e-12) {
        return 0.0;
    }
    return num / den;
}

double aura_metrics_frames_mean(const double *frame_energy, uint32_t count, uint32_t from,
                                uint32_t to)
{
    if (frame_energy == NULL || from >= to) {
        return 0.0;
    }
    if (to > count) {
        to = count;
    }
    if (from >= to) {
        return 0.0;
    }
    double sum = 0.0;
    for (uint32_t i = from; i < to; i++) {
        sum += frame_energy[i];
    }
    return sum / (double)(to - from);
}

uint64_t aura_metrics_clipped(const int16_t *samples, uint32_t frames, uint32_t channels,
                              int32_t *out_peak)
{
    if (out_peak != NULL) {
        *out_peak = 0;
    }
    if (samples == NULL || channels == 0) {
        return 0;
    }
    uint64_t clipped = 0;
    int32_t  peak    = 0;
    const size_t total = (size_t)frames * channels;
    for (size_t i = 0; i < total; i++) {
        const int32_t v = samples[i];
        const int32_t a = (v < 0) ? -v : v;
        if (a > peak) {
            peak = a;
        }
        if (a >= CLIP_LEVEL) {
            clipped++;
        }
    }
    if (out_peak != NULL) {
        *out_peak = peak;
    }
    return clipped;
}
