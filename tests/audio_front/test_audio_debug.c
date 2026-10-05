/* Aura — Phase 2 语音前端指标测试（合成回声场景）
 *
 * 干什么：在进程内装配**真链** aec3,bf,ns,agc,src（16k / 10ms / 2 路麦），喂一段
 * 回声路径已知的合成场景，对每个节点的输出断言客观指标，并把各节点 PCM 落盘到
 * ${CMAKE_BINARY_DIR}/audio_front/ 供人工听评。链路装配与喂帧见 front_rig.h。
 *
 * 为什么是断言而不是"打印出来看看"：ERLE 这类指标**会静默退化** —— 参考错位、
 * SRC 没生效、BF 把信号压没了，任何一种都不报错、不崩，只是数字变小。工具里给人
 * 看数（tools/audio_debug 打印同一张表），测试里给机器判对错，两边跑在同一条链、
 * 同一套落盘与统计上（见 tools/CMakeLists.txt 的说明）。
 *
 * 阈值为什么这么定 —— 每个取值都要能追到一个物理量，不能是"上次测出来是这个数"：
 *   ERLE ≥ 10 dB        合成回声是线性的（无非线性失真、无背景噪声），AEC3 收敛后
 *                       残余应远低于此；10 dB 是"确实在消"的下限，不是收敛目标。
 *                       实测 43 dB。偏低先听 aec3_out.wav，别急着降阈值。
 *   近端保真 ∈ [-6,+3]  近端单讲段（此时参考是静音）：AEC 不该动近端。上界 +3 留给
 *                       收敛过程中的轻微增益波动；下界 -6 是"没被当回声吃掉"。
 *   BF 保形 ∈ [1.5,8]   两路麦在本场景**同相**（声源在宽侧向，程差为 0，见
 *                       synth_scene.c），相干求和 → 幅度 ×2 → 能量 ×4，理论值 4.0。
 *                       实测 4.41。区间只判"没被压死也没炸开"；指向性在这个场景里
 *                       验不了（没有程差），别在这里找。
 *   AGC 无削波           峰值 < 0.98×32767，且越限样本数为 0。爆音是 AGC 最典型的
 *                       坏法，且它**不会**让任何统计报错。
 *   AGC/NS ∈ [0.1,8]    AGC 是增益级：上限取它的压缩增益上限（compression_gain_db=9
 *                       → 约 8 倍能量），实测 2.89；下限判"没把信号压没"。
 *   SRC                 末帧 8k / 单通道 / 每帧 80 样本，总样本数比 ≈ 0.5。这类"形状"
 *                       错误（out_sample_rate 没生效）会让下游拿到错误时基的数据，
 *                       而链上没有任何一环会报错。
 *   参考健康              ref_missed=0（没走静音顶替）、ref_stale=0（配对没错位）、
 *                       ref_lead_max ≤ 一帧（参考没跑到近端前面去）。这是**回归
 *                       断言**：参考配对从"FIFO 撞"改成"按 pts 配对"之前，这里量到
 *                       过 5120 样本（320ms）的固定领先，而当时其他统计全都正常。
 */
#include <stdio.h>

#include "unit/aura_test.h" /* 框架在 tests/unit/ 下，按路径引（见 tests/CMakeLists.txt） */

#include "front_rig.h"
#include "metrics.h"
#include "synth_scene.h"

#ifndef AURA_TEST_SOURCE_DIR
#define AURA_TEST_SOURCE_DIR "."
#endif
#ifndef AURA_TEST_OUT_DIR
#define AURA_TEST_OUT_DIR "audio_front"
#endif

/* 与 tools/audio_debug --gen-scene 同一个种子：工具里听的和测试里判的必须是逐样本
 * 相同的一段音频，否则"工具里听着没问题"就解释不了测试的红。 */
#define TEST_SCENE_SEED 20261003u

