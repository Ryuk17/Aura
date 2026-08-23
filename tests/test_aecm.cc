// AECM 链节点 + AudioChain 测试：真实 golden wav（TrickRoom 官方测试数据）
// 输入：audio_nearend16k.wav（麦克风）+ audio_farend16k.wav（播放参考）
// 验收：
//   1) 行为一致性：输出与官方参考 audio_nearend16k_aecm_out.wav 分段能量一致（±10%）
//   2) 消除效果：收敛段（前 19s）输出能量 / 输入能量 < 0.2
// 数据目录通过 CMake 宏 AURA_TEST_DATA_DIR 注入（resources/audio_engine/data）
#include "test_framework.h"

#include "aecm_node.h"
#include "audio_chain.h"
#include "audio_focus.h"
#include "wav.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace aura::test {

namespace {

constexpr int kSampleRate = 16000;
constexpr int kFrameSize = 160;   // 10ms
constexpr int kSegFrames = 100;   // 分段：1s

std::string DataPath(const char* name) {
    return std::string(AURA_TEST_DATA_DIR) + "/" + name;
}

// 逐帧跑 AecmNode（每帧先喂当前播放帧 farend），返回输出
void RunAecm(audio::AecmNode* aecm, const std::vector<int16_t>& nearend,
             const std::vector<int16_t>& farend, std::vector<int16_t>* out) {
    const size_t nframes = std::min(nearend.size(), farend.size()) / kFrameSize;
    out->reserve(nframes * kFrameSize);
    for (size_t f = 0; f < nframes; ++f) {
        aecm->FeedFarend(farend.data() + f * kFrameSize, kFrameSize);
        int16_t o[kFrameSize];
        size_t got = aecm->Process(nearend.data() + f * kFrameSize, kFrameSize, o,
                                   kFrameSize);
        CHECK(got == kFrameSize);
        out->insert(out->end(), o, o + got);
    }
}

// 分段（kSegFrames 帧/段）能量；不足一段的尾部丢弃
void SegEnergy(const std::vector<int16_t>& samples, std::vector<double>* seg_e) {
    const size_t nframes = samples.size() / kFrameSize;
    for (size_t f = 0; f + kSegFrames <= nframes; f += kSegFrames) {
        double e = 0;
        for (size_t i = f * kFrameSize; i < (f + kSegFrames) * kFrameSize; ++i) {
            e += static_cast<double>(samples[i]) * samples[i];
        }
        seg_e->push_back(e);
    }
}

}  // namespace

TEST_CASE(aecm_node_matches_reference) {
    // 行为一致性：我们的封装输出 ≈ 官方参考输出（确定性算法，应逐段吻合）
    std::vector<int16_t> nearend, farend, ref;
    aura::WavInfo ni, fi, ri;
    CHECK(aura::ReadWav(DataPath("audio_nearend16k.wav"), &nearend, &ni));
    CHECK(aura::ReadWav(DataPath("audio_farend16k.wav"), &farend, &fi));
    CHECK(aura::ReadWav(DataPath("audio_nearend16k_aecm_out.wav"), &ref, &ri));
    CHECK(ni.sample_rate == kSampleRate);
    CHECK(fi.sample_rate == kSampleRate);

    audio::AecmNode aecm;
    CHECK(aecm.Init(kSampleRate));
    std::vector<int16_t> my_out;
    RunAecm(&aecm, nearend, farend, &my_out);

    std::vector<double> my_seg, ref_seg;
    SegEnergy(my_out, &my_seg);
    SegEnergy(ref, &ref_seg);
    CHECK(my_seg.size() == ref_seg.size());

    const size_t nsegs = std::min(my_seg.size(), ref_seg.size());
    // 跳过静音段（能量低于峰值 1e-4）与未对齐尾部
    double peak = 0;
    for (double e : ref_seg) peak = std::max(peak, e);
    int compared = 0, bad = 0;
    for (size_t i = 0; i + 1 < nsegs; ++i) {  // 丢弃最末段（边界不齐）
        if (ref_seg[i] < peak * 1e-4) continue;
        ++compared;
        double ratio = my_seg[i] / ref_seg[i];
        if (ratio < 0.9 || ratio > 1.11) {
            ++bad;
            if (bad <= 3) {
                std::printf("  seg %zu ratio=%.3f (my=%.3e ref=%.3e)\n", i, ratio,
                            my_seg[i], ref_seg[i]);
            }
        }
    }
    CHECK(compared >= 10);
    CHECK(bad <= std::max(1, compared / 20));  // 允许 ≤5% 分段偏离
}

TEST_CASE(aecm_node_reduces_echo) {
    // 消除效果：收敛段 = seg 1~18（跳过 seg 0 的 startup 段——参考输出显示
    // 该段滤波器收敛初期放大 3.6 倍，为 AECM 对这份数据的真实行为）
    // 参考数据 seg 1~18 输出/输入 ≈ 0.011，验收阈值 < 0.05
    std::vector<int16_t> nearend, farend;
    aura::WavInfo ni, fi;
    CHECK(aura::ReadWav(DataPath("audio_nearend16k.wav"), &nearend, &ni));
    CHECK(aura::ReadWav(DataPath("audio_farend16k.wav"), &farend, &fi));

    audio::AecmNode aecm;
    CHECK(aecm.Init(kSampleRate));
    std::vector<int16_t> my_out;
    RunAecm(&aecm, nearend, farend, &my_out);

    const size_t start = 100 * kFrameSize;    // 样本：seg 1 起点（跳过 seg 0 startup）
    const size_t end = 1900 * kFrameSize;     // 样本：seg 19 起点（seg 19 起参考数据发散）
    double in_e = 0, out_e = 0;
    for (size_t i = start; i < end; ++i) {
        in_e += static_cast<double>(nearend[i]) * nearend[i];
        out_e += static_cast<double>(my_out[i]) * my_out[i];
    }
    double ratio = out_e / in_e;
    if (ratio >= 0.05) {
        std::printf("  AECM converge-seg ratio = %f (target < 0.05), in=%.3e out=%.3e\n",
                    ratio, in_e, out_e);
    }
    CHECK(ratio < 0.05);
}

