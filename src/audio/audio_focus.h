// AudioFocus：播放/采集焦点管理 + barge-in 联动
// 约束：采集常开（barge-in 前提），播放可插；barge-in 时播放立即停止、采集继续。
#pragma once

#include <atomic>

namespace aura::audio {

class AudioFocus {
public:
    enum class State : uint8_t { kIdle, kCapturing, kSpeaking };

    AudioFocus() = default;

    // TTS 开始播放（采集同时进行）；成功返回 true
    bool BeginPlayback();
    // TTS 播放结束
    void EndPlayback();

    // barge-in：播放立即停止（由 Orchestrator 在 kBargeIn 时调用）
    void OnBargeIn();

    State state() const { return state_.load(); }
    const char* StateName() const;

private:
    std::atomic<State> state_{State::kIdle};
    std::atomic<bool> playing_{false};
};

}  // namespace aura::audio
