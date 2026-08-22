#include "orchestrator.h"

#include "aura/core/interfaces.h"
#include "log.h"

namespace aura {

namespace {
constexpr const char* kTag = "orchestrator";
}

const char* AgentStateName(AgentState s) {
    switch (s) {
        case AgentState::kIdle: return "Idle";
        case AgentState::kListening: return "Listening";
        case AgentState::kProcessing: return "Processing";
        case AgentState::kSpeaking: return "Speaking";
    }
    return "Unknown";
}

Orchestrator::Orchestrator(EventBus& bus) : bus_(bus) {}

Orchestrator::~Orchestrator() { Stop(); }

void Orchestrator::Start() {
    // 订阅顺序：BargeIn 必须先注册（同一优先级下按注册序分发，但 BargeIn 优先级最高，
    // 总线上必然先于其他事件被取出）
    bus_.Subscribe(EventType::kBargeIn, [this](const Event& ev) { OnEvent(ev); });
    bus_.Subscribe(EventType::kSpeechStart, [this](const Event& ev) { OnEvent(ev); });
    bus_.Subscribe(EventType::kSpeechEnd, [this](const Event& ev) { OnEvent(ev); });
    bus_.Subscribe(EventType::kAudioChunk, [this](const Event& ev) { OnEvent(ev); });
    bus_.Subscribe(EventType::kAsrFinal, [this](const Event& ev) { OnEvent(ev); });
    bus_.Subscribe(EventType::kLlmComplete, [this](const Event& ev) { OnEvent(ev); });
    bus_.Subscribe(EventType::kTtsComplete, [this](const Event& ev) { OnEvent(ev); });
    bus_.Subscribe(EventType::kError, [this](const Event& ev) { OnEvent(ev); });
    ALOG_INFO(kTag, "orchestrator started");
}

void Orchestrator::Stop() {
    ALOG_INFO(kTag, "orchestrator stopped");
}

void Orchestrator::Transition(AgentState next) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    AgentState prev = state_.load();
    if (prev == next) return;
    ALOG_DEBUG(kTag, "state: %s -> %s", AgentStateName(prev), AgentStateName(next));
    state_ = next;
}

void Orchestrator::OnEvent(const Event& ev) {
    switch (ev.type) {
        case EventType::kBargeIn:
            // 任何状态下的打断：回 LISTENING，继续采集
            if (state_.load() != AgentState::kIdle) {
                bargeins_.fetch_add(1);
                ALOG_INFO(kTag, "barge-in received (state=%s)",
                          AgentStateName(state_.load()));
                Transition(AgentState::kListening);
            }
            break;

        case EventType::kSpeechStart:
            if (state_.load() == AgentState::kIdle ||
                state_.load() == AgentState::kListening) {
                speech_start_ms_ = ev.timestamp_ms;
                Transition(AgentState::kListening);
            }
            break;

        case EventType::kSpeechEnd:
            if (state_.load() == AgentState::kListening) {
                speech_end_ms_ = ev.timestamp_ms;
                uint64_t dur = speech_end_ms_.load() - speech_start_ms_.load();
                utterances_.fetch_add(1);
                // M1：无 LLM/TTS，说话结束即回到监听（等下一句）
                // M3 起：此处驱动 LLM 调用，进入 kProcessing
                ALOG_DEBUG(kTag, "utterance %llu ended, dur=%llu ms",
                           static_cast<unsigned long long>(utterances_.load()),
                           static_cast<unsigned long long>(dur));
                Transition(AgentState::kIdle);
            }
            break;

        case EventType::kAsrFinal:
            // M3 起：收到整句后进入 kProcessing 并调用 LLM
            if (state_.load() == AgentState::kListening) {
                Transition(AgentState::kProcessing);
                ALOG_DEBUG(kTag, "asr final received (engine pending, M3)");
                Transition(AgentState::kIdle);
            }
            break;

        case EventType::kLlmComplete:
            // M4 起：触发 TTS 进入 kSpeaking
            break;

        case EventType::kTtsComplete:
            if (state_.load() == AgentState::kSpeaking) {
                Transition(AgentState::kListening);
            }
            break;

        case EventType::kAudioChunk:
            // M1：音频块直接透传（由订阅了 AudioChunk 的其他模块消费，如回环 demo）
            break;

        case EventType::kError: {
            auto err = ev.AsError();
            ALOG_ERROR(kTag, "module error: %s code=%d msg=%s",
                       err ? err->module.c_str() : "?",
                       err ? err->code : -1,
                       err ? err->message.c_str() : "?");
            break;
        }

        default:
            break;  // 未订阅类型不会到达
    }
}

Orchestrator::LatencyStats Orchestrator::stats() const {
    LatencyStats s;
    uint64_t start = speech_start_ms_.load();
    uint64_t end = speech_end_ms_.load();
    if (start != 0 && end >= start) s.last_vad_utterance_ms = end - start;
    s.utterances_count = utterances_.load();
    s.bargeins_count = bargeins_.load();
    return s;
}

}  // namespace aura
