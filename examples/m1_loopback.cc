// M1 集成 demo：音频采集 → AECM 回声消除 → 事件总线 → 回环播放
// 运行 10 秒自动退出；麦克风声音经管线从扬声器播出（可感知回声消除效果）
//
// 链路验证：
//   采集线程 ─► AudioPipeline::OnCapture ─► AECM ─► Publish(AudioChunk)
//   总线订阅者 ─► AudioPipeline::Play ─► 播放设备 + farend 延迟线
//   Orchestrator 状态机同步运行（M1 无 VAD，状态保持在 Idle）
#include "aura/core/events.h"
#include "aura/platform/audio_device.h"
#include "audio_pipeline.h"
#include "event_bus.h"
#include "log.h"
#include "orchestrator.h"

#include <chrono>
#include <cstdio>
#include <memory>
#include <thread>

using namespace aura;

int main() {
    Log::Instance().SetLevel(LogLevel::kInfo);
    ALOG_INFO("demo", "Aura M1 loopback demo starting...");

    // 1. 总线（先订阅、后启动）
    EventBus bus;
    Orchestrator orchestrator(bus);

    // 2. 音频管线（采集 + 播放 + AECM）
    AudioPipeline pipeline(bus);
    if (!pipeline.Start(platform::CreateAudioSource(), platform::CreateAudioSink())) {
        ALOG_ERROR("demo", "audio pipeline failed to start");
        return 1;
    }

    // 3. 回环订阅：AudioChunk → 播放（演示数据通路）
    bus.Subscribe(EventType::kAudioChunk, [&](const Event& ev) {
        auto chunk = ev.AsAudioChunk();
        if (chunk && chunk->samples && !chunk->samples->empty()) {
            pipeline.Play(chunk->samples->data(), chunk->samples->size());
        }
    });

    bus.Start();
    orchestrator.Start();

    ALOG_INFO("demo", "loopback running for 10s... (speak into the mic, hear yourself)");
    const auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < std::chrono::seconds(10)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    ALOG_INFO("demo", "captured frames: %llu, played frames: %llu, bus dropped: %llu",
              static_cast<unsigned long long>(pipeline.total_capture_frames()),
              static_cast<unsigned long long>(pipeline.total_playback_frames()),
              static_cast<unsigned long long>(bus.TotalDropped()));

    orchestrator.Stop();
    bus.Stop();
    pipeline.Stop();
    ALOG_INFO("demo", "done");
    return 0;
}
