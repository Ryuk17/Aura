// 事件定义（公共头）：全链路模块间的唯一通信载体
// 约定：事件不可变（const），跨线程传递所有权；payload 按 type 判读
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace aura {

enum class EventType : uint8_t {
    kAudioChunk,        // 麦克风 PCM 帧（AECM 处理后，16 kHz/16 bit/mono）
    kWakeWordDetected,  // 唤醒词命中（v0 占位，不触发）
    kSpeechStart,       // 检出语音起始
    kSpeechEnd,         // 检出语句结束
    kAsrPartial,        // ASR 增量文本
    kAsrFinal,          // ASR 整句定稿
    kLlmToken,          // LLM 流式 token 增量
    kLlmToolCall,       // LLM 请求调用工具
    kLlmToolResult,     // 工具执行结果
    kLlmComplete,       // 回复生成完毕
    kTtsChunk,          // TTS PCM 块
    kTtsComplete,       // 播放完成
    kBargeIn,           // 打断请求（最高优先级）
    kError,             // 模块错误
    kEventTypeCount,
};

const char* EventTypeName(EventType type);

// 事件优先级（数字越小越先处理）
int EventPriority(EventType type);

// ---------------- Payload 定义 ----------------

// 音频块：16 kHz / 16 bit / mono 的 int16 采样
struct AudioChunkPayload {
    std::shared_ptr<const std::vector<int16_t>> samples;
    uint64_t capture_ms = 0;   // 采集时刻（Log::NowMs 时间轴）
    uint64_t duration_ms = 0;  // 块时长
};

// 语音边界（kSpeechStart / kSpeechEnd 共用）
struct SpeechBoundaryPayload {
    uint64_t at_ms = 0;  // 边界发生时刻
};

// 文本类事件（ASR / LLM 共用）
struct TextPayload {
    std::string text;
    float confidence = 1.0f;       // ASR 用
    bool is_partial = false;       // ASR 增量标记
    std::string tool_name;         // kLlmToolCall 用：目标工具名
    std::string tool_args_json;    // kLlmToolCall 用：工具参数（JSON 字符串）
};

// 错误事件
struct ErrorPayload {
    std::string module;
    int code = 0;
    std::string message;
};

struct Event {
    EventType type = EventType::kEventTypeCount;
    uint64_t timestamp_ms = 0;  // Log::NowMs() 时间轴

    // 具体 payload 由 type 决定：
    //   kAudioChunk    -> AudioChunkPayload
    //   kSpeechStart/End -> SpeechBoundaryPayload
    //   kAsr/kLlm 文本类 -> TextPayload
    //   kError         -> ErrorPayload
    //   其余为 null
    std::shared_ptr<const void> payload;

    // 便捷构造
    static Event MakeAudioChunk(std::shared_ptr<const std::vector<int16_t>> samples,
                                uint64_t capture_ms, uint64_t duration_ms);
    static Event MakeSpeechBoundary(EventType type, uint64_t at_ms);
    static Event MakeText(EventType type, std::string text, float confidence = 1.0f,
                          bool is_partial = false);
    static Event MakeError(std::string module, int code, std::string message);
    static Event MakeEmpty(EventType type);

    // payload 类型安全读取（判错返回 nullptr）
    const AudioChunkPayload* AsAudioChunk() const;
    const SpeechBoundaryPayload* AsSpeechBoundary() const;
    const TextPayload* AsText() const;
    const ErrorPayload* AsError() const;
};

}  // namespace aura
