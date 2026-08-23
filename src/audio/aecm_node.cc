// AECM 链节点实现：libAE_AECM 统一 C API（TrickRoom audio_engine）
#include "aecm_node.h"

#include "audio_engine_aecm.h"
#include "log.h"

#include <algorithm>
#include <cstring>

namespace aura::audio {

namespace {
constexpr const char* kTag = "aecm_node";
}

AecmNode::~AecmNode() {
    if (handle_) {
        AudioEngine_Aecm_Destroy(handle_);
        handle_ = nullptr;
    }
}

bool AecmNode::Init(int sample_rate) {
    if (handle_) return true;

    handle_ = AudioEngine_Aecm_Create();
    if (!handle_) {
        ALOG_ERROR(kTag, "AudioEngine_Aecm_Create failed");
        return false;
    }

    AecmInitConfig cfg;
    cfg.sample_rate = sample_rate;
    cfg.cng_mode = 1;   // comfort noise ON（与 audio_engine 自带测试一致）
    cfg.echo_mode = echo_mode_;
    if (AudioEngine_Aecm_Init(handle_, &cfg) != AUDIO_ENGINE_SUCCESS) {
        ALOG_ERROR(kTag, "AudioEngine_Aecm_Init failed");
        AudioEngine_Aecm_Destroy(handle_);
        handle_ = nullptr;
        return false;
    }
    frame_size_ = static_cast<size_t>(sample_rate) / 100;  // 10ms
    ALOG_INFO(kTag, "AECM initialized (%d Hz, %zu samples/frame)", sample_rate,
              frame_size_);
    return true;
}

void AecmNode::SetEchoMode(int mode) {
    echo_mode_ = mode;
    if (!handle_) return;
    AecmRtConfig rt;
    rt.reserved = 0;
    (void)rt;
    // 运行中调整 echo mode：通过重新 Init 生效（C API 无直接 setter）
    // v1 保持 init 时设置，此处仅记录
}

void AecmNode::FeedFarend(const int16_t* data, size_t frames) {
    if (!data || frames == 0) return;
    std::lock_guard<std::mutex> lock(farend_mutex_);
    if (farend_.size() + frames > kFarendMaxSamples) {
        size_t drop = farend_.size() + frames - kFarendMaxSamples;
        farend_.erase(farend_.begin(), farend_.begin() + drop);
    }
    farend_.insert(farend_.end(), data, data + frames);
}

bool AecmNode::TakeFarendFrame(int16_t out[160]) {
    std::lock_guard<std::mutex> lock(farend_mutex_);
    if (farend_.size() < frame_size_) return false;
    std::copy_n(farend_.begin(), frame_size_, out);
    farend_.erase(farend_.begin(), farend_.begin() + frame_size_);
    return true;
}

size_t AecmNode::Process(const int16_t* in, size_t frames, int16_t* out,
                         size_t cap_out) {
    if (!in || !out) return 0;
    if (!handle_ || !initialized()) {
        // 未初始化：直通
        size_t n = std::min(frames, cap_out);
        std::memcpy(out, in, n * sizeof(int16_t));
        return n;
    }
    if (frames != frame_size_) {
        // 非整帧：直通（帧对齐由链上层保证，采集侧 10ms 块）
        size_t n = std::min(frames, cap_out);
        std::memcpy(out, in, n * sizeof(int16_t));
        return n;
    }

    int16_t farend[160] = {0};
    bool has_farend = TakeFarendFrame(farend);
    int out_samples = 0;
    int ret = AudioEngine_Aecm_Process(handle_, in, has_farend ? farend : nullptr,
                                       static_cast<int>(frames), out,
                                       static_cast<int>(cap_out), &out_samples);
    if (ret != AUDIO_ENGINE_SUCCESS || out_samples <= 0) {
        // 处理失败：直通兜底
        size_t n = std::min(frames, cap_out);
        std::memcpy(out, in, n * sizeof(int16_t));
        return n;
    }
    return static_cast<size_t>(out_samples);
}

}  // namespace aura::audio
