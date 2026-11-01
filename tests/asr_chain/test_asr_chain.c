/* test_asr_chain — ASR 流式识别链（真模型 + 真音频，Phase 3 行为级验收）
 *
 * 喂模型自带的中文语音（KWS test_wavs 的唤醒词录音，内容是真人读关键词）
 * + 3s 静音尾（触发 endpoint 规则 1：2.4s 尾静音 → ASR_FINAL）。
 * 断言：partial 流产生（≥1）、final 触发（≥1）、final 文本非空，并打印识别文本。
 *
 * 精确文本断言 SKIP：这些音频没有人工标注，模型转出什么是什么 —— 这里只验
 * "流式链跑通、事件按时序到、文本面有产出"。有标注的音频接入后收紧断言。
 * 素材缺失（模型/音频）SKIP 返回成功（audio_front 约定）。
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "unit/aura_test.h"

#include "algorithm/aura_nn_adapter.h"
#include "asr/aura_asr.h"
#include "core/algorithm.h"
#include "core/pipeline/pipeline.h"
#include "unit/aura_event_log.h"

#ifndef AURA_HOST_SIM_SOURCE_DIR
#define AURA_HOST_SIM_SOURCE_DIR "."
#endif

#define MODEL_DIR \
    AURA_HOST_SIM_SOURCE_DIR "/models/asr/sherpa-mnn-streaming-zipformer-bilingual-zh-en-2023-02-20"
#define TEST_WAV_DIR \
    AURA_HOST_SIM_SOURCE_DIR "/models/kws/kws-zipformer-wenetspeech-3.3M-MNN/test_wavs"

/* ------------------------------------------------------------ WAV 读取 */

typedef struct {
    uint32_t sr;
    uint16_t channels;
    int16_t *data; /* 通道 0 */
    uint32_t frames;
} wav16_t;

static void wav16_free(wav16_t *w)
{
    free(w->data);
    w->data = NULL;
}

static bool wav16_load(const char *path, wav16_t *w)
{
    memset(w, 0, sizeof(*w));
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return false;
    }
    uint8_t hdr[12];
    if (fread(hdr, 1, 12, f) != 12 || memcmp(hdr, "RIFF", 4) != 0 ||
        memcmp(hdr + 8, "WAVE", 4) != 0) {
        fclose(f);
        return false;
    }
    uint16_t fmt = 0, bits = 0, channels = 0;
    uint32_t sr = 0, data_len = 0;
    for (;;) {
        uint8_t ck[8];
        if (fread(ck, 1, 8, f) != 8) {
            break;
        }
        uint32_t sz = (uint32_t)ck[4] | ((uint32_t)ck[5] << 8) |
                      ((uint32_t)ck[6] << 16) | ((uint32_t)ck[7] << 24);
        if (memcmp(ck, "fmt ", 4) == 0) {
            uint8_t fb[16];
            if (fread(fb, 1, 16, f) != 16) {
                break;
            }
            fmt      = (uint16_t)(fb[0] | (fb[1] << 8));
            channels = (uint16_t)(fb[2] | (fb[3] << 8));
            sr       = (uint32_t)fb[4] | ((uint32_t)fb[5] << 8) |
                 ((uint32_t)fb[6] << 16) | ((uint32_t)fb[7] << 24);
            bits = (uint16_t)(fb[14] | (fb[15] << 8));
            if (sz > 16) {
                fseek(f, (long)(sz - 16), SEEK_CUR);
            }
        } else if (memcmp(ck, "data", 4) == 0) {
            data_len = sz;
            break;
        } else {
            fseek(f, (long)(sz + (sz & 1u)), SEEK_CUR);
        }
    }
    if (fmt != 1 || bits != 16 || sr == 0 || channels == 0 || data_len == 0) {
        fclose(f);
        return false;
    }
    uint32_t n = data_len / (2u * channels);
    int16_t *tmp = (int16_t *)malloc(data_len);
    w->data = (int16_t *)malloc((size_t)n * 2);
    if (tmp == NULL || w->data == NULL) {
        free(tmp);
        fclose(f);
        return false;
    }
    if (fread(tmp, 1, data_len, f) != data_len) {
        free(tmp);
        fclose(f);
        return false;
    }
    for (uint32_t i = 0; i < n; i++) {
        w->data[i] = tmp[(size_t)i * channels];
    }
    free(tmp);
    fclose(f);
    w->sr       = sr;
    w->channels = channels;
    w->frames   = n;
    return true;
}

/* ------------------------------------------------------------ 测试 */

static bool model_present(void)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/encoder-epoch-99-avg-1.int8.mnn", MODEL_DIR);
    FILE *f = fopen(path, "rb");
    if (f != NULL) {
        fclose(f);
        return true;
    }
    return false;
}

