/* test_fbank — fbank 前端自检（Phase 3 前置：KWS/ASR/声纹共用）
 *
 * 验四件事，按"谁先坏、坏在哪"排序：
 *   1. NE10 接线（aura_fft）：impulse → 全 1 功率谱；bin 对齐正弦 → 峰落点 +
 *      Parseval 能量守恒 —— 抓"缩放系数错了/复数布局读错/旋转方向反了"这类
 *      "编译通过但特征全错"的接线问题
 *   2. 镜像前缀（snip_edges=false）：第 0 帧的窗覆盖 wave[-120..-1]，负数下标
 *      按 kaldi 规则**镜像**而不是补零。用"只在采样点 0 有值的冲击"来测 ——
 *      真栅格下冲击经镜像活进窗（能量大），假栅格下它压在 w[0]=0 上被吃掉。
 *   3. 帧数口径：流式 160 采样一块地喂 —— 第 1 块建前缀不出帧，此后每块出 1 帧
 *      （= kaldi flush=false 语义：尾部不足一帧丢弃，不补零凑帧）
 *   4. 静音下限：全零输入 → 每维都是 ln(FLT_EPSILON)
 *
 * 为什么测这些而不是"和 kaldi 逐位对齐"：本机没有 kaldi 的二进制可对照，而模型
 * 精度靠的是"特征与训练侧同一个算法"——所以这里锁算法性质（栅格、镜像、下限、
 * 能量守恒），不锁具体数值。
 */
#include <math.h>
#include <stdio.h>

#include <vector>

#include "aura_test.h"
#include "dsp/aura_fft.h"
#include "dsp/aura_feature_pipeline.h"

static const double kPi = 3.14159265358979323846;

/* ------------------------------------------------------------ NE10 接线 */

AURA_TEST(test_fft_impulse)
{
    aura_fft_t *f = aura_fft_create(512);
    AURA_ASSERT(f != NULL);
    AURA_ASSERT_EQ(aura_fft_bins(f), 256);

    /* 冲击 → 所有 bin 的模都是 1（朴素 DFT，无缩放）。直接抓缩放错误。 */
    float in[512] = {0};
    float out[256];
    in[0] = 1.0f;
    AURA_ASSERT_EQ(aura_fft_power(f, in, out), AURA_OK);
    for (int k = 0; k < 256; k++) {
        AURA_CHECK(fabsf(out[k] - 1.0f) < 1e-4f);
    }
    aura_fft_destroy(f);
}

AURA_TEST(test_fft_sine)
{
    aura_fft_t *f = aura_fft_create(512);
    AURA_ASSERT(f != NULL);

    /* 512 点里放 32 个整周期 → 无泄漏，能量全落在 bin 32：峰 (N/2)^2 = 65536 */
    float in[512];
    double sum_x2 = 0.0;
    for (int n = 0; n < 512; n++) {
        in[n] = (float)sin(2.0 * kPi * 32.0 * n / 512.0);
        sum_x2 += (double)in[n] * in[n];
    }
    float out[256];
    AURA_ASSERT_EQ(aura_fft_power(f, in, out), AURA_OK);

    AURA_CHECK(out[32] > 60000.0f);

    double other = 0.0;
    for (int k = 0; k < 256; k++) {
        if (k != 32) other += out[k];
    }
    AURA_CHECK(other < 1.0);

    /* Parseval：实信号半谱（DC 单算、不含 Nyquist）能量 = N/2 · Σx²。
     * 抓"输出的实/虚部读反了"这类问题 —— 读反时峰照样在，但总能量会差很多。 */
    double half = 0.0;
    for (int k = 0; k < 256; k++) {
        half += out[k];
    }
    AURA_CHECK(fabs(half - 256.0 * sum_x2) < 10.0);

    aura_fft_destroy(f);
}

