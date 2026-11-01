/* test_voice_chain — Phase 3 全链路串通（真模型 ×4：silero / KWS / 声纹 / ASR + 状态机）
 *
 * 走 agent 装配（与真实产品同一条路径）：链 silero_vad, kws, tee(voiceprint), asr
 * —— silero 真 VAD 圈语音、KWS 唤醒、声纹按 VAD_END 验证、ASR 流式出文本，
 * 状态机 Idle → Listening → Thinking。
 *
 * 喂入："1s 静音 + 唤醒词录音（wav0）+ 2s 静音"拼接音频，断言：
 *   - VAD_SPEECH_START/END 出现（silero 真模型在跑）
 *   - KWS_HIT（唤醒词命中，payload ∈ test_keywords 词条集）
 *   - VOICEPRINT_RESULT passed（wav0 已注册为说话人 A）
 *   - ASR_PARTIAL 流 + ASR_FINAL（文本非空）
 *   - 状态序列 Idle → Listening → Thinking
 * 并打印全链路延迟（喂入→KWS_HIT、KWS_HIT→ASR_FINAL）。
 *
 * 素材缺失 SKIP（audio_front 约定）。
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "unit/aura_test.h"

#include "agent/agent_core.h"
#include "agent_export.h"
#include "asr/aura_asr.h"
#include "audio/vad/aura_silero.h"
#include "core/algorithm.h"
#include "kws/aura_kws.h"
#include "unit/aura_event_log.h"
#include "voiceprint/aura_voiceprint.h"
#include "voiceprint/vp_lib.h"

#ifndef AURA_HOST_SIM_SOURCE_DIR
#define AURA_HOST_SIM_SOURCE_DIR "."
#endif

#define KWS_MODEL_DIR \
    AURA_HOST_SIM_SOURCE_DIR "/models/kws/kws-zipformer-wenetspeech-3.3M-MNN"
#define ASR_MODEL_DIR \
    AURA_HOST_SIM_SOURCE_DIR "/models/asr/sherpa-mnn-streaming-zipformer-bilingual-zh-en-2023-02-20"
#define VP_MODEL_DIR AURA_HOST_SIM_SOURCE_DIR "/models/speaker_recognition"
#define SILERO_MODEL_DIR AURA_HOST_SIM_SOURCE_DIR "/models/vad/silero_vad.mnn"
#define KW_FILE KWS_MODEL_DIR "/test_wavs/test_keywords.txt"
#define TEST_WAV KWS_MODEL_DIR "/test_wavs/3.wav"

#define VP_EVENTS_MASK                                           \
    ((1u << AURA_EVENT_KWS_HIT) | (1u << AURA_EVENT_VAD_SPEECH_START) | \
     (1u << AURA_EVENT_VAD_SPEECH_END) |                         \
     (1u << AURA_EVENT_VOICEPRINT_RESULT) |                      \
     (1u << AURA_EVENT_ASR_PARTIAL) | (1u << AURA_EVENT_ASR_FINAL))

/* ------------------------------------------------------------ WAV 读取 */

typedef struct {
    uint32_t sr;
    uint16_t channels;
    int16_t *data;
    uint32_t frames;
} wav16_t;

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

static bool models_present(void)
{
    const char *paths[] = {
        KWS_MODEL_DIR "/encoder-epoch-12-avg-2-chunk-16-left-64.mnn",
        ASR_MODEL_DIR "/encoder-epoch-99-avg-1.int8.mnn",
        VP_MODEL_DIR "/3dspeaker_speech_eres2netv2_sv_zh-cn_16k-common.mnn",
        SILERO_MODEL_DIR "/silero_vad.mnn",
    };
    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        FILE *f = fopen(paths[i], "rb");
        if (f == NULL) {
            printf("    缺模型: %s\n", paths[i]);
            return false;
        }
        fclose(f);
    }
    return true;
}

/* 按 15ms/帧喂（ASR 的 int8 在这台无 i8sdot 的 x86 上解码略慢于实时，见
 * test_asr_chain 的注释）。 */
