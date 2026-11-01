/* test_zipformer — zipformer2 流式引擎冒烟
 *
 * 验四件事（按"谁先坏、坏在哪"排序）：
 *   1. 装配期校验：三模型加载、metadata 读齐、状态张量形状/dtype 对齐 ——
 *      任何一处对不上都在 create 时报错，而不是跑起来出垃圾数；
 *   2. 真前向：全零特征喂满 chunk → decode_chunk 产出 out_frames 帧 →
 *      decode_frame 逐帧出**有限** logits，且不是纯零（layernorm 偏置保证）；
 *   3. 状态携带：chunk2 的第 0 帧与 chunk1 的第 0 帧不同 —— 状态确实在跨 chunk
 *      流动，而不是每次都被当首 chunk 重算；
 *   4. reset 复现：reset 后重喂重算，与 chunk1 的第 0 帧一致（阈值 1e-3 ——
 *      MNN 多线程归约的浮点尾差可到 1e-5~1e-4；断言的是"状态确实归零"）。
 *
 * 素材约定：模型文件不在 git 里（models/ 由用户手工转换），缺失时打印 SKIP
 * 返回成功 —— 与 tests/audio_front 的约定一致。真·行为断言（唤醒词命中）
 * 在 M1-3 的 kws_chain 里做，那才需要真音频。
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aura_test.h"
#include "dsp/zipformer/aura_zipformer.h"
#include "inference/inference.h"

#ifndef AURA_HOST_SIM_SOURCE_DIR
#define AURA_HOST_SIM_SOURCE_DIR "."
#endif

#define MODEL_DIR \
    AURA_HOST_SIM_SOURCE_DIR "/models/kws/kws-zipformer-wenetspeech-3.3M-MNN"

static bool model_present(void)
{
    char path[512];
    snprintf(path, sizeof(path),
             "%s/encoder-epoch-12-avg-2-chunk-16-left-64.mnn", MODEL_DIR);
    FILE *f = fopen(path, "rb");
    if (f != NULL) {
        fclose(f);
        return true;
    }
    return false;
}

AURA_TEST(test_zipformer_smoke)
{
    if (!model_present()) {
        printf("    SKIP: KWS 模型不在 %s（models/ 不入库，用户手工转换）\n",
               MODEL_DIR);
        return;
    }

    aura_infer_cfg_t icfg;
    memset(&icfg, 0, sizeof(icfg));
    icfg.backend = AURA_INFER_BACKEND_CPU;
    icfg.threads = 2;
    AURA_ASSERT_EQ(aura_infer_init(&icfg), AURA_OK);

    aura_zipformer_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.encoder_path = MODEL_DIR "/encoder-epoch-12-avg-2-chunk-16-left-64.mnn";
    cfg.decoder_path = MODEL_DIR "/decoder-epoch-12-avg-2-chunk-16-left-64.mnn";
    cfg.joiner_path  = MODEL_DIR "/joiner-epoch-12-avg-2-chunk-16-left-64.mnn";
    cfg.feature_dim  = 80;

    aura_zipformer_t *z = aura_zipformer_create(&cfg);
    AURA_ASSERT(z != NULL);
    if (z == NULL) {
        aura_infer_deinit();
        return;
    }

    /* 超参 sanity（打印留档；具体值以模型 metadata 为准，不作硬断言） */
    int32_t T          = aura_zipformer_T(z);
    int32_t shift      = aura_zipformer_chunk_shift(z);
    int32_t out_frames = aura_zipformer_output_frames(z);
    int32_t vocab      = aura_zipformer_vocab_size(z);
    int32_t ctx        = aura_zipformer_context_size(z);
    int32_t enc_dim    = aura_zipformer_encoder_dim(z);
    printf("    T=%d shift=%d out_frames=%d vocab=%d context=%d enc_dim=%d\n", T,
           shift, out_frames, vocab, ctx, enc_dim);
    AURA_CHECK(T > 0 && T <= 100);
    AURA_CHECK(shift > 0 && shift <= T);
    AURA_ASSERT_EQ(out_frames, shift / 4);
    AURA_CHECK(vocab > 0 && vocab < 10000);
    AURA_CHECK(ctx > 0 && ctx <= 8);
    AURA_CHECK(enc_dim > 0 && enc_dim < 4096);

    float *feat    = (float *)calloc(80, sizeof(float));
    float *logits0 = (float *)malloc((size_t)vocab * sizeof(float)); /* chunk1 第 0 帧留档 */
    float *logits  = (float *)malloc((size_t)vocab * sizeof(float)); /* 工作缓冲 */
    float *logits2 = (float *)malloc((size_t)vocab * sizeof(float));
    AURA_ASSERT(feat != NULL && logits0 != NULL && logits != NULL && logits2 != NULL);

    /* 初始上下文：[-1, ..., -1, 0]（sherpa 的 blank_hyp：末位 blank=0） */
    int32_t ys_init[8];
    memset(ys_init, -1, sizeof(ys_init));
    ys_init[ctx - 1] = 0;

    /* ---- ready 是严格大于：攒满 T 帧还不算，T+1 才算 */
    for (int32_t i = 0; i < T; i++) {
        AURA_CHECK(aura_zipformer_accept(z, feat, 1) >= 0);
    }
    AURA_CHECK(!aura_zipformer_ready(z));
    /* 未就绪就 decode → STATE（要先在"没喂满"时走一遍，别等喂满再测） */
    AURA_ASSERT_EQ(aura_zipformer_decode_chunk(z), AURA_ERR_STATE);

    AURA_ASSERT_EQ(aura_zipformer_accept(z, feat, 1), T + 1);
    AURA_CHECK(aura_zipformer_ready(z));

    /* ---- chunk 1：真前向 */
    AURA_ASSERT_EQ(aura_zipformer_decode_chunk(z), out_frames);
    bool any_nonzero = false;
    for (int32_t t = 0; t < out_frames; t++) {
        AURA_ASSERT_EQ(aura_zipformer_decode_frame(z, t, ys_init, logits), AURA_OK);
        if (t == 0) {
            /* 第 0 帧留档 —— 后面 chunk2/reset 都比它。注意别拿循环末尾的缓冲
             * 当第 0 帧：logits 每轮都被覆盖。 */
            memcpy(logits0, logits, (size_t)vocab * sizeof(float));
        }
        for (int32_t k = 0; k < vocab; k++) {
            AURA_CHECK(isfinite(logits[k]));
            if (logits[k] != 0.0f) {
                any_nonzero = true;
            }
        }
    }
    AURA_CHECK(any_nonzero); /* 全零特征也有 layernorm 偏置，不该是纯零 */

    /* ---- chunk 2：状态携带（第 0 帧应与 chunk 1 的第 0 帧不同） */
    for (int32_t i = 0; i < shift; i++) {
        AURA_CHECK(aura_zipformer_accept(z, feat, 1) >= 0);
    }
    AURA_CHECK(aura_zipformer_ready(z));
    AURA_ASSERT_EQ(aura_zipformer_decode_chunk(z), out_frames);
    AURA_ASSERT_EQ(aura_zipformer_decode_frame(z, 0, ys_init, logits2), AURA_OK);
    {
        bool differs = false;
        for (int32_t k = 0; k < vocab; k++) {
            if (fabsf(logits2[k] - logits0[k]) > 1e-4f) {
                differs = true;
            }
        }
        AURA_CHECK(differs);
    }

    /* ---- reset 复现：从零重算必须与 chunk 1 第 0 帧一致（确定性）
     * 阈值 1e-3 而非逐位：MNN 多线程归约的浮点尾差可以到 1e-5~1e-4 量级，
     * 这里断言的是"reset 后状态确实归零"，不是"位级确定"。 */
    aura_zipformer_reset(z);
    AURA_ASSERT_EQ(aura_zipformer_num_frames(z), 0);
    AURA_CHECK(!aura_zipformer_ready(z));
    for (int32_t i = 0; i < T + 1; i++) {
        AURA_CHECK(aura_zipformer_accept(z, feat, 1) >= 0);
    }
    AURA_ASSERT_EQ(aura_zipformer_decode_chunk(z), out_frames);
    AURA_ASSERT_EQ(aura_zipformer_decode_frame(z, 0, ys_init, logits2), AURA_OK);
    {
        float max_diff = 0.0f;
        for (int32_t k = 0; k < vocab; k++) {
            float d = fabsf(logits2[k] - logits0[k]);
            if (d > max_diff) {
                max_diff = d;
            }
        }
        if (max_diff > 1e-3f) {
            printf("    reset 复现最大偏差 %.6f（> 1e-3）\n", (double)max_diff);
        }
        AURA_CHECK(max_diff < 1e-3f);
    }

    free(feat);
    free(logits0);
    free(logits);
    free(logits2);
    aura_zipformer_destroy(z);
    aura_infer_deinit();
}