AURA_TEST(test_fft_bad_args)
{
    /* NE10 只收 2 的幂；非幂与非法尺寸必须拒绝而不是段错误。 */
    AURA_CHECK(aura_fft_create(0) == NULL);
    AURA_CHECK(aura_fft_create(400) == NULL);
    AURA_CHECK(aura_fft_create(-512) == NULL);

    aura_fft_t *f = aura_fft_create(512);
    AURA_ASSERT(f != NULL);
    float buf[256];
    AURA_ASSERT_EQ(aura_fft_power(f, NULL, buf), AURA_ERR_INVALID_ARG);
    AURA_ASSERT_EQ(aura_fft_power(NULL, buf, buf), AURA_ERR_INVALID_ARG);
    AURA_ASSERT_EQ(aura_fft_power(f, buf, NULL), AURA_ERR_INVALID_ARG);
    AURA_ASSERT_EQ(aura_fft_bins(NULL), 0);
    aura_fft_destroy(f);
}

/* ------------------------------------------------------------ 特征流水线 */

static float frame_mean(const std::vector<float> &feat)
{
    double s = 0.0;
    for (float v : feat) {
        s += v;
    }
    return (float)(s / feat.size());
}

AURA_TEST(test_fbank_mirror_prefix)
{
    /* 只在采样点 0 放一个 100 的冲击，其余全 0。
     *
     * 真栅格（snip_edges=false）：第 0 帧的窗 = [镜像 120 ++ 前 280]，冲击经镜像
     * 落在窗内权重 ≈0.69 的位置 → 能量大；
     * 假栅格（snip_edges=true）：第 0 帧从采样点 0 开始，冲击压在 w[0]=0 上
     * → 被窗吃掉，只剩直流/预加重的残渣 → 能量小。
     * 两条管线喂同样的输入，比第 0 帧的平均特征即可。 */
    const int N = 800;
    std::vector<float> spike(N, 0.0f);
    spike[0] = 100.0f;

    aura::FeaturePipelineConfig cfg =
        aura::FeaturePipelineConfig::TrainingDefaults(80, 16000);
    aura::FeaturePipeline pipe(cfg);
    pipe.AcceptWaveform(spike.data(), N);

    /* snip_edges=true（WeNet 原味栅格）做对照组 */
    aura::FeaturePipelineConfig cfg_snip(80, 16000);
    aura::FeaturePipeline pipe_snip(cfg_snip);
    pipe_snip.AcceptWaveform(spike.data(), N);

    std::vector<float> feat;
    AURA_CHECK(pipe.ReadOne(&feat));
    float mean_false = frame_mean(feat);
    AURA_CHECK(pipe_snip.ReadOne(&feat));
    float mean_true = frame_mean(feat);

    AURA_CHECK(mean_false > 3.0f);             /* 冲击活进了第 0 帧 */
    AURA_CHECK(mean_true < 3.0f);              /* 对照组里它被窗吃掉了 */
    AURA_CHECK(mean_false > mean_true + 3.0f); /* 两者至少差 e^3 倍能量 */
}

