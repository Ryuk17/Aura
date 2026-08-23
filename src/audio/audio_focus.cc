// AudioFocus 实现
#include "audio_focus.h"

namespace aura::audio {

bool AudioFocus::BeginPlayback() {
    if (playing_.exchange(true)) return false;  // 已在播放：拒绝重入
    state_.store(State::kSpeaking);
    return true;
}

void AudioFocus::EndPlayback() {
    playing_.store(false);
    state_.store(State::kCapturing);
}

void AudioFocus::OnBargeIn() {
    playing_.store(false);
    state_.store(State::kCapturing);
}

const char* AudioFocus::StateName() const {
    switch (state_.load()) {
        case State::kIdle: return "idle";
        case State::kCapturing: return "capturing";
        case State::kSpeaking: return "speaking";
    }
    return "?";
}

}  // namespace aura::audio
