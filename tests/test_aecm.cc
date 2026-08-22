// AECM 回声消除测试：合成信号验证收敛后回声残余显著降低
// 场景：farend = 1kHz 正弦（模拟扬声器播放），nearend = farend（纯回声，无近端语音）
// 验收：后 500ms 输出能量 / 输入能量 < 5%（对应 ERLE > 13dB，保守阈值）
#include "test_framework.h"

#include "aecm_processor.h"

#include <cmath>
#include <cstdint>
#include <vector>

namespace aura::test {

namespace {

// 帧能量（dB 不直接用，返回平方和）
double Energy(const std::vector<int16_t>& samples) {
    double sum = 0;
    for (int16_t s : samples) sum += static_cast<double>(s) * s;
    return sum;
}

}  // namespace

TEST_CASE(aecm_reduces_echo) {
    constexpr int kSampleRate = 16000;
    constexpr int kFrameSize = 160;                 // 10ms
    constexpr int kTotalFrames = 300;               // 3s
    constexpr double kFreq = 1000.0;                // 1kHz
    constexpr double kEchoGain = 0.5;               // 回声幅度 0.5
    constexpr int kEchoDelaySamples = 320;          // 20ms 声学延迟（扬声器→麦克风）

    AecmProcessor aecm;
    CHECK(aecm.Init(kSampleRate));
    CHECK(aecm.frame_size() == kFrameSize);

    std::vector<int16_t> farend;
    std::vector<int16_t> nearend;
    std::vector<int16_t> out;
    farend.reserve(kTotalFrames * kFrameSize);
    nearend.reserve(kTotalFrames * kFrameSize);
    out.reserve(kTotalFrames * kFrameSize);

    // 播放信号历史（模拟声学延迟：近端 = 20ms 前播放的 farend 的回声）
    std::vector<int16_t> farend_history(kEchoDelaySamples + kFrameSize, 0);

    for (int frame = 0; frame < kTotalFrames; ++frame) {
        int16_t f[kFrameSize], n[kFrameSize], o[kFrameSize];
        for (int i = 0; i < kFrameSize; ++i) {
            int idx = frame * kFrameSize + i;
            // 当前播放的 farend 持续正弦
            int16_t v = static_cast<int16_t>(
                std::sin(2 * 3.14159265358979 * kFreq * idx / kSampleRate) * 8000);
            f[i] = v;
            // 近端 = 20ms 前播放信号的回声（幅度减半）
            int16_t echo = farend_history[i];
            n[i] = static_cast<int16_t>(echo * kEchoGain);
        }
        // 历史滚动：丢弃最旧一帧，追加当前帧
        std::copy(farend_history.begin() + kFrameSize, farend_history.end(),
                  farend_history.begin());
        std::copy(f, f + kFrameSize, farend_history.end() - kFrameSize);

        // 无播放卡硬件缓冲：ms_in_sndcard_buf = 0（声学延迟由 AECM 内部估计）
        aecm.ProcessFrame(f, n, o, 0);
        farend.insert(farend.end(), f, f + kFrameSize);
        nearend.insert(nearend.end(), n, n + kFrameSize);
        out.insert(out.end(), o, o + kFrameSize);
    }

    // 只统计后 500ms（AECM 自适应收敛期约 1-2s）
    const size_t tail_start = kTotalFrames - 50;  // 后 50 帧
    std::vector<int16_t> in_tail(nearend.begin() + tail_start * kFrameSize, nearend.end());
    std::vector<int16_t> out_tail(out.begin() + tail_start * kFrameSize, out.end());

    double in_energy = Energy(in_tail);
    double out_energy = Energy(out_tail);
    double ratio = out_energy / in_energy;

    CHECK(in_energy > 0);
    if (ratio >= 0.05) {
        std::printf("  AECM residual ratio = %f (target < 0.05), in=%.2f out=%.2f\n",
                    ratio, in_energy, out_energy);
    }
    CHECK(ratio < 0.05);
}

TEST_CASE(aecm_pass_through_without_farend) {
    // 无播放信号时：近端语音应基本保留（AEC 不清除近端）
    AecmProcessor aecm;
    CHECK(aecm.Init(16000));

    std::vector<int16_t> nearend;
    std::vector<int16_t> out;
    for (int frame = 0; frame < 100; ++frame) {
        int16_t n[160], o[160];
        for (int i = 0; i < 160; ++i) {
            // 300Hz 近端语音（远离回声测试频率）
            n[i] = static_cast<int16_t>(
                std::sin(2 * 3.14159265358979 * 300.0 * (frame * 160 + i) / 16000) * 4000);
        }
        aecm.ProcessFrame(nullptr, n, o);  // farend = 无
        nearend.insert(nearend.end(), n, n + 160);
        out.insert(out.end(), o, o + 160);
    }
    double in_e = Energy(nearend);
    double out_e = Energy(out);
    // 近端语音保留 50% 以上（CNG 与处理会有衰减，但不该被清掉）
    CHECK(out_e / in_e > 0.5);
}

}  // namespace aura::test
