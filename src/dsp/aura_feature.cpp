/* Aura — 特征流水线的 C 封装实现
 *
 * 一层薄壳：把 aura::FeaturePipeline 包成纯 C。它是本仓库里唯一同时认识
 * aura_feature.h 与 aura_feature_pipeline.h 的地方 —— 算法节点（C）永远看不到
 * C++ 类，pipeline 层永远看不到 C 结构体。
 */
#include "dsp/aura_feature.h"

#include <cstdlib>
#include <cstring>
#include <new>
#include <vector>

#include "dsp/aura_feature_pipeline.h"
#include "utils/logger/logger.h"

#define TAG "dsp.feature"

struct aura_feature {
    aura::FeaturePipeline *pipe;
    int32_t                dim;
};

void aura_feature_cfg_default(aura_feature_cfg_t *cfg)
{
    if (cfg == NULL) {
        return;
    }
    cfg->sample_rate  = 16000;
    cfg->frame_length = 400;
    cfg->frame_shift  = 160;
    cfg->num_bins     = 80;
    cfg->low_freq     = 20.0f;
    cfg->high_freq    = -400.0f;
    cfg->snip_edges   = false;
}

int32_t aura_feature_dim(const aura_feature_t *f)
{
    return (f == NULL) ? 0 : f->dim;
}

aura_feature_t *aura_feature_create(const aura_feature_cfg_t *cfg)
{
    aura_feature_cfg_t def;
    if (cfg == NULL) {
        aura_feature_cfg_default(&def);
        cfg = &def;
    }
    if (cfg->sample_rate <= 0 || cfg->frame_length <= 0 ||
        cfg->frame_shift <= 0 || cfg->frame_shift > cfg->frame_length ||
        cfg->num_bins <= 0) {
        AURA_LOGE(TAG, "配置非法: sr=%d len=%d shift=%d bins=%d", cfg->sample_rate,
                  cfg->frame_length, cfg->frame_shift, cfg->num_bins);
        return NULL;
    }

    aura_feature_t *f = (aura_feature_t *)calloc(1, sizeof(*f));
    if (f == NULL) {
        AURA_LOGE(TAG, "分配 aura_feature 失败");
        return NULL;
    }
    f->dim = cfg->num_bins;

    /* kKaldi 铺好绝大多数默认值，这里只覆盖与训练侧不同的两个口径。 */
    aura::FeaturePipelineConfig pc(cfg->num_bins, cfg->sample_rate,
                                   aura::FeatureType::kKaldi);
    pc.low_freq   = cfg->low_freq;
    pc.high_freq  = cfg->high_freq;
    pc.snip_edges = cfg->snip_edges;

    try {
        f->pipe = new (std::nothrow) aura::FeaturePipeline(pc);
    } catch (...) {
        /* std::vector 分配失败等；C 接口不传播异常。 */
        f->pipe = nullptr;
    }
    if (f->pipe == NULL) {
        AURA_LOGE(TAG, "FeaturePipeline 构造失败 (bins=%d, sr=%d)", cfg->num_bins,
                  cfg->sample_rate);
        free(f);
        return NULL;
    }
    return f;
}

void aura_feature_destroy(aura_feature_t *f)
{
    if (f == NULL) {
        return;
    }
    delete f->pipe;
    free(f);
}

int32_t aura_feature_accept(aura_feature_t *f, const float *pcm, int32_t n_samples)
{
    if (f == NULL || pcm == NULL || n_samples < 0) {
        return AURA_ERR_INVALID_ARG;
    }
    try {
        f->pipe->AcceptWaveform(pcm, n_samples);
    } catch (...) {
        return AURA_ERR_NOMEM;
    }
    return AURA_OK;
}

int32_t aura_feature_ready(const aura_feature_t *f)
{
    return (f == NULL) ? 0 : (int32_t)f->pipe->NumQueuedFrames();
}

int32_t aura_feature_pop(aura_feature_t *f, float *feat_out)
{
    if (f == NULL || feat_out == NULL) {
        return AURA_ERR_INVALID_ARG;
    }
    std::vector<float> feat;
    if (!f->pipe->TryPopOne(&feat)) {
        return 0; /* 这一帧还没攒够 —— 非阻塞，不等待 */
    }
    memcpy(feat_out, feat.data(), sizeof(float) * feat.size());
    return 1;
}

int32_t aura_feature_num_frames(const aura_feature_t *f)
{
    return (f == NULL) ? 0 : f->pipe->num_frames();
}

void aura_feature_reset(aura_feature_t *f)
{
    if (f != NULL) {
        f->pipe->Reset();
    }
}
