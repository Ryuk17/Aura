// ALSA 音频采集/播放（Linux / aarch64 树莓派）
#pragma once

#include "aura/core/interfaces.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>

typedef struct _snd_pcm snd_pcm_t;

namespace aura::platform {

class AlsaCaptureDevice : public IAudioSource {
public:
    AlsaCaptureDevice() = default;
    ~AlsaCaptureDevice() override;

    bool Start(SampleCallback cb) override;
    void Stop() override;
    int sample_rate() const override { return 16000; }
    bool IsCapturing() const override { return capturing_.load(); }

private:
    void CaptureLoop();

    SampleCallback cb_;
    std::thread thread_;
    std::atomic<bool> capturing_{false};
    std::atomic<bool> stop_requested_{false};
    snd_pcm_t* pcm_ = nullptr;
};

class AlsaPlaybackDevice : public IAudioSink {
public:
    AlsaPlaybackDevice() = default;
    ~AlsaPlaybackDevice() override;

    bool Start() override;
    void Stop() override;
    void Push(const int16_t* data, size_t frames) override;
    void Clear() override;
    int sample_rate() const override { return 16000; }
    bool IsPlaying() const override { return playing_.load(); }

private:
    void PlaybackLoop();

    std::thread thread_;
    std::atomic<bool> playing_{false};
    std::atomic<bool> stop_requested_{false};

    std::mutex fifo_mutex_;
    std::condition_variable fifo_cv_;
    std::deque<int16_t> fifo_;

    snd_pcm_t* pcm_ = nullptr;
    size_t period_frames_ = 0;
};

}  // namespace aura::platform
