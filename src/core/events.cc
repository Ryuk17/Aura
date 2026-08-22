#include "aura/core/events.h"

namespace aura {

const char* EventTypeName(EventType type) {
    switch (type) {
        case EventType::kAudioChunk: return "AudioChunk";
        case EventType::kWakeWordDetected: return "WakeWordDetected";
        case EventType::kSpeechStart: return "SpeechStart";
        case EventType::kSpeechEnd: return "SpeechEnd";
        case EventType::kAsrPartial: return "AsrPartial";
        case EventType::kAsrFinal: return "AsrFinal";
        case EventType::kLlmToken: return "LlmToken";
        case EventType::kLlmToolCall: return "LlmToolCall";
        case EventType::kLlmToolResult: return "LlmToolResult";
        case EventType::kLlmComplete: return "LlmComplete";
        case EventType::kTtsChunk: return "TtsChunk";
        case EventType::kTtsComplete: return "TtsComplete";
        case EventType::kBargeIn: return "BargeIn";
        case EventType::kError: return "Error";
        case EventType::kEventTypeCount: break;
    }
    return "Unknown";
}

int EventPriority(EventType type) {
    // 数字越小优先级越高
    switch (type) {
        case EventType::kBargeIn: return 0;
        case EventType::kError: return 5;
        case EventType::kSpeechStart: return 10;
        case EventType::kSpeechEnd: return 10;
        case EventType::kWakeWordDetected: return 10;
        case EventType::kAudioChunk: return 20;
        case EventType::kAsrPartial: return 30;
        case EventType::kAsrFinal: return 30;
        case EventType::kLlmComplete: return 40;
        case EventType::kTtsChunk: return 50;
        case EventType::kTtsComplete: return 50;
        case EventType::kLlmToken: return 60;
        case EventType::kLlmToolCall: return 60;
        case EventType::kLlmToolResult: return 70;
        case EventType::kEventTypeCount: break;
    }
    return 100;
}

Event Event::MakeAudioChunk(std::shared_ptr<const std::vector<int16_t>> samples,
                            uint64_t capture_ms, uint64_t duration_ms) {
    Event e;
    e.type = EventType::kAudioChunk;
    e.timestamp_ms = capture_ms;
    auto p = std::make_shared<AudioChunkPayload>();
    p->samples = std::move(samples);
    p->capture_ms = capture_ms;
    p->duration_ms = duration_ms;
    e.payload = p;
    return e;
}

Event Event::MakeSpeechBoundary(EventType type, uint64_t at_ms) {
    Event e;
    e.type = type;
    e.timestamp_ms = at_ms;
    auto p = std::make_shared<SpeechBoundaryPayload>();
    p->at_ms = at_ms;
    e.payload = p;
    return e;
}

Event Event::MakeText(EventType type, std::string text, float confidence, bool is_partial) {
    Event e;
    e.type = type;
    auto p = std::make_shared<TextPayload>();
    p->text = std::move(text);
    p->confidence = confidence;
    p->is_partial = is_partial;
    e.payload = p;
    return e;
}

Event Event::MakeError(std::string module, int code, std::string message) {
    Event e;
    e.type = EventType::kError;
    auto p = std::make_shared<ErrorPayload>();
    p->module = std::move(module);
    p->code = code;
    p->message = std::move(message);
    e.payload = p;
    return e;
}

Event Event::MakeEmpty(EventType type) {
    Event e;
    e.type = type;
    return e;
}

const AudioChunkPayload* Event::AsAudioChunk() const {
    return type == EventType::kAudioChunk
               ? static_cast<const AudioChunkPayload*>(payload.get())
               : nullptr;
}

const SpeechBoundaryPayload* Event::AsSpeechBoundary() const {
    return (type == EventType::kSpeechStart || type == EventType::kSpeechEnd)
               ? static_cast<const SpeechBoundaryPayload*>(payload.get())
               : nullptr;
}

const TextPayload* Event::AsText() const {
    switch (type) {
        case EventType::kAsrPartial:
        case EventType::kAsrFinal:
        case EventType::kLlmToken:
        case EventType::kLlmToolCall:
        case EventType::kLlmToolResult:
            return static_cast<const TextPayload*>(payload.get());
        default:
            return nullptr;
    }
}

const ErrorPayload* Event::AsError() const {
    return type == EventType::kError ? static_cast<const ErrorPayload*>(payload.get())
                                     : nullptr;
}

}  // namespace aura