AURA_TEST(test_fbank_frame_count)
{
    /* 流式喂法：每块 160 采样。第 1 块只够建 120 点镜像前缀（280 < 400，不出帧），
     * 之后每块恰好产 1 帧。这个"第 1 块出 0 帧、此后每块 1 帧"的节拍是 KWS/ASR
     * 攒帧调度直接依赖的口径。 */
    aura::FeaturePipelineConfig cfg =
        aura::FeaturePipelineConfig::TrainingDefaults(80, 16000);
    aura::FeaturePipeline pipe(cfg);

    std::vector<float> chunk(160, 0.0f);
    pipe.AcceptWaveform(chunk.data(), (int)chunk.size());
    AURA_ASSERT_EQ(pipe.num_frames(), 0);

    for (int c = 1; c <= 8; c++) {
        pipe.AcceptWaveform(chunk.data(), (int)chunk.size());
        AURA_ASSERT_EQ(pipe.num_frames(), c);
    }

    /* 8 帧都在队列里。ReadOne 在"队列空且输入未结束"时是**阻塞**的（WeNet 语义），
     * 所以先把输入标成结束再排空 —— 排空后 ReadOne 返回 false（尾部不足一帧丢弃）。 */
    AURA_ASSERT_EQ(pipe.NumQueuedFrames(), 8);
    pipe.set_input_finished();
    std::vector<float> feat;
    int got = 0;
    while (pipe.ReadOne(&feat)) {
        AURA_ASSERT_EQ((int)feat.size(), 80);
        got++;
    }
    AURA_ASSERT_EQ(got, 8);
    AURA_CHECK(!pipe.ReadOne(&feat));

    /* 复位：帧数归零，且镜像前缀一起清掉（否则下一句话的栅格整体错位） */
    pipe.Reset();
    AURA_ASSERT_EQ(pipe.num_frames(), 0);
    pipe.AcceptWaveform(chunk.data(), (int)chunk.size());
    AURA_ASSERT_EQ(pipe.num_frames(), 0); /* 又是"第一块"的节拍 */
}

AURA_TEST(test_fbank_silence)
{
    /* 全零输入：功率谱全 0 → mel 能量全 0 → 全部钳到 log_floor。
     * 这同时验了"mel 滤波器没有空 bin"（有零权重 bin 会一直输出 floor，
     * 反过来这里全 floor 说明没有 bin 是漏的……不，全 floor 恰恰说明每一维
     * 都走到了 floor 路径 —— 见下一条正弦测试补正面）。 */
    aura::FeaturePipelineConfig cfg =
        aura::FeaturePipelineConfig::TrainingDefaults(80, 16000);
    aura::FeaturePipeline pipe(cfg);

    std::vector<float> sil(1600, 0.0f);
    pipe.AcceptWaveform(sil.data(), (int)sil.size());
    /* 帧数 = 1 + (120 + 1600 - 400)/160 = 9（镜像前缀也算进有效长度） */
    AURA_ASSERT_EQ(pipe.num_frames(), 9);

    const float kLnEps = logf(1.1920928955078125e-7f); /* ln(FLT_EPSILON) */
    pipe.set_input_finished(); /* 同上：不标结束，排空队列后 ReadOne 会阻塞 */
    std::vector<float> feat;
    while (pipe.ReadOne(&feat)) {
        for (float v : feat) {
            AURA_CHECK(fabsf(v - kLnEps) < 0.01f);
        }
    }
}

AURA_TEST(test_fbank_sine_response)
{
    /* 1000Hz 正弦（FFT bin 32）穿过整条特征管线：能量应集中在 mel 三角滤波器
     * 覆盖它的那一段（1000Hz ≈ mel 中心 28 附近），其余 bin 贴 floor。
     * 这是"静音下限"的反面 —— 证明滤波器真在滤，而不是全 floor。 */
    aura::FeaturePipelineConfig cfg =
        aura::FeaturePipelineConfig::TrainingDefaults(80, 16000);
    aura::FeaturePipeline pipe(cfg);

    std::vector<float> wav(1600);
    for (int n = 0; n < (int)wav.size(); n++) {
        wav[n] = (float)sin(2.0 * kPi * 1000.0 * n / 16000.0);
    }
    pipe.AcceptWaveform(wav.data(), (int)wav.size());
    AURA_ASSERT_EQ(pipe.num_frames(), 9);

    std::vector<float> feat;
    AURA_CHECK(pipe.ReadOne(&feat));
    int    argmax = 0;
    float  best   = feat[0];
    for (int b = 1; b < 80; b++) {
        if (feat[b] > best) {
            best   = feat[b];
            argmax = b;
        }
    }
    AURA_CHECK(best > 5.0f);              /* 正弦的能量确实进来了 */
    AURA_CHECK(argmax >= 24 && argmax <= 34); /* 峰落在 1000Hz 该在的 mel 段 */
}

AURA_TEST_MAIN()
