// ALSA 采集/播放实现（16 kHz / 16 bit / mono，period = 10ms）
#include "alsa_device.h"

#include "log.h"

#ifdef __linux__

#include <alsa/asoundlib.h>

#include <cstring>

namespace aura::platform {

namespace {
constexpr const char* kTag = "alsa";

// 配置 PCM 为 16 kHz / mono / S16_LE / 10ms period
bool ConfigurePcm(snd_pcm_t* pcm, snd_pcm_stream_t stream, size_t* period_frames) {
    unsigned rate = 16000;
    if (snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED,
                           1, rate, 1, 100000 /* 100ms 启动延迟容忍 */) < 0) {
        ALOG_ERROR(kTag, "snd_pcm_set_params failed");
        return false;
    }
    snd_pcm_uframes_t frames = 160;  // 10ms
    // 用 hw_params 请求 10ms period（裁剪版 ALSA 头文件缺失 sw_params setter，
    // 但 snd_pcm_set_period_size 与 snd_pcm_sw_params_set_* 均不可用）
    snd_pcm_hw_params_t* hw = nullptr;
    snd_pcm_hw_params_alloca(&hw);
    if (snd_pcm_hw_params_current(pcm, hw) == 0 &&
        snd_pcm_hw_params_set_period_size_near(pcm, hw, &frames, 0) == 0) {
        snd_pcm_hw_params(pcm, hw);
    } else {
        // 非致命：使用驱动默认 period
        snd_pcm_uframes_t actual = 0;
        snd_pcm_get_params(pcm, &actual, nullptr);
        frames = actual;
    }
    *period_frames = frames;
    return true;
}

// 处理 PCM 欠载/过载：恢复后继续
void RecoverPcm(snd_pcm_t* pcm, int err) {
    if (err == -EPIPE) {
        snd_pcm_prepare(pcm);
    } else if (err == -ESTRPIPE) {
        while ((err = snd_pcm_resume(pcm)) == -EAGAIN) {
            // 等待驱动恢复
        }
        if (err < 0) snd_pcm_prepare(pcm);
    } else if (err < 0) {
        snd_pcm_prepare(pcm);
    }
}

}  // namespace

// ---------------- 采集 ----------------

AlsaCaptureDevice::~AlsaCaptureDevice() { Stop(); }

bool AlsaCaptureDevice::Start(SampleCallback cb) {
    if (capturing_.load()) return true;
    cb_ = std::move(cb);
    if (!cb_) return false;

    if (snd_pcm_open(&pcm_, "default", SND_PCM_STREAM_CAPTURE, 0) < 0) {
        ALOG_ERROR(kTag, "snd_pcm_open (capture) failed");
        return false;
    }
    size_t period = 0;
    if (!ConfigurePcm(pcm_, SND_PCM_STREAM_CAPTURE, &period)) {
        return false;
    }

    stop_requested_ = false;
    capturing_ = true;
    thread_ = std::thread(&AlsaCaptureDevice::CaptureLoop, this);
    ALOG_INFO(kTag, "capture started (16 kHz/16 bit/mono)");
    return true;
}

void AlsaCaptureDevice::Stop() {
    if (!capturing_.load()) return;
    stop_requested_ = true;
    if (thread_.joinable()) thread_.join();
    capturing_ = false;
    if (pcm_) {
        snd_pcm_drain(pcm_);
        snd_pcm_close(pcm_);
        pcm_ = nullptr;
    }
    ALOG_INFO(kTag, "capture stopped");
}

void AlsaCaptureDevice::CaptureLoop() {
    size_t period = 160;  // 默认 10ms
    // 从驱动查询实际 period
    snd_pcm_uframes_t f = 0;
    if (snd_pcm_get_params(pcm_, &f, nullptr) == 0 && f > 0) period = f;

    std::vector<int16_t> buf(period);
    while (!stop_requested_.load()) {
        snd_pcm_sframes_t n = snd_pcm_readi(pcm_, buf.data(), period);
        if (n < 0) {
            if (n == -EAGAIN) {
                continue;
            }
            if (n == -EINTR) {
                continue;
            }
            RecoverPcm(pcm_, static_cast<int>(n));
            continue;
        }
        if (n > 0) {
            cb_(buf.data(), static_cast<size_t>(n));
        }
    }
}

// ---------------- 播放 ----------------

AlsaPlaybackDevice::~AlsaPlaybackDevice() { Stop(); }

bool AlsaPlaybackDevice::Start() {
    if (playing_.load()) return true;

    if (snd_pcm_open(&pcm_, "default", SND_PCM_STREAM_PLAYBACK, 0) < 0) {
        ALOG_ERROR(kTag, "snd_pcm_open (playback) failed");
        return false;
    }
    size_t period = 0;
    if (!ConfigurePcm(pcm_, SND_PCM_STREAM_PLAYBACK, &period)) {
        return false;
    }
    period_frames_ = period ? period : 160;

    stop_requested_ = false;
    playing_ = true;
    thread_ = std::thread(&AlsaPlaybackDevice::PlaybackLoop, this);
    ALOG_INFO(kTag, "playback started (16 kHz/16 bit/mono)");
    return true;
}

void AlsaPlaybackDevice::Stop() {
    if (!playing_.load()) return;
    stop_requested_ = true;
    fifo_cv_.notify_all();
    if (thread_.joinable()) thread_.join();
    playing_ = false;
    {
        std::lock_guard<std::mutex> lock(fifo_mutex_);
        fifo_.clear();
    }
    if (pcm_) {
        snd_pcm_drop(pcm_);
        snd_pcm_close(pcm_);
        pcm_ = nullptr;
    }
    ALOG_INFO(kTag, "playback stopped");
}

void AlsaPlaybackDevice::Push(const int16_t* data, size_t frames) {
    {
        std::lock_guard<std::mutex> lock(fifo_mutex_);
        fifo_.insert(fifo_.end(), data, data + frames);
    }
    fifo_cv_.notify_one();
}

void AlsaPlaybackDevice::Clear() {
    std::lock_guard<std::mutex> lock(fifo_mutex_);
    fifo_.clear();
}

void AlsaPlaybackDevice::PlaybackLoop() {
    std::vector<int16_t> buf(period_frames_);
    while (!stop_requested_.load()) {
        {
            std::unique_lock<std::mutex> lock(fifo_mutex_);
            // 等待有数据（静音时也保证周期输出，用 10ms 超时兜底）
            if (fifo_cv_.wait_for(lock, std::chrono::milliseconds(20),
                                  [this] { return !fifo_.empty() || stop_requested_.load(); }) &&
                !fifo_.empty()) {
                size_t take = std::min(period_frames_, fifo_.size());
                std::copy(fifo_.begin(), fifo_.begin() + take, buf.begin());
                fifo_.erase(fifo_.begin(), fifo_.begin() + take);
                std::fill(buf.begin() + take, buf.end(), 0);
            } else if (!stop_requested_.load()) {
                std::fill(buf.begin(), buf.end(), 0);
            } else {
                break;
            }
        }
        snd_pcm_sframes_t n = snd_pcm_writei(pcm_, buf.data(), period_frames_);
        if (n < 0) {
            RecoverPcm(pcm_, static_cast<int>(n));
        }
    }
}

}  // namespace aura::platform

#else
#error "alsa_device.cc is Linux-only"
#endif