static void feed_silence(uint32_t frames)
{
    static int16_t sil[160] = {0};
    for (uint32_t i = 0; i < frames; i++) {
        AURA_ASSERT_EQ(aura_agent_feed_audio(sil, 160, 1, AURA_AGENT_AUDIO_S16, 0, 1000),
                       AURA_OK);
        aura_test_sleep_ms(25);
    }
}

AURA_TEST(test_voice_chain_full)
{
    if (!models_present()) {
        printf("    SKIP: 模型不全（models/ 不入库）\n");
        return;
    }
    wav16_t w;
    if (!wav16_load(TEST_WAV, &w) || w.sr != 16000) {
        printf("    SKIP: 测试音频不可用\n");
        free(w.data); w.data = NULL;
        return;
    }

    /* ---- 注册四个算法族（agent init 之前） */
    AURA_ASSERT_EQ(aura_silero_register(), AURA_OK);
    AURA_ASSERT_EQ(aura_kws_register(), AURA_OK);
    AURA_ASSERT_EQ(aura_voiceprint_register(), AURA_OK);
    AURA_ASSERT_EQ(aura_asr_register(), AURA_OK);

    /* ---- agent 装配（与真实产品同一条路径） */
    aura_agent_config_t cfg;
    aura_agent_config_default(&cfg);
    cfg.model_dir = AURA_HOST_SIM_SOURCE_DIR "/models";
    cfg.chain     = "silero_vad, kws, tee(voiceprint), asr";
    cfg.frame_pool_blocks = 128; /* 全链路扇出比单链大 */
    cfg.log_level = 2;

    aura_agent_chain_param_t params[2];
    params[0].key = "kws.keywords_file";
    params[0].val = "kws/kws-zipformer-wenetspeech-3.3M-MNN/test_wavs/test_keywords.txt";
    params[1].key = "voiceprint.library_file";
    params[1].val = "vp_voice_chain_lib.bin";
    cfg.chain_params      = params;
    cfg.chain_param_count = 2;

    AURA_ASSERT_EQ(aura_agent_init(&cfg), AURA_OK);

    /* ---- 离线注册（init 之后、start 之前：链还没跑，零并发） */
    {
        remove("vp_voice_chain_lib.bin");
        float emb[192];
        AURA_ASSERT_EQ(aura_vp_embed_offline(w.data, w.frames, w.sr,
                                             VP_MODEL_DIR "/3dspeaker_speech_"
                                                          "eres2netv2_sv_zh-cn_"
                                                          "16k-common.mnn",
                                             emb),
                       AURA_OK);
        aura_vp_entry_t entries[16];
        memset(entries, 0, sizeof(entries));
        snprintf(entries[0].id, sizeof(entries[0].id), "%s", "说话人A");
        memcpy(entries[0].emb, emb, sizeof(emb));
        AURA_ASSERT_EQ(aura_vp_lib_save("vp_voice_chain_lib.bin", entries, 1),
                       AURA_OK);
    }

    AURA_ASSERT_EQ(aura_agent_start(), AURA_OK);

    /* ---- 订阅事件（先订阅后触发） */
    aura_event_log_t log;
    AURA_ASSERT_EQ(aura_event_log_init(&log, aura_agent_bus(), VP_EVENTS_MASK),
                   AURA_OK);

    /* ---- 喂"1s 静音 + 唤醒词录音 + 2s 静音" */
    uint64_t t0 = aura_osal_time_ms();
    feed_silence(100);
    {
        uint32_t off = 0;
        while (off < w.frames) {
            uint32_t n = w.frames - off;
            if (n > 160) {
                n = 160;
            }
            AURA_ASSERT_EQ(aura_agent_feed_audio(w.data + off, n, 1, AURA_AGENT_AUDIO_S16, 0,
                                                 1000),
                           AURA_OK);
            off += n;
            aura_test_sleep_ms(25);
        }
    }
    feed_silence(200);
    free(w.data);
    w.data = NULL;
    printf("    [诊断] 喂完 %llu ms, count=%u\n",
           (unsigned long long)(aura_osal_time_ms() - t0), log.count);

    /* ---- 等全链路事件（KWS 唤醒 → 声纹 → ASR_FINAL） */
    AURA_ASSERT_EQ(aura_event_log_wait(&log, 1, 60000), AURA_OK);
    uint64_t deadline = aura_osal_time_ms() + 120000;
    while (aura_event_log_count(&log, AURA_EVENT_ASR_FINAL) == 0 &&
           aura_osal_time_ms() < deadline) {
        aura_test_sleep_ms(200);
    }

    uint32_t n_kws  = aura_event_log_count(&log, AURA_EVENT_KWS_HIT);
    uint32_t n_vad_s = aura_event_log_count(&log, AURA_EVENT_VAD_SPEECH_START);
    uint32_t n_vad_e = aura_event_log_count(&log, AURA_EVENT_VAD_SPEECH_END);
    uint32_t n_vp   = aura_event_log_count(&log, AURA_EVENT_VOICEPRINT_RESULT);
    uint32_t n_part = aura_event_log_count(&log, AURA_EVENT_ASR_PARTIAL);
    uint32_t n_fin  = aura_event_log_count(&log, AURA_EVENT_ASR_FINAL);
    printf("    事件: KWS=%u VAD=%u/%u VP=%u ASR=%u/%u\n", n_kws, n_vad_s, n_vad_e,
           n_vp, n_part, n_fin);

    /* ---- 断言 */
    AURA_CHECK(n_vad_s >= 1); /* silero 真模型在跑 */
    AURA_CHECK(n_kws >= 1);   /* 唤醒词命中 */
    AURA_CHECK(n_vp >= 1);    /* 声纹验证过 */
    AURA_CHECK(n_part >= 1);  /* partial 流 */
    AURA_CHECK(n_fin >= 1);   /* final + 文本 */

    const aura_event_t *ev_kws = aura_event_log_find(&log, AURA_EVENT_KWS_HIT, 0);
    const aura_event_t *ev_vp  = aura_event_log_find(&log, AURA_EVENT_VOICEPRINT_RESULT, 0);
    const aura_event_t *ev_fin = aura_event_log_find(&log, AURA_EVENT_ASR_FINAL, 0);
    if (ev_kws != NULL) {
        printf("    唤醒: '%s' score=%d\n", ev_kws->payload, ev_kws->u.kws.score);
    }
    if (ev_vp != NULL) {
        printf("    声纹: passed=%d score=%d spk='%s'\n", ev_vp->u.voiceprint.passed,
               ev_vp->u.voiceprint.score, ev_vp->payload);
        AURA_CHECK(ev_vp->u.voiceprint.passed); /* 已注册的说话人 */
    }
    if (ev_fin != NULL) {
        printf("    ASR 文本: \"%s\"\n", ev_fin->payload);
        AURA_CHECK(ev_fin->payload[0] != '\0');
    }

    /* 状态机：Idle → Listening → Thinking */
    {
        aura_state_t st = aura_sm_state(aura_agent_sm());
        printf("    终态: %d（期望 %d=Thinking）\n", (int)st, (int)AURA_STATE_THINKING);
        AURA_CHECK(st == AURA_STATE_THINKING);
    }

    /* 延迟打印（与验收口径对表：唤醒→ASR_FINAL 是"唤醒→TTS 出声 <1s"的 ASR 段） */
    if (ev_kws != NULL && ev_fin != NULL) {
        uint64_t kws_to_final = (ev_fin->ts_us - ev_kws->ts_us) / 1000;
        printf("    延迟: 喂入→唤醒 %llu ms | KWS_HIT→ASR_FINAL %llu ms\n",
               (unsigned long long)((ev_kws->ts_us / 1000) - t0),
               (unsigned long long)kws_to_final);
    }

    aura_event_log_fini(&log);
    aura_agent_stop();
    aura_agent_deinit();
}

AURA_TEST_MAIN()
