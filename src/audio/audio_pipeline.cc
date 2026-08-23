// AudioPipeline 实现：AudioChain（默认 AECM 节点）+ 事件总线装配
#include "audio_pipeline.h"

#include "aecm_node.h"
#include "aura/core/events.h"
#include "log.h"

#include <algorithm>
#include <vector>

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

    // 默认链：AECM（无节点配置时）；链路初始化失败不阻塞（降级直通）
    if (chain_.size() == 0) {
        auto aecm = std::make_unique<audio::AecmNode>();
        if (!aecm->Init(16000)) {
            ALOG_WARN(kTag, "AECM init failed, running without echo cancellation");
        }
        chain_.AddNode(std::move(aecm));
    }
    aecm_ = dynamic_cast<audio::AecmNode*>(chain_.Find("aecm"));
    chain_.Init(16000);

    if (!sink_->Start()) {
        ALOG_ERROR(kTag, "audio sink start failed");
        return false;
    }
    if (!source_->Start(
            [this](const int16_t* data, size_t frames) { OnCapture(data, frames); })) {
        ALOG_ERROR(kTag, "audio source start failed");
        sink_->Stop();
        return false;
    }

    focus_.OnBargeIn();  // 初始无播放，处于采集态
    ALOG_INFO(kTag, "pipeline started (chain=%zu nodes, aecm=%s)", chain_.size(),
              aecm_ ? "on" : "off");
    return true;
}

void AudioPipeline::Stop() {
    if (source_) source_->Stop();
    if (sink_) sink_->Stop();
    ALOG_INFO(kTag, "pipeline stopped");
}

void AudioPipeline::Play(const int16_t* data, size_t frames) {
    if (!data || frames == 0 || !sink_) return;
    sink_->Push(data, frames);
    played_frames_.fetch_add(frames);

    // 播放信号注入链内 AECM 的 farend 延迟线
    if (aecm_) aecm_->FeedFarend(data, frames);
}

void AudioPipeline::StopPlayback() {
    if (sink_) sink_->Clear();
    focus_.OnBargeIn();
}

void AudioPipeline::OnCapture(const int16_t* data, size_t frames) {
    if (frames == 0) return;
    captured_frames_.fetch_add(frames);

    std::vector<int16_t> output(frames);
    size_t got = chain_.Process(data, frames, output.data(), output.size());
    if (got == 0 || got > output.size()) {
        // 处理异常：直通兜底
        std::copy_n(data, frames, output.data());
        got = frames;
    }
    output.resize(got);

    auto samples = std::make_shared<const std::vector<int16_t>>(std::move(output));
    uint64_t now = Log::NowMs();
    bus_.Publish(Event::MakeAudioChunk(
        samples, now, static_cast<uint64_t>(frames) * 1000 / AURA_SAMPLE_RATE));
}

}  // namespace aura