static void test_synth_frontend_metrics(void)
{
    char conf[512];
    snprintf(conf, sizeof(conf), "%s/tests/audio_front/configs/frontend_synth.conf",
             AURA_TEST_SOURCE_DIR);

    synth_scene_t sc;
    AURA_ASSERT_EQ(synth_scene_build(&sc, TEST_SCENE_SEED), AURA_OK);

    const uint32_t fc = SYNTH_RATE * SYNTH_FRAME_MS / 1000u;
    front_rig_t    rig;
    const aura_err_t rc = front_rig_run(&rig, conf, AURA_TEST_OUT_DIR, sc.mic, sc.channels, sc.ref,
                                        sc.channels, sc.frames / fc, fc);
    AURA_ASSERT_EQ(rc, AURA_OK);
    if (rig.pipe == NULL) {
        synth_scene_free(&sc);
        return; /* 链都没跑起来，后面的指标没有意义 */
    }

    /* 探针统计是后面所有指标的前提：缺一个就整段跳过，别拿着未初始化的 stats 往下算
     * —— 那会把"探针没挂上"变成一堆看不懂的 NaN 断言失败。 */
    wavtap_stats_t st_aec, st_bf, st_ns, st_agc, st_src;
    const bool have_all = front_tap_stats(&rig, "aec3", &st_aec) &&
                          front_tap_stats(&rig, "bf", &st_bf) &&
                          front_tap_stats(&rig, "ns", &st_ns) &&
                          front_tap_stats(&rig, "agc", &st_agc) &&
                          front_tap_stats(&rig, "src", &st_src);
    AURA_ASSERT(have_all);
    if (!have_all) {
        front_rig_done(&rig);
        synth_scene_free(&sc);
        return;
    }

    /* ---- 指标计算（与 audio_debug.c 的 report_synth_metrics 同口径）----
     * 输入侧能量直接取喂进去的那份场景（不经磁盘）：指标要回答的是"算法做了什么"，
     * 从 WAV 读回来再算就把落盘环节也混进因果链里了。
     * 窗口下标能直接对，是因为 aec3/bf/ns/agc 都是 16k 定长帧、一进一出；SRC 的输出
     * 在另一条时间轴上（8k），**不参与**这两个窗口。 */
    const double e_mic_echo = aura_metrics_energy(sc.mic, sc.frames, sc.channels,
                                                  SYNTH_FRAME_ERLE_0 * fc, SYNTH_FRAME_ERLE_1 * fc);
    const double e_mic_near = aura_metrics_energy(sc.mic, sc.frames, sc.channels,
                                                  SYNTH_FRAME_NEARFID_0 * fc,
                                                  SYNTH_FRAME_NEARFID_1 * fc);
    const double e_aec_echo =
        aura_metrics_frames_mean(st_aec.frame_energy, st_aec.energy_frames, SYNTH_FRAME_ERLE_0,
                                 SYNTH_FRAME_ERLE_1);
    const double e_aec_near =
        aura_metrics_frames_mean(st_aec.frame_energy, st_aec.energy_frames, SYNTH_FRAME_NEARFID_0,
                                 SYNTH_FRAME_NEARFID_1);
    const double e_bf_near =
        aura_metrics_frames_mean(st_bf.frame_energy, st_bf.energy_frames, SYNTH_FRAME_NEARFID_0,
                                 SYNTH_FRAME_NEARFID_1);
    const double e_ns_all =
        aura_metrics_frames_mean(st_ns.frame_energy, st_ns.energy_frames, 0, st_ns.energy_frames);
    const double e_agc_all =
        aura_metrics_frames_mean(st_agc.frame_energy, st_agc.energy_frames, 0, st_agc.energy_frames);

    const double erle     = aura_metrics_db(e_mic_echo, e_aec_echo);
    const double near_fid = aura_metrics_db(e_aec_near, e_mic_near);
    const double bf_keep  = aura_metrics_ratio(e_bf_near, e_mic_near);
    const double agc_gn   = aura_metrics_ratio(e_agc_all, e_ns_all);
    const double src_rat  =
        aura_metrics_ratio((double)st_src.samples_total, (double)st_agc.samples_total);

    printf("  ERLE=%.1f dB（窗口[%u,%u)）  近端保真=%.2f dB（窗口[%u,%u)）\n", erle,
           SYNTH_FRAME_ERLE_0, SYNTH_FRAME_ERLE_1, near_fid, SYNTH_FRAME_NEARFID_0,
           SYNTH_FRAME_NEARFID_1);
    printf("  BF 保形=%.3f  AGC/NS=%.3f  AGC 峰值=%d 削波=%llu\n", bf_keep, agc_gn, st_agc.peak,
           (unsigned long long)st_agc.clipped);
    printf("  SRC 末帧 %uch/%uHz/%u 样本，样本数比=%.4f\n", st_src.last_channels, st_src.last_rate,
           st_src.last_frame_count, src_rat);
    printf("  落盘目录 %s\n", AURA_TEST_OUT_DIR);

    /* ---- 断言 ---- */
    AURA_ASSERT(erle >= 10.0);
    AURA_ASSERT(near_fid >= -6.0 && near_fid <= 3.0);
    AURA_ASSERT(bf_keep >= 1.5 && bf_keep <= 8.0);
    AURA_ASSERT(st_agc.peak < 32111); /* 0.98 × 32767 */
    AURA_ASSERT_EQ(st_agc.clipped, 0);
    AURA_ASSERT(agc_gn >= 0.1 && agc_gn <= 8.0);
    /* SRC：形状三件套 + 样本数比。8000 是 conf 里 chain_param_src.out_sample_rate 声明的，
     * 不是"跑到哪算哪"。 */
    AURA_ASSERT_EQ(st_src.last_rate, 8000);
    AURA_ASSERT_EQ(st_src.last_channels, 1);
    AURA_ASSERT_EQ(st_src.last_frame_count, 80);
    AURA_ASSERT(src_rat >= 0.48 && src_rat <= 0.52);

    /* ---- 健康度 ---- */
    AURA_ASSERT_EQ(rig.ps.node_errors, 0);
    AURA_ASSERT_EQ(rig.ps.frames_dropped, 0);
    AURA_ASSERT_EQ(rig.ps.ref_stale, 0);

    const char *algos[] = { "aec3", "bf", "ns", "agc", "src" };
    for (uint32_t i = 0; i < 5; i++) {
        aura_dsp_stats_t ds;
        if (front_node_stats(&rig, algos[i], &ds)) {
            AURA_ASSERT_EQ(ds.process_errors, 0);
            AURA_ASSERT(ds.frames_in > 0);
            AURA_ASSERT(ds.frames_out > 0);
        } else {
            AURA_ASSERT(false); /* 节点不在链上：上面的 tap 断言已经先响了 */
        }
    }

    /* 参考健康：这三条是"配对错位"这类静默故障的回归断言（见文件头）。 */
    aura_dsp_stats_t ds_aec;
    if (front_node_stats(&rig, "aec3", &ds_aec)) {
        AURA_ASSERT_EQ(ds_aec.ref_missed, 0);
        AURA_ASSERT_EQ(ds_aec.ref_stale, 0);
        AURA_ASSERT(ds_aec.ref_lead_max <= (uint64_t)fc * SYNTH_CHANNELS);
    }

    /* 终点 tap 收到整段，且每个 tap 都真的落了盘（不是"跑完了但文件是空的"）。 */
    aura_node_t   *out_node = aura_probe_find(rig.nodes, rig.ncount, "out");
    wavtap_stats_t out_st;
    AURA_ASSERT(out_node != NULL && wavtap_get_stats(out_node, &out_st) == AURA_OK);
    if (out_node != NULL) {
        AURA_ASSERT_EQ(out_st.frames_in, rig.frames_fed);
        AURA_ASSERT_EQ(out_st.write_errors, 0);
    }
    for (uint32_t i = 0; i < 5; i++) {
        wavtap_stats_t st;
        if (front_tap_stats(&rig, algos[i], &st)) {
            AURA_ASSERT_EQ(st.frames_written, st.frames_in);
        }
    }

    front_rig_done(&rig);
    synth_scene_free(&sc);
}

AURA_TEST(test_synth_frontend_metrics);

AURA_TEST_MAIN()