/* zipformer v1（ASR bilingual int8）冒烟：与 v2 同一套断言，但状态布局不同
 * （7/block、cached_len 是 int32、无 processed_lens）—— 装配期能过就说明
 * 分派正确；真·识别文本断言在 tests/asr_chain。 */
AURA_TEST(test_zipformer_v1_smoke)
{
    char path[512];
    snprintf(path, sizeof(path),
             "%s/asr/sherpa-mnn-streaming-zipformer-bilingual-zh-en-2023-02-20/"
             "encoder-epoch-99-avg-1.int8.mnn",
             AURA_HOST_SIM_SOURCE_DIR "/models");
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        printf("    SKIP: ASR 模型不在 %s（models/ 不入库）\n",
               AURA_HOST_SIM_SOURCE_DIR "/models");
        return;
    }
    fclose(f);

    aura_infer_cfg_t icfg;
    memset(&icfg, 0, sizeof(icfg));
    icfg.backend = AURA_INFER_BACKEND_CPU;
    icfg.threads = 2;
    AURA_ASSERT_EQ(aura_infer_init(&icfg), AURA_OK);

    aura_zipformer_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.encoder_path = AURA_HOST_SIM_SOURCE_DIR
        "/models/asr/sherpa-mnn-streaming-zipformer-bilingual-zh-en-2023-02-20/"
        "encoder-epoch-99-avg-1.int8.mnn";
    cfg.decoder_path = AURA_HOST_SIM_SOURCE_DIR
        "/models/asr/sherpa-mnn-streaming-zipformer-bilingual-zh-en-2023-02-20/"
        "decoder-epoch-99-avg-1.int8.mnn";
    cfg.joiner_path = AURA_HOST_SIM_SOURCE_DIR
        "/models/asr/sherpa-mnn-streaming-zipformer-bilingual-zh-en-2023-02-20/"
        "joiner-epoch-99-avg-1.int8.mnn";
    cfg.feature_dim = 80;

    aura_zipformer_t *z = aura_zipformer_create(&cfg);
    AURA_ASSERT(z != NULL);
    if (z == NULL) {
        aura_infer_deinit();
        return;
    }

    int32_t T          = aura_zipformer_T(z);
    int32_t shift      = aura_zipformer_chunk_shift(z);
    int32_t out_frames = aura_zipformer_output_frames(z);
    int32_t vocab      = aura_zipformer_vocab_size(z);
    int32_t ctx        = aura_zipformer_context_size(z);
    int32_t enc_dim    = aura_zipformer_encoder_dim(z);
    printf("    v1: T=%d shift=%d out_frames=%d vocab=%d context=%d enc_dim=%d\n",
           T, shift, out_frames, vocab, ctx, enc_dim);
    AURA_CHECK(T > 0 && T <= 100);
    AURA_CHECK(shift > 0 && shift <= T);
    AURA_ASSERT_EQ(out_frames, shift / 4);
    AURA_CHECK(vocab > 0 && vocab < 100000);
    AURA_CHECK(ctx > 0 && ctx <= 8);
    AURA_CHECK(enc_dim > 0 && enc_dim < 4096);

    float *feat   = (float *)calloc(80, sizeof(float));
    float *logits = (float *)malloc((size_t)vocab * sizeof(float));
    AURA_ASSERT(feat != NULL && logits != NULL);

    int32_t ys_init[8];
    memset(ys_init, -1, sizeof(ys_init));
    ys_init[ctx - 1] = 0;

    for (int32_t i = 0; i < T + 1; i++) {
        AURA_CHECK(aura_zipformer_accept(z, feat, 1) >= 0);
    }
    AURA_ASSERT_EQ(aura_zipformer_decode_chunk(z), out_frames);
    AURA_ASSERT_EQ(aura_zipformer_decode_frame(z, 0, ys_init, logits), AURA_OK);
    bool any_nonzero = false;
    for (int32_t k = 0; k < vocab; k++) {
        AURA_CHECK(isfinite(logits[k]));
        if (logits[k] != 0.0f) {
            any_nonzero = true;
        }
    }
    AURA_CHECK(any_nonzero);

    free(feat);
    free(logits);
    aura_zipformer_destroy(z);
    aura_infer_deinit();
}

AURA_TEST_MAIN()
