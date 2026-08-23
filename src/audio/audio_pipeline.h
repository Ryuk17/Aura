// 音频管线：采集 → AudioChain（默认 AECM 回声消除）→ 事件总线；
// 播放数据 → sink + 链内 AECM farend 延迟线
// 线程模型：
//   采集线程（platform 设备回调）→ OnCapture()：chain.Process() + Publish(AudioChunk)
//   任意线程 → Play()：写入播放设备 + FeedFarend()
#pragma once

#include "audio_chain.h"
#include "audio_focus.h"
#include "aura/core/interfaces.h"
#include "event_bus.h"

#include <atomic>
#include <cstdint>
#include <memory>

namespace aura::audio {
class AecmNode;  // 仅指针成员，前置声明即可
}  // namespace aura::audio

namespace aura {

class AudioPipeline {
public:
    explicit AudioPipeline(EventBus& bus);
    ~AudioPipeline();

    AudioPipeline(const AudioPipeline&) = delete;
    AudioPipeline& operator=(const AudioPipeline&) = delete;

    // 装配平台采集/播放设备并启动（默认链：AECM；可在 Start 前配置 chain()）
    bool Start(std::unique_ptr<IAudioSource> source, std::unique_ptr<IAudioSink> sink);
    void Stop();

    // 播放（TTS 输出 / demo 回环）：入播放队列 + 送链内 AECM farend 延迟线
    void Play(const int16_t* data, size_t frames);
    // 立即停止播放并清空（barge-in 用）
    void StopPlayback();

    IAudioSink* sink() { return sink_.get(); }
    IAudioSource* source() { return source_.get(); }

    // 链（可在 Start 前 AddNode 定制；默认仅 AECM）
    audio::AudioChain& chain() { return chain_; }
    audio::AudioFocus& focus() { return focus_; }

    // 链内 AECM 节点（无则 nullptr）；Play 时自动喂 farend
    audio::AecmNode* aecm() { return aecm_; }

    uint64_t total_capture_frames() const { return captured_frames_.load(); }
    uint64_t total_playback_frames() const { return played_frames_.load(); }

private:
    void OnCapture(const int16_t* data, size_t frames);

    EventBus& bus_;
    audio::AudioChain chain_;
    audio::AudioFocus focus_;
    audio::AecmNode* aecm_ = nullptr;  // 链内查找，非拥有

    std::unique_ptr<IAudioSource> source_;
    std::unique_ptr<IAudioSink> sink_;

    std::atomic<uint64_t> captured_frames_{0};
    std::atomic<uint64_t> played_frames_{0};
};

}  // namespace aura
