// 音频管线：采集 → AECM 回声消除 → 事件总线；播放数据 → AECM farend 延迟线
// 线程模型：
//   采集线程（platform 设备回调）→ OnCapture()：AECM 处理 + Publish(AudioChunk)
//   任意线程 → Play()：写入播放设备 + farend 延迟线
#pragma once

#include "aura/core/interfaces.h"
#include "aecm_processor.h"
#include "event_bus.h"

#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>

namespace aura {

class AudioPipeline {
public:
    explicit AudioPipeline(EventBus& bus);
    ~AudioPipeline();

    AudioPipeline(const AudioPipeline&) = delete;
    AudioPipeline& operator=(const AudioPipeline&) = delete;

    // 装配平台采集/播放设备并启动
    bool Start(std::unique_ptr<IAudioSource> source, std::unique_ptr<IAudioSink> sink);
    void Stop();

    // 播放（TTS 输出 / demo 回环）：入播放队列 + 送 AECM farend 延迟线
    void Play(const int16_t* data, size_t frames);
    // 立即停止播放并清空（barge-in 用）
    void StopPlayback();

    IAudioSink* sink() { return sink_.get(); }
    IAudioSource* source() { return source_.get(); }

    // AEC 开关（默认开；无播放场景可关闭节省算力）
    void SetAecEnabled(bool on) { aec_enabled_ = on; }
    bool aec_enabled() const { return aec_enabled_; }

    // 播放卡缓冲延迟估计（ms），传入 AECM 做延迟对齐；默认 20ms
    void SetEchoDelayEstimateMs(uint16_t ms) { echo_delay_ms_ = ms; }

    uint64_t total_capture_frames() const { return captured_frames_.load(); }
    uint64_t total_playback_frames() const { return played_frames_.load(); }

private:
    void OnCapture(const int16_t* data, size_t frames);
    // 从延迟线取一帧 farend（无则返回 false）
    bool TakeFarendFrame(int16_t out[160]);

    EventBus& bus_;
    std::unique_ptr<IAudioSource> source_;
    std::unique_ptr<IAudioSink> sink_;
    AecmProcessor aecm_;

    bool aec_enabled_ = true;
    uint16_t echo_delay_ms_ = 20;

    // farend 延迟线：FIFO，保存最近播放数据（上限 1s，@16k = 16000 样本）
    static constexpr size_t kFarendMaxSamples = 16000;
    std::mutex farend_mutex_;
    std::deque<int16_t> farend_;

    std::atomic<uint64_t> captured_frames_{0};
    std::atomic<uint64_t> played_frames_{0};
};

}  // namespace aura
