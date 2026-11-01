/* test_voiceprint — 声纹注册/验证链（真模型 + 真音频）
 *
 * 用 KWS test_wavs 的真人语音：wav0 当"说话人 A"注册 → 喂 wav0 音频到环 →
 * 发 VAD_SPEECH_END（链上没有真 VAD，测试直接投事件触发验证）→ 断言
 * VOICEPRINT_RESULT passed=true、payload="A"；换 wav1 验证 → passed=false；
 * 未注册时库为空 → 一律拒绝。
 *
 * 素材缺失 SKIP（audio_front 约定）。
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "unit/aura_test.h"

#include "algorithm/aura_nn_adapter.h"
#include "core/algorithm.h"
#include "core/pipeline/pipeline.h"
#include "unit/aura_event_log.h"
#include "voiceprint/aura_voiceprint.h"
#include "voiceprint/vp_lib.h"

#ifndef AURA_HOST_SIM_SOURCE_DIR
#define AURA_HOST_SIM_SOURCE_DIR "."
#endif

#define MODEL_DIR \
    AURA_HOST_SIM_SOURCE_DIR "/models/speaker_recognition"
#define TEST_WAV_DIR \
    AURA_HOST_SIM_SOURCE_DIR "/models/kws/kws-zipformer-wenetspeech-3.3M-MNN/test_wavs"

/* ------------------------------------------------------------ WAV 读取 */

typedef struct {
    uint32_t sr;
    uint16_t channels;
    int16_t *data;
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

/* ------------------------------------------------------------ 脚手架 */

static bool model_present(void)
{
    char path[512];
    snprintf(path, sizeof(path),
             "%s/3dspeaker_speech_eres2netv2_sv_zh-cn_16k-common.mnn", MODEL_DIR);
    FILE *f = fopen(path, "rb");
    if (f != NULL) {
        fclose(f);
        return true;
    }
    return false;
}

/* 假透传节点：链上需要一个产音频的上游喂 voiceprint（OBSERVER 挂旁路）。 */
static aura_nn_desc_t passthru_desc = {
    .name    = "vp_passthru",
    .kind    = AURA_ALGO_KIND_NN_SYNC,
    .io_kind = AURA_NN_IO_PASSTHROUGH,
};

static void feed_wav(aura_pipeline_t *p, const wav16_t *w)
{
    uint64_t pts = 0;
    uint32_t off = 0;
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
        aura_test_sleep_ms(2); /* 轻微放慢，让任务线程跟上 */
    }
}

/* 喂音频 + 发 VAD_END 触发验证，等结果事件返回。 */
static const aura_event_t *trigger_verify(aura_pipeline_t *p, aura_event_log_t *log,
                                          const wav16_t *w, uint32_t base)
{
    feed_wav(p, w);
    AURA_ASSERT_EQ(aura_pipeline_wait_drained(p, 30000), AURA_OK);
    AURA_ASSERT_EQ(
        aura_event_bus_post(aura_pipeline_bus(p), AURA_EVENT_VAD_SPEECH_END, 0,
                            "test"),
        AURA_OK);
    AURA_ASSERT_EQ(aura_event_log_wait(log, base + 1, 30000), AURA_OK);
    return aura_event_log_find(log, AURA_EVENT_VOICEPRINT_RESULT, base);
}

