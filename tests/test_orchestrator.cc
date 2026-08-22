// Orchestrator 状态机测试：状态迁移与打断路径
#include "test_framework.h"

#include "orchestrator.h"

#include <chrono>
#include <thread>

namespace aura::test {

namespace {
void WaitMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
}  // namespace

TEST_CASE(orchestrator_idle_to_listening) {
    EventBus bus;
    Orchestrator orch(bus);
    bus.Start();
    orch.Start();

    CHECK(orch.state() == AgentState::kIdle);
    bus.Publish(Event::MakeSpeechBoundary(EventType::kSpeechStart, 1000));
    WaitMs(50);
    CHECK(orch.state() == AgentState::kListening);
    bus.Stop();
}

TEST_CASE(orchestrator_utterance_cycle) {
    EventBus bus;
    Orchestrator orch(bus);
    bus.Start();
    orch.Start();

    bus.Publish(Event::MakeSpeechBoundary(EventType::kSpeechStart, 1000));
    WaitMs(30);
    CHECK(orch.state() == AgentState::kListening);

    bus.Publish(Event::MakeSpeechBoundary(EventType::kSpeechEnd, 2300));
    WaitMs(50);
    // M1：无 LLM/TTS，说话结束回到 Idle
    CHECK(orch.state() == AgentState::kIdle);

    auto stats = orch.stats();
    CHECK(stats.utterances_count == 1);
    CHECK(stats.last_vad_utterance_ms == 1300);
    bus.Stop();
}

TEST_CASE(orchestrator_bargein_interrupts) {
    EventBus bus;
    Orchestrator orch(bus);
    bus.Start();
    orch.Start();

    // 说话中直接打断
    bus.Publish(Event::MakeSpeechBoundary(EventType::kSpeechStart, 1000));
    WaitMs(30);
    bus.Publish(Event::MakeEmpty(EventType::kBargeIn));
    WaitMs(50);
    CHECK(orch.state() == AgentState::kListening);  // 打断 → 继续监听

    auto stats = orch.stats();
    CHECK(stats.bargeins_count == 1);
    bus.Stop();
}

TEST_CASE(orchestrator_bargein_in_idle_noop) {
    EventBus bus;
    Orchestrator orch(bus);
    bus.Start();
    orch.Start();

    bus.Publish(Event::MakeEmpty(EventType::kBargeIn));
    WaitMs(50);
    CHECK(orch.state() == AgentState::kIdle);  // Idle 下打断不迁移
    CHECK(orch.stats().bargeins_count == 0);
    bus.Stop();
}

TEST_CASE(orchestrator_error_reported) {
    EventBus bus;
    Orchestrator orch(bus);
    bus.Start();
    orch.Start();

    // 错误事件只记日志，不改变状态
    bus.Publish(Event::MakeError("audio", -1, "boom"));
    WaitMs(50);
    CHECK(orch.state() == AgentState::kIdle);
    bus.Stop();
}

}  // namespace aura::test