static void feed_wav(aura_pipeline_t *p, const wav16_t *w, float silence_s)
{
    /* 按稍慢于实时的节拍喂（15ms/帧）：int8 模型在这台无 i8sdot 的 x86 上
     * 每 chunk 解码 ~370ms（> 320ms 实时预算），10ms 节拍会让引擎特征环
     * 溢出丢帧 —— 丢帧会连静音尾一起丢，endpoint 的尾静音永远数不满。
     * 板端（ARM NEON）快得多，喂入回到 10ms 即可。 */
    static int16_t sil[160] = {0};
    uint64_t        pts     = 0;
    uint32_t        off     = 0;
    while (off < w->frames) {
        uint32_t n = w->frames - off;
        if (n > 160) {
            n = 160;
        }
        AURA_ASSERT_EQ(aura_pipeline_feed(p, w->data + off, n, 1, AURA_SAMPLE_S16,
                                          pts, 1000),
                       AURA_OK);
        off += n;
        pts += (uint64_t)n * 1000000ull / w->sr;
        aura_test_sleep_ms(15);
    }
    int32_t nsil = (int32_t)(silence_s * 100.0f);
    for (int32_t i = 0; i < nsil; i++) {
        AURA_ASSERT_EQ(
            aura_pipeline_feed(p, sil, 160, 1, AURA_SAMPLE_S16, pts, 1000), AURA_OK);
        pts += 10000;
        aura_test_sleep_ms(15);
    }
}

AURA_TEST(test_asr_streaming)
{
    if (!model_present()) {
        printf("    SKIP: ASR 模型不在 %s（models/ 不入库）\n", MODEL_DIR);
        return;
    }

    aura_infer_cfg_t icfg;
    memset(&icfg, 0, sizeof(icfg));
    icfg.backend = AURA_INFER_BACKEND_CPU;
    icfg.threads = 2;
    AURA_ASSERT_EQ(aura_nn_init(AURA_HOST_SIM_SOURCE_DIR "/models", &icfg), AURA_OK);
    AURA_ASSERT_EQ(aura_asr_register(), AURA_OK);

    aura_chain_t c;
    aura_chain_init(&c);
    AURA_ASSERT_EQ(aura_chain_add(&c, "asr", NULL), AURA_OK);
    {
        /* 单测链没有 KWS/状态机：关掉唤醒门控（产品链上默认开启） */
        aura_algo_params_t *pa = aura_chain_params(&c, 0);
        AURA_ASSERT(pa != NULL);
        AURA_ASSERT_EQ(aura_algo_params_set_bool(pa, "require_wake", false), AURA_OK);
    }

    aura_pipeline_t *p = aura_pipeline_create(NULL);
    AURA_ASSERT(p != NULL);
    aura_node_t *nodes[4];
    uint32_t     ncount = 0;
    AURA_ASSERT_EQ(aura_chain_build(p, &c, nodes, &ncount), AURA_OK);

    aura_event_log_t log;
    AURA_ASSERT_EQ(aura_event_log_init(&log, aura_pipeline_bus(p),
                                       (1u << AURA_EVENT_ASR_PARTIAL) |
                                           (1u << AURA_EVENT_ASR_FINAL)),
                   AURA_OK);
    AURA_ASSERT_EQ(aura_pipeline_start(p), AURA_OK);

    wav16_t w;
    char    path[512];
    snprintf(path, sizeof(path), "%s/0.wav", TEST_WAV_DIR);
    if (!wav16_load(path, &w) || w.sr != 16000) {
        printf("    SKIP: 测试音频不可用（%s）\n", path);
        wav16_free(&w);
        aura_event_log_fini(&log);
        aura_pipeline_stop(p);
        aura_pipeline_destroy(p);
        aura_chain_destroy_nodes(nodes, ncount);
        aura_infer_deinit();
        return;
    }

    uint64_t t0 = aura_osal_time_ms();
    feed_wav(p, &w, 3.0f); /* 5.6s 语音 + 3s 静音（触发 2.4s endpoint 规则） */
    wav16_free(&w);

    /* 等 ASR_FINAL（不是任一事件 —— partial 早就来了，FINAL 要等尾静音
     * 数满 2.4s 的那个 chunk；worker 落后于喂入，给足时间） */
    AURA_ASSERT_EQ(aura_pipeline_wait_drained(p, 60000), AURA_OK);
    uint64_t deadline = aura_osal_time_ms() + 120000;
    while (aura_event_log_count(&log, AURA_EVENT_ASR_FINAL) == 0 &&
           aura_osal_time_ms() < deadline) {
        aura_test_sleep_ms(100);
    }

    uint32_t n_partial = aura_event_log_count(&log, AURA_EVENT_ASR_PARTIAL);
    uint32_t n_final   = aura_event_log_count(&log, AURA_EVENT_ASR_FINAL);
    printf("    partial=%u final=%u（喂入→首个事件 %llu ms）\n", n_partial, n_final,
           (unsigned long long)(aura_osal_time_ms() - t0));

    /* 结构断言：partial 流产生 + final 触发 + 文本非空。文本内容打印留档
     * （素材无标注，精确断言 SKIP —— 有标注音频后收紧）。 */
    AURA_CHECK(n_partial >= 1);
    AURA_CHECK(n_final >= 1);

    const aura_event_t *evf = aura_event_log_find(&log, AURA_EVENT_ASR_FINAL, 0);
    if (evf != NULL) {
        printf("    识别文本: \"%s\"\n", evf->payload);
        AURA_CHECK(evf->payload[0] != '\0');
        AURA_CHECK(evf->u.asr.is_final);
    }

    aura_event_log_fini(&log);
    aura_pipeline_stop(p);
    aura_pipeline_destroy(p);
    aura_chain_destroy_nodes(nodes, ncount);
    aura_infer_deinit();
}

AURA_TEST_MAIN()
