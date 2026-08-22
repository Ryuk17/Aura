// WASAPI 音频采集/播放（Windows，MinGW 编译）
// 共享模式 + AUTOCONVERTPCM：请求 16 kHz/16 bit/mono，系统自动转换
#pragma once

#include "aura/core/interfaces.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>

struct IAudioClient;
struct IAudioCaptureClient;
struct IAudioRenderClient;
struct IMMDeviceEnumerator;

namespace aura::platform {

class WasapiCaptureDevice : public IAudioSource {
public:
    WasapiCaptureDevice();
    ~WasapiCaptureDevice() override;

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

    void* coinit_state_ = nullptr;         // 线程 COM 状态（采集线程内初始化）
    IMMDeviceEnumerator* enumerator_ = nullptr;
    IAudioClient* audio_client_ = nullptr;
    IAudioCaptureClient* capture_client_ = nullptr;
    void* event_handle_ = nullptr;         // 音频事件（HANDLE）
};

class WasapiPlaybackDevice : public IAudioSink {
public:
    WasapiPlaybackDevice();
    ~WasapiPlaybackDevice() override;

    bool Start() override;
    void Stop() override;
    void Push(const int16_t* data, size_t frames) override;
    void Clear() override;
    int sample_rate() const override { return 16000; }
    bool IsPlaying() const override { return playing_.load(); }

private:
    void PlaybackLoop();
    void FillBuffer(uint32_t frames);

    std::thread thread_;
    std::atomic<bool> playing_{false};
    std::atomic<bool> stop_requested_{false};
    void* event_handle_ = nullptr;

    // 待播数据 FIFO（int16 交错帧；vector 保证连续内存便于 memcpy）
    std::mutex fifo_mutex_;
    std::vector<int16_t> fifo_;

    IMMDeviceEnumerator* enumerator_ = nullptr;
    IAudioClient* audio_client_ = nullptr;
    IAudioRenderClient* render_client_ = nullptr;
    uint32_t buffer_frames_ = 0;
};

}  // namespace aura::platform
