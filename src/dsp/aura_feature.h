/* Aura — 特征流水线的 C 封装（fbank）
 *
 * 内部是 aura::FeaturePipeline（dsp/aura_feature_pipeline.h，取自 WeNet 并适配），
 * 本文件给算法节点（C）一个纯 C 接口。KWS / ASR / 声纹各持一个实例，**各自喂同一
 * 段 PCM、各自 pop 帧** —— 三个模型共享同一套特征（16kHz / 25ms / 10ms / 80bin），
 * 不必每个节点自己攒一份。
 *
 * 线程模型：accept 与 pop 通常发生在同一个任务上下文（SYNC 节点）；FeaturePipeline
 * 本身线程安全（内部有阻塞队列），跨线程用也没问题 —— pop 是非阻塞的，所以
 * "这一帧还没攒够"只会拿到 0 帧，不会卡住 pipeline。
 */
#ifndef AURA_DSP_AURA_FEATURE_H
#define AURA_DSP_AURA_FEATURE_H

#include <stdbool.h>
#include <stdint.h>

#include "utils/error.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct aura_feature aura_feature_t;

typedef struct {
    int32_t sample_rate;  /* 16000 */
    int32_t frame_length; /* 400（25ms） */
    int32_t frame_shift;  /* 160（10ms） */
    int32_t num_bins;     /* 80 */
    float   low_freq;     /* 20 */
    /* 语义同 kaldi：<= 0 表示 Nyquist + high_freq。训练侧是 -400（即 7600Hz），
     * 0 会退到 Nyquist(8000)，高档 mel 滤波器整体平移 —— 三个模型都别用 0。 */
    float   high_freq;
    bool    snip_edges; /* false = 训练侧栅格（开头 120 点镜像，见 feature pipeline） */
} aura_feature_cfg_t;

/* 默认值 = 训练侧参数（16k/25ms/10ms/80bin/20Hz/-400Hz/snip_edges=false）。 */
void aura_feature_cfg_default(aura_feature_cfg_t *cfg);

aura_feature_t *aura_feature_create(const aura_feature_cfg_t *cfg);
void            aura_feature_destroy(aura_feature_t *f);

int32_t aura_feature_dim(const aura_feature_t *f);

/* 喂 n_samples 个采样（[-1,1] 单声道 float）。返回 0 或负错误码。 */
int32_t aura_feature_accept(aura_feature_t *f, const float *pcm, int32_t n_samples);

/* 当前可取多少帧（非阻塞口径）。 */
int32_t aura_feature_ready(const aura_feature_t *f);

/* 取一帧（dim 个 float 紧密排列）。队列空返回 0，取到返回 1。 */
int32_t aura_feature_pop(aura_feature_t *f, float *feat_out);

/* 累计已产出的帧数（诊断用）。 */
int32_t aura_feature_num_frames(const aura_feature_t *f);

/* 复位：清队列、清剩余采样、清镜像前缀状态（下一句话的栅格从头算）。 */
void aura_feature_reset(aura_feature_t *f);

#ifdef __cplusplus
}
#endif

#endif /* AURA_DSP_AURA_FEATURE_H */
