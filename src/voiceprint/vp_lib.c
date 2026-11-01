/* Aura — 声纹库文件 + 离线 embedding 实现（见 vp_lib.h） */
#include "voiceprint/vp_lib.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dsp/aura_feature.h"
#include "inference/inference.h"
#include "utils/logger/logger.h"

#define TAG "vp.lib"

#define AURA_VP_LIB_MAGIC 0x4C505641u /* "AVPL" */
#define AURA_VP_LIB_VERSION 1

/* 与验证同口径：能量最高的有效语音窗口（≤ max_s）。 */
uint32_t aura_vp_energy_window(const int16_t *pcm, uint32_t n, uint32_t rate,
                               float max_s)
{
    uint32_t win = (uint32_t)(max_s * (float)rate);
    if (n <= win) {
        return 0;
    }
    uint32_t frame      = rate / 100; /* 10ms */
    uint32_t nframes    = n / frame;
    uint32_t win_frames = win / frame;
    if (win_frames == 0 || nframes <= win_frames) {
        return 0;
    }
    uint64_t e[600];
    if (nframes > 600) {
        nframes = 600;
    }
    for (uint32_t f = 0; f < nframes; f++) {
        uint64_t s = 0;
        for (uint32_t i = 0; i < frame; i++) {
            int32_t v = pcm[f * frame + i];
            s += (uint64_t)v * (uint64_t)v;
        }
        e[f] = s;
    }
    uint64_t best_sum = 0;
    uint32_t best_f   = 0;
    uint64_t sum      = 0;
    for (uint32_t f = 0; f < win_frames; f++) {
        sum += e[f];
    }
    best_sum = sum;
    for (uint32_t f = 1; f + win_frames <= nframes; f++) {
        sum = sum - e[f - 1] + e[f + win_frames - 1];
        if (sum > best_sum) {
            best_sum = sum;
            best_f   = f;
        }
    }
    return best_f * frame;
}

aura_err_t aura_vp_lib_load(const char *path, aura_vp_entry_t *entries, int32_t max,
                            int32_t *count)
{
    if (entries == NULL || count == NULL || max <= 0) {
        return AURA_ERR_INVALID_ARG;
    }
    *count = 0;
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return AURA_ERR_NOT_FOUND;
    }
    uint32_t hdr[3];
    if (fread(hdr, 4, 3, f) != 3 || hdr[0] != AURA_VP_LIB_MAGIC ||
        hdr[1] != AURA_VP_LIB_VERSION) {
        AURA_LOGE(TAG, "库文件头非法: %s", path);
        fclose(f);
        return AURA_ERR_MODEL;
    }
    int32_t n = (int32_t)hdr[2];
    if (n > max) {
        AURA_LOGE(TAG, "库文件条目 %d 超上限 %d", n, max);
        fclose(f);
        return AURA_ERR_MODEL;
    }
    if (fread(entries, sizeof(aura_vp_entry_t), (size_t)n, f) != (size_t)n) {
        AURA_LOGE(TAG, "库文件条目读取失败: %s", path);
        fclose(f);
        return AURA_ERR_IO;
    }
    fclose(f);
    *count = n;
    return AURA_OK;
}

aura_err_t aura_vp_lib_save(const char *path, const aura_vp_entry_t *entries,
                            int32_t count)
{
    if (path == NULL || count < 0 || (count > 0 && entries == NULL)) {
        return AURA_ERR_INVALID_ARG;
    }
    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        AURA_LOGE(TAG, "写库文件失败: %s", path);
        return AURA_ERR_IO;
    }
    uint32_t hdr[3] = {AURA_VP_LIB_MAGIC, AURA_VP_LIB_VERSION, (uint32_t)count};
    bool ok = fwrite(hdr, 4, 3, f) == 3 &&
              (count == 0 ||
               fwrite(entries, sizeof(aura_vp_entry_t), (size_t)count, f) ==
                   (size_t)count);
    fclose(f);
    return ok ? AURA_OK : AURA_ERR_IO;
}

