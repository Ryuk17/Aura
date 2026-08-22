// Orchestrator：会话状态机 + 模块装配 + 延迟监控
// M1 阶段：状态迁移框架 + 音频链路事件处理（SpeechStart/End、BargeIn、AudioChunk）；
//          ASR/LLM/TTS 引擎为空占位（M2/M3 注入）
#pragma once

#include "aura/core/events.h"
#include "event_bus.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>

namespace aura {

enum class AgentState : uint8_t {
    kIdle,       // 常驻：仅 VAD/轮次检测运行
    kListening,  // 采集 + ASR 流式
    kProcessing, // LLM 生成回复
    kSpeaking,   // TTS 播放
};

const char* AgentStateName(AgentState s);

class Orchestrator {
public:
    explicit Orchestrator(EventBus& bus);
    ~Orchestrator();

    // 订阅总线并开始处理事件
    void Start();
    void Stop();

    AgentState state() const { return state_.load(); }

    // 延迟监控统计（单位 ms，0 表示未记录）
    struct LatencyStats {
        uint64_t last_vad_utterance_ms = 0;  // SpeechStart → SpeechEnd
        uint64_t last_turn_total_ms = 0;     // SpeechStart → TtsComplete（M1 为 0）
        uint64_t utterances_count = 0;
        uint64_t bargeins_count = 0;
    };
    LatencyStats stats() const;

private:
    void OnEvent(const Event& ev);
    void Transition(AgentState next);

    EventBus& bus_;
    std::atomic<AgentState> state_{AgentState::kIdle};
    std::mutex state_mutex_;

    // 状态时间线（延迟监控用）
    std::atomic<uint64_t> speech_start_ms_{0};
    std::atomic<uint64_t> speech_end_ms_{0};
    std::atomic<uint64_t> processing_start_ms_{0};
    std::atomic<uint64_t> utterances_{0};
    std::atomic<uint64_t> bargeins_{0};
};

}  // namespace aura
