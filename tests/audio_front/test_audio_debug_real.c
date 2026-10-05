/* Aura — Phase 2 语音前端测试（真实阵列录音）
 *
 * 与 test_audio_debug.c 的分工：那边的合成场景验的是"有参考时确实在消回声"，
 * 这段真实录音**没有播放回采**，AEC 只能走"缺参考 → 静音顶替"的降级路径。
 * 所以这里**不验回声消除**，验的是另外三件在真机上同样会出事的：
 *   ① 真实录音（非理想、非稳态、带真实底噪）喂进这条链，五个算法都不出错；
 *   ② 缺参考的降级路径可预测 —— ref_missed 恰好等于喂入帧数（每一帧都顶替），
 *      不是"大概在顶替"。缺参考时的表现必须是明确的，否则真机上忘了接回采
 *      只会表现为"回声消不掉"，而没人知道是因为没接；
 *   ③ 全链跑完后每个节点都有输出、都落了盘（供人工听评）。
 *
 * 素材（assets/audio/microphone_array/，未入库）不存在时打印 SKIP 并返回成功：
 * 这是"素材缺失"不是"代码坏了"，让它红会训练出"忽略这条测试"的习惯，比不测更糟。
 * 但素材在而断言不成立时，必须红。
 */
#include <stdio.h>

#include "unit/aura_test.h"

#include "front_rig.h"
#include "wav_io.h"

#ifndef AURA_TEST_SOURCE_DIR
#define AURA_TEST_SOURCE_DIR "."
#endif
#ifndef AURA_TEST_OUT_DIR
#define AURA_TEST_OUT_DIR "audio_front_real"
#endif

/* 阵列录音是"每个通道一个文件"的形态，喂进链前要交织成 2 路。 */
#define REAL_CH0 AURA_TEST_SOURCE_DIR "/assets/audio/microphone_array/F02_011C021A_BUS.CH0.wav"
#define REAL_CH1 AURA_TEST_SOURCE_DIR "/assets/audio/microphone_array/F02_011C021A_BUS.CH1.wav"

static bool file_exists(const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (fp == NULL) {
        return false;
    }
    fclose(fp);
    return true;
}

static void test_real_array_recording(void)
{
    if (!file_exists(REAL_CH0) || !file_exists(REAL_CH1)) {
        printf("  SKIP：素材不在（%s / CH1 未下载），跳过真实录音用例\n", REAL_CH0);
        return;
    }

    char conf[512];
    snprintf(conf, sizeof(conf), "%s/tests/audio_front/configs/frontend_real.conf",
             AURA_TEST_SOURCE_DIR);

    wav_data_t mic;
    AURA_ASSERT_EQ(wav_read_interleave2(REAL_CH0, REAL_CH1, &mic), AURA_OK);
    if (mic.samples == NULL) {
        return;
    }
    AURA_ASSERT_EQ(mic.channels, 2);
    AURA_ASSERT_EQ(mic.sample_rate, 16000);

    const uint32_t fc = mic.sample_rate * 10u / 1000u; /* conf 里 frame_ms = 10 */
    front_rig_t    rig;
    /* ref = NULL：这条路径就是真机上"忘了接播放回采"的样子。 */
    const aura_err_t rc = front_rig_run(&rig, conf, AURA_TEST_OUT_DIR, mic.samples, mic.channels,
                                        NULL, 0, mic.frames / fc, fc);
    AURA_ASSERT_EQ(rc, AURA_OK);
    if (rig.pipe == NULL) {
        wav_free(&mic);
        return;
    }

    /* ① 五个算法都真的处理过数据、都没报错。真实录音比合成场景"脏"得多，
     * 这里是唯一会喂进非理想输入的地方。 */
    const char *algos[] = { "aec3", "bf", "ns", "agc", "src" };
    for (uint32_t i = 0; i < 5; i++) {
        aura_dsp_stats_t ds;
        if (front_node_stats(&rig, algos[i], &ds)) {
            printf("  %-4s 帧 in=%llu out=%llu 错误=%llu\n", algos[i],
                   (unsigned long long)ds.frames_in, (unsigned long long)ds.frames_out,
                   (unsigned long long)ds.process_errors);
            AURA_ASSERT_EQ(ds.process_errors, 0);
            AURA_ASSERT(ds.frames_in > 0);
            AURA_ASSERT(ds.frames_out > 0);
        } else {
            AURA_ASSERT(false);
        }
    }
    AURA_ASSERT_EQ(rig.ps.node_errors, 0);
    AURA_ASSERT_EQ(rig.ps.frames_dropped, 0);

    /* ② 缺参考的降级路径：每一帧都该记一次 ref_missed。等价于"参考帧一次没到"，
     * 而不是"部分到了"（部分到 = 参考流在跑但时轴对不上，那是另一类问题）。 */
    aura_dsp_stats_t ds_aec;
    if (front_node_stats(&rig, "aec3", &ds_aec)) {
        printf("  aec3 参考缺失=%llu（喂入 %u 帧）\n", (unsigned long long)ds_aec.ref_missed,
               rig.frames_fed);
        AURA_ASSERT_EQ(ds_aec.ref_missed, (uint64_t)rig.frames_fed);
        AURA_ASSERT_EQ(ds_aec.ref_stale, 0);
    }
    AURA_ASSERT_EQ(rig.ps.ref_in, 0);

    /* ③ 每个节点都有输出并落了盘。 */
    for (uint32_t i = 0; i < 5; i++) {
        wavtap_stats_t st;
        if (front_tap_stats(&rig, algos[i], &st)) {
            AURA_ASSERT(st.frames_in > 0);
            AURA_ASSERT_EQ(st.write_errors, 0);
            AURA_ASSERT_EQ(st.frames_written, st.frames_in);
        } else {
            AURA_ASSERT(false);
        }
    }

    wavtap_stats_t st_src;
    if (front_tap_stats(&rig, "src", &st_src)) {
        printf("  SRC 末帧 %uch/%uHz\n", st_src.last_channels, st_src.last_rate);
        AURA_ASSERT_EQ(st_src.last_rate, 8000);
        AURA_ASSERT_EQ(st_src.last_channels, 1);
    }

    aura_node_t   *out_node = aura_probe_find(rig.nodes, rig.ncount, "out");
    wavtap_stats_t out_st;
    AURA_ASSERT(out_node != NULL && wavtap_get_stats(out_node, &out_st) == AURA_OK);
    if (out_node != NULL) {
        AURA_ASSERT_EQ(out_st.frames_in, rig.frames_fed);
    }
    printf("  落盘目录 %s\n", AURA_TEST_OUT_DIR);

    front_rig_done(&rig);
    wav_free(&mic);
}

AURA_TEST(test_real_array_recording);

AURA_TEST_MAIN()
