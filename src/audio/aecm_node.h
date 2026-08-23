// AECM 链节点：TrickRoom libAE_AECM 统一 C API 封装（16 kHz，10ms = 160 样本/帧）
// farend：播放路径经 FeedFarend() 注入（内部延迟线，超限丢最旧）
#pragma once

#include "chain_node.h"

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace aura::audio {

class AecmNode : public IChainNode {
public:
    AecmNode() = default;
    ~AecmNode() override;

    AecmNode(const AecmNode&) = delete;
    AecmNode& operator=(const AecmNode&) = delete;

    // 初始化（默认 16 kHz；AECM 仅支持 8k/16k）
    bool Init(int sample_rate = 16000);

    // echoMode 0-4（0 最激进，4 最保守），默认 3
    void SetEchoMode(int mode);

    // 播放路径注入 farend（无播放数据时不调用）
    void FeedFarend(const int16_t* data, size_t frames);

    bool initialized() const { return handle_ != nullptr; }

    // IChainNode：仅处理整帧（160 样本 @16k）；非整帧直通
    size_t Process(const int16_t* in, size_t frames, int16_t* out,
                   size_t cap_out) override;
    int in_rate() const override { return 16000; }
    int out_rate() const override { return 16000; }
    const char* name() const override { return "aecm"; }

private:
    // 从 farend 延迟线取一帧（不足返回 false，等效静音 farend）
    bool TakeFarendFrame(int16_t out[160]);

    void* handle_ = nullptr;
    int echo_mode_ = 3;
    size_t frame_size_ = 0;

    std::mutex farend_mutex_;
    std::vector<int16_t> farend_;  // 播放信号延迟线
    static constexpr size_t kFarendMaxSamples = 48000;  // 3s @16k
};

}  // namespace aura::audio