aura_err_t aura_vp_embed_offline(const int16_t *pcm, uint32_t n, uint32_t sample_rate,
                                 const char *model_path, float emb_out[AURA_VP_EMB_DIM])
{
    if (pcm == NULL || n == 0 || model_path == NULL || emb_out == NULL) {
        return AURA_ERR_INVALID_ARG;
    }

    /* 能量窗口（与节点的验证同口径） */
    uint32_t off = aura_vp_energy_window(pcm, n, sample_rate, 3.0f);
    const int16_t *use = pcm + off;
    uint32_t use_n = n - off;
    uint32_t win_n = 3u * sample_rate;
    if (use_n > win_n) {
        use_n = win_n;
    }

    /* 临时特征 */
    aura_feature_cfg_t fc;
    aura_feature_cfg_default(&fc);
    fc.sample_rate = (int32_t)sample_rate;
    aura_feature_t *feat = aura_feature_create(&fc);
    if (feat == NULL) {
        return AURA_ERR_NOMEM;
    }
    float *fpcm = (float *)malloc(sizeof(float) * use_n);
    if (fpcm == NULL) {
        aura_feature_destroy(feat);
        return AURA_ERR_NOMEM;
    }
    for (uint32_t i = 0; i < use_n; i++) {
        fpcm[i] = (float)use[i] * (1.0f / 32768.0f);
    }
    (void)aura_feature_accept(feat, fpcm, (int32_t)use_n);
    free(fpcm);

    float *feat_buf = (float *)malloc(sizeof(float) * 300 * 80);
    if (feat_buf == NULL) {
        aura_feature_destroy(feat);
        return AURA_ERR_NOMEM;
    }
    int32_t nf = 0;
    while (aura_feature_ready(feat) > 0 && nf < 300) {
        if (aura_feature_pop(feat, feat_buf + (size_t)nf * 80) <= 0) {
            break;
        }
        nf++;
    }
    aura_feature_destroy(feat);
    if (nf < 20) {
        free(feat_buf);
        AURA_LOGE(TAG, "离线注册音频太短（%d 帧 < 20）", nf);
        return AURA_ERR_INVALID_ARG;
    }

    /* 独立模型实例：注册与运行中的链路零共享 */
    aura_infer_model_t *model =
        aura_infer_load_in_group(NULL, model_path, AURA_INFER_FORM_MODULE);
    if (model == NULL) {
        free(feat_buf);
        return AURA_ERR_MODEL;
    }
    aura_err_t rc = AURA_OK;
    aura_tensor_t *x = aura_model_input(model, NULL);
    int32_t dims[3] = {1, nf, 80};
    aura_tensor_t *e = NULL;
    if (x == NULL || aura_tensor_resize(x, dims, 3) != AURA_OK ||
        aura_tensor_write(x, feat_buf, (uint64_t)nf * 80 * 4) != AURA_OK ||
        aura_model_run(model) != AURA_OK ||
        (e = aura_model_output(model, NULL)) == NULL ||
        aura_tensor_read(e, emb_out, AURA_VP_EMB_DIM * 4) != AURA_OK) {
        rc = AURA_ERR_MODEL;
    }
    aura_infer_unload(model);
    free(feat_buf);
    if (rc != AURA_OK) {
        AURA_LOGE(TAG, "离线 embedding 前向失败");
        return rc;
    }

    float norm = 0.0f;
    for (int32_t i = 0; i < AURA_VP_EMB_DIM; i++) {
        norm += emb_out[i] * emb_out[i];
    }
    norm = sqrtf(norm);
    if (norm < 1e-8f) {
        return AURA_ERR_MODEL;
    }
    for (int32_t i = 0; i < AURA_VP_EMB_DIM; i++) {
        emb_out[i] /= norm;
    }
    return AURA_OK;
}