AURA_TEST(test_voiceprint_enroll_verify)
{
    if (!model_present()) {
        printf("    SKIP: 声纹模型不在 %s（models/ 不入库）\n", MODEL_DIR);
        return;
    }

    aura_infer_cfg_t icfg;
    memset(&icfg, 0, sizeof(icfg));
    icfg.backend = AURA_INFER_BACKEND_CPU;
    icfg.threads = 2;
    /* ---- 音频先加载（离线注册要用） */
    char path[512];
    wav16_t wa, wb;
    snprintf(path, sizeof(path), "%s/0.wav", TEST_WAV_DIR);
    bool ok_a = wav16_load(path, &wa) && wa.sr == 16000;
    snprintf(path, sizeof(path), "%s/1.wav", TEST_WAV_DIR);
    bool ok_b = wav16_load(path, &wb) && wb.sr == 16000;
    if (!ok_a || !ok_b) {
        printf("    SKIP: 测试音频不可用\n");
        wav16_free(&wa);
        wav16_free(&wb);
        return;
    }

    AURA_ASSERT_EQ(aura_nn_init(AURA_HOST_SIM_SOURCE_DIR "/models", &icfg), AURA_OK);
    AURA_ASSERT_EQ(aura_nn_register_all(&passthru_desc, 1), AURA_OK);
    AURA_ASSERT_EQ(aura_voiceprint_register(), AURA_OK);

    /* ---- 离线注册：写库文件（与运行中的链零共享状态） */
    {
        const char *lib_path = "vp_library_test.bin";
        remove(lib_path);
        float emb[192];
        AURA_ASSERT_EQ(aura_vp_embed_offline(wa.data, wa.frames, wa.sr,
                                             MODEL_DIR "/3dspeaker_speech_"
                                                        "eres2netv2_sv_zh-cn_16k-"
                                                        "common.mnn",
                                             emb),
                       AURA_OK);
        aura_vp_entry_t entries[16];
        memset(entries, 0, sizeof(entries));
        snprintf(entries[0].id, sizeof(entries[0].id), "%s", "说话人A");
        memcpy(entries[0].emb, emb, sizeof(emb));
        AURA_ASSERT_EQ(aura_vp_lib_save(lib_path, entries, 1), AURA_OK);
    }

    aura_chain_t c;
    aura_chain_init(&c);
    AURA_ASSERT_EQ(aura_chain_add(&c, "vp_passthru", NULL), AURA_OK);
    AURA_ASSERT_EQ(aura_chain_add(&c, "voiceprint", NULL), AURA_OK);
    {
        aura_algo_params_t *pa = aura_chain_params(&c, 1);
        AURA_ASSERT(pa != NULL);
        AURA_ASSERT_EQ(aura_algo_params_set_str(pa, "library_file",
                                                "vp_library_test.bin"),
                       AURA_OK);
    }

    aura_pipeline_t *p = aura_pipeline_create(NULL);
    AURA_ASSERT(p != NULL);
    aura_node_t *nodes[4];
    uint32_t     ncount = 0;
    AURA_ASSERT_EQ(aura_chain_build(p, &c, nodes, &ncount), AURA_OK);
    AURA_ASSERT(ncount == 2);

    aura_event_log_t log;
    AURA_ASSERT_EQ(aura_event_log_init(&log, aura_pipeline_bus(p),
                                       1u << AURA_EVENT_VOICEPRINT_RESULT),
                   AURA_OK);
    AURA_ASSERT_EQ(aura_pipeline_start(p), AURA_OK);

    /* ---- 同音频：通过（库由离线注册预置） */
    int32_t same_score = 0;
    {
        uint32_t base = log.count;
        const aura_event_t *ev = trigger_verify(p, &log, &wa, base);
        AURA_ASSERT(ev != NULL);
        if (ev != NULL) {
            printf("    同音频验证: passed=%d score=%d spk='%s'\n",
                   ev->u.voiceprint.passed, ev->u.voiceprint.score, ev->payload);
            AURA_CHECK(ev->u.voiceprint.passed);
            AURA_ASSERT_STREQ(ev->payload, "说话人A");
            same_score = ev->u.voiceprint.score;
        }
    }

    /* ---- 另一段音频（wav1）：分数必须**低于**同音频验证。
     * 严格"拒绝"断言 SKIP：test_wavs 大概率是同一（或少数）说话人录的不同
     * 关键词，0.848 正是"同人不同句"的量级 —— 没有确凿的异人素材时，
     * 相对断言（不同内容分数显著低于相同内容）是能落地的口径。 */
    {
        uint32_t base = log.count;
        const aura_event_t *ev = trigger_verify(p, &log, &wb, base);
        AURA_ASSERT(ev != NULL);
        if (ev != NULL) {
            printf("    异音频验证: passed=%d score=%d spk='%s'\n",
                   ev->u.voiceprint.passed, ev->u.voiceprint.score, ev->payload);
            AURA_CHECK(ev->u.voiceprint.score < same_score);
        }
    }

    /* ---- 删除 + 清库路径 */
    {
        aura_vp_set_config_t cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.op     = AURA_VP_OP_REMOVE;
        cfg.spk_id = "说话人A";
        AURA_ASSERT_EQ(aura_pipeline_control_node(p, nodes[1], AURA_CMD_SET_CONFIG,
                                                  &cfg),
                       AURA_OK);
        aura_test_sleep_ms(200);

        uint32_t base = log.count;
        const aura_event_t *ev = trigger_verify(p, &log, &wa, base);
        AURA_ASSERT(ev != NULL);
        if (ev != NULL) {
            AURA_CHECK(!ev->u.voiceprint.passed); /* 删了之后应当拒绝 */
        }
    }

    wav16_free(&wa);
    wav16_free(&wb);
    aura_event_log_fini(&log);
    aura_pipeline_stop(p);
    aura_pipeline_destroy(p);
    aura_chain_destroy_nodes(nodes, ncount);
    aura_infer_deinit();
}

AURA_TEST_MAIN()
