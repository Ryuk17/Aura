/* Aura — 实数 FFT（NE10 后端）实现
 *
 * 调用姿势照抄 TrickRoom 的 aec3_fft.cc（那是本仓库里唯一已验证过的 NE10 用法）：
 * 直接用 `_c` 后缀的通用 C 实现，**不经过 ne10_init() 的函数指针分发**。
 * 理由：我们只编了通用 C 源（cmake/neon_fft.cmake 里明确排除了 .neon.c /
 * .neonintrinsic.c），函数指针表在 x86 上根本没有对应实现可装；走分发只是多一跳、
 * 多一个"忘了调 ne10_init 就空指针"的坑。TrickRoom 也是这么干的。
 */
#include "dsp/aura_fft.h"

#include <stdlib.h>
#include <string.h>

#include "NE10_dsp.h"

#include "utils/logger/logger.h"

#define TAG "dsp.fft"

struct aura_fft {
    int32_t                     nfft;
    int32_t                     ncfft;   /* nfft/2：对外暴露的功率谱点数 */
    ne10_fft_r2c_cfg_float32_t  cfg;     /* NE10 的旋转因子/临时缓冲，create 时分配一次 */
    float                      *scratch; /* nfft 个 float，喂给 NE10 的输入副本 */
    ne10_fft_cpx_float32_t     *spec;    /* ncfft+1 个复数（NE10 会写到含 Nyquist 那一点） */
};

int32_t aura_fft_bins(const aura_fft_t *fft)
{
    return (fft == NULL) ? 0 : fft->ncfft;
}

aura_fft_t *aura_fft_create(int32_t nfft)
{
    if (nfft < 2 || (nfft & (nfft - 1)) != 0) {
        AURA_LOGE(TAG, "nfft 必须是 >= 2 的 2 的幂，收到 %d", nfft);
        return NULL;
    }

    aura_fft_t *f = (aura_fft_t *)calloc(1, sizeof(*f));
    if (f == NULL) {
        AURA_LOGE(TAG, "分配 aura_fft 失败");
        return NULL;
    }
    f->nfft  = nfft;
    f->ncfft = nfft / 2;

    f->cfg     = ne10_fft_alloc_r2c_float32(nfft);
    f->scratch = (float *)calloc((size_t)nfft, sizeof(float));
    f->spec    = (ne10_fft_cpx_float32_t *)calloc((size_t)f->ncfft + 1, sizeof(*f->spec));
    if (f->cfg == NULL || f->scratch == NULL || f->spec == NULL) {
        AURA_LOGE(TAG, "NE10 初始化失败 (nfft=%d)", nfft);
        aura_fft_destroy(f);
        return NULL;
    }

    /* 预热一次全零输入。NE10 的 cfg 里有惰性初始化的表，第一次调用比后续慢一个
     * 量级；放在 create 里吃掉，免得它落在第一帧音频上 —— 那正是实时性最吃紧的
     * 时刻，也正是最容易把"首帧抖动"误判成"性能不够"的地方。 */
    ne10_fft_r2c_1d_float32_c(f->spec, f->scratch, f->cfg);

    return f;
}

void aura_fft_destroy(aura_fft_t *fft)
{
    if (fft == NULL) {
        return;
    }
    if (fft->cfg != NULL) {
        ne10_fft_destroy_r2c_float32(fft->cfg);
    }
    free(fft->scratch);
    free(fft->spec);
    free(fft);
}

aura_err_t aura_fft_power(aura_fft_t *fft, const float *in, float *out)
{
    if (fft == NULL || in == NULL || out == NULL) {
        return AURA_ERR_INVALID_ARG;
    }

    /* 拷一份再交给 NE10：它的入参签名是 non-const（`ne10_float32_t *fin`），
     * 而我们想给调用方一个 const 的干净契约。1.6KB 的 memcpy 相对这次 FFT
     * 是 1% 量级，换掉一个"以后 NE10 改成 in-place 就会静默改坏调用方数据"的
     * 隐患，值。 */
    memcpy(fft->scratch, in, sizeof(float) * (size_t)fft->nfft);
    ne10_fft_r2c_1d_float32_c(fft->spec, fft->scratch, fft->cfg);

    /* NE10 的输出布局：spec[0] = {DC, 0}，spec[k] = {re, im} (1 <= k < nfft/2)，
     * spec[nfft/2] = {Nyquist, 0}。这里取前 nfft/2 个 —— 含 DC、不含 Nyquist。
     * spec[0].i 恒为 0，所以同一个式子对 DC 也成立，不必特判。
     *
     * 符号无关：NE10 的旋转因子用 e^{-jω}，kaldi 系用 e^{+jω}，两者互为共轭；
     * 功率谱只跟模有关，共轭不改变结果。 */
    for (int32_t k = 0; k < fft->ncfft; k++) {
        out[k] = fft->spec[k].r * fft->spec[k].r + fft->spec[k].i * fft->spec[k].i;
    }
    return AURA_OK;
}
