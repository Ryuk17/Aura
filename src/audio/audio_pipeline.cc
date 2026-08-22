#include "audio_pipeline.h"

#include "aura/core/events.h"
#include "log.h"

#include <algorithm>

namespace aura {

namespace {
constexpr const char* kTag = "audio_pipeline";
}

AudioPipeline::AudioPipeline(EventBus& bus) : bus_(bus) {}

AudioPipeline::~AudioPipeline() { Stop(); }

bool AudioPipeline::Start(std::unique_ptr<IAudioSource> source,
                          std::unique_ptr<IAudioSink> sink) {
    if (!source || !sink) {
        ALOG_ERROR(kTag, "Start requires both source and sink");
        return false;
    }
    source_ = std::move(source);
    sink_ = std::move(sink);

    if (!sink_->Start()) {
        ALOG_ERROR(kTag, "audio sink start failed");
        return false;
    }

    // AECM 初始化失败不阻塞链路（关闭 AEC 继续跑）
    if (aec_enabled_) {
        if (!aecm_.Init(16000)) {
            ALOG_WARN(kTag, "AECM init failed, running without echo cancellation");
            aec_enabled_ = false;
        }
    }

    if (!source_->Start(
            [this](const int16_t* data, size_t frames) { OnCapture(data, frames); })) {
        ALOG_ERROR(kTag, "audio source start failed");
        sink_->Stop();
        return false;
    }

    ALOG_INFO(kTag, "pipeline started (aec=%s)", aec_enabled_ ? "on" : "off");
    return true;
}

void AudioPipeline::Stop() {
    if (source_) source_->Stop();
    if (sink_) sink_->Stop();
    {
        std::lock_guard<std::mutex> lock(farend_mutex_);
        farend_.clear();
    }
    ALOG_INFO(kTag, "pipeline stopped");
}

void AudioPipeline::Play(const int16_t* data, size_t frames) {
    if (!data || frames == 0 || !sink_) return;
    sink_->Push(data, frames);
    played_frames_.fetch_add(frames);

    // 写入 farend 延迟线（上限内丢弃最旧）
    std::lock_guard<std::mutex> lock(farend_mutex_);
    if (farend_.size() + frames > kFarendMaxSamples) {
        size_t drop = farend_.size() + frames - kFarendMaxSamples;
        farend_.erase(farend_.begin(), farend_.begin() + drop);
    }
    farend_.insert(farend_.end(), data, data + frames);
}

void AudioPipeline::StopPlayback() {
    if (sink_) sink_->Clear();
    // 延迟线保留：播放卡内仍有缓冲，AECM 延迟估计覆盖
}

bool AudioPipeline::TakeFarendFrame(int16_t out[160]) {
    std::lock_guard<std::mutex> lock(farend_mutex_);
    if (farend_.size() < 160) return false;
    std::copy_n(farend_.begin(), 160, out);
    farend_.erase(farend_.begin(), farend_.begin() + 160);
    return true;
}

void AudioPipeline::OnCapture(const int16_t* data, size_t frames) {
    if (frames == 0) return;
    captured_frames_.fetch_add(frames);

    // 帧对齐：WASAPI 可能返回非 160 的块（共享模式转换后通常 160），逐帧处理
    std::vector<int16_t> output;
    output.reserve(frames);

    if (aec_enabled_ && aecm_.initialized() && frames == 160) {
        int16_t farend[160];
        int16_t out[160];
        bool has_farend = TakeFarendFrame(farend);
        aecm_.ProcessFrame(has_farend ? farend : nullptr, data, out);
        output.insert(output.end(), out, out + 160);
    } else {
        // AEC 关闭或非标准帧长：直通
        output.insert(output.end(), data, data + frames);
    }

    auto samples = std::make_shared<const std::vector<int16_t>>(std::move(output));
    uint64_t now = Log::NowMs();
    bus_.Publish(Event::MakeAudioChunk(
        samples, now, static_cast<uint64_t>(frames) * 1000 / 16000));
}

}  // namespace aura