TEST_CASE(aecm_node_pass_through_without_farend) {
    // 无播放信号时：近端信号应基本保留（AEC 不清除近端）
    // 用真实 nearend 前 1s（无 farend 输入）
    std::vector<int16_t> nearend;
    aura::WavInfo ni;
    CHECK(aura::ReadWav(DataPath("audio_nearend16k.wav"), &nearend, &ni));

    audio::AecmNode aecm;
    CHECK(aecm.Init(kSampleRate));

    std::vector<int16_t> out;
    const size_t nframes = std::min<size_t>(100, nearend.size() / kFrameSize);
    for (size_t f = 0; f < nframes; ++f) {
        int16_t o[kFrameSize];
        size_t got = aecm.Process(nearend.data() + f * kFrameSize, kFrameSize, o,
                                  kFrameSize);
        CHECK(got == kFrameSize);
        out.insert(out.end(), o, o + got);
    }
    double in_e = 0, out_e = 0;
    for (size_t i = 0; i < nframes * kFrameSize; ++i) {
        in_e += static_cast<double>(nearend[i]) * nearend[i];
        out_e += static_cast<double>(out[i]) * out[i];
    }
    // 保留 50% 以上（AECM 启动期 passthrough + 后续处理，不该被清掉）
    CHECK(out_e / in_e > 0.5);
}

TEST_CASE(audio_chain_passthrough_when_empty) {
    // 空链：直通
    audio::AudioChain chain;
    CHECK(chain.Init(kSampleRate));

    int16_t in[kFrameSize], out[kFrameSize];
    for (int i = 0; i < kFrameSize; ++i) in[i] = static_cast<int16_t>(i * 100 - 8000);
    size_t got = chain.Process(in, kFrameSize, out, kFrameSize);
    CHECK(got == kFrameSize);
    bool same = true;
    for (int i = 0; i < kFrameSize; ++i) same = same && (in[i] == out[i]);
    CHECK(same);
}

TEST_CASE(audio_chain_aecm_matches_reference) {
    // 整链（AudioChain 装配 AecmNode）输出 ≈ 官方参考
    std::vector<int16_t> nearend, farend, ref;
    aura::WavInfo ni, fi, ri;
    CHECK(aura::ReadWav(DataPath("audio_nearend16k.wav"), &nearend, &ni));
    CHECK(aura::ReadWav(DataPath("audio_farend16k.wav"), &farend, &fi));
    CHECK(aura::ReadWav(DataPath("audio_nearend16k_aecm_out.wav"), &ref, &ri));

    audio::AudioChain chain;
    auto aecm = std::make_unique<audio::AecmNode>();
    CHECK(aecm->Init(kSampleRate));
    audio::AecmNode* aecm_ptr = aecm.get();
    chain.AddNode(std::move(aecm));
    CHECK(chain.Init(kSampleRate));
    CHECK(chain.Find("aecm") == aecm_ptr);

    std::vector<int16_t> my_out;
    const size_t nframes = std::min(nearend.size(), farend.size()) / kFrameSize;
    my_out.reserve(nframes * kFrameSize);
    for (size_t f = 0; f < nframes; ++f) {
        aecm_ptr->FeedFarend(farend.data() + f * kFrameSize, kFrameSize);
        int16_t o[kFrameSize];
        size_t got = chain.Process(nearend.data() + f * kFrameSize, kFrameSize, o,
                                   kFrameSize);
        CHECK(got == kFrameSize);
        my_out.insert(my_out.end(), o, o + got);
    }

    std::vector<double> my_seg, ref_seg;
    SegEnergy(my_out, &my_seg);
    SegEnergy(ref, &ref_seg);
    CHECK(my_seg.size() == ref_seg.size());

    const size_t nsegs = std::min(my_seg.size(), ref_seg.size());
    double peak = 0;
    for (double e : ref_seg) peak = std::max(peak, e);
    int compared = 0, bad = 0;
    for (size_t i = 0; i + 1 < nsegs; ++i) {
        if (ref_seg[i] < peak * 1e-4) continue;
        ++compared;
        double ratio = my_seg[i] / ref_seg[i];
        if (ratio < 0.9 || ratio > 1.11) ++bad;
    }
    CHECK(compared >= 10);
    CHECK(bad <= std::max(1, compared / 20));
}

TEST_CASE(audio_focus_state_machine) {
    audio::AudioFocus focus;
    CHECK(focus.state() == audio::AudioFocus::State::kIdle);
    CHECK(focus.BeginPlayback());
    CHECK(focus.state() == audio::AudioFocus::State::kSpeaking);
    // 重入拒绝
    CHECK(!focus.BeginPlayback());
    focus.EndPlayback();
    CHECK(focus.state() == audio::AudioFocus::State::kCapturing);
    // barge-in：播放态 → 采集态
    CHECK(focus.BeginPlayback());
    focus.OnBargeIn();
    CHECK(focus.state() == audio::AudioFocus::State::kCapturing);
}

}  // namespace aura::test
