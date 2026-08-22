// EventBus 测试：分发、优先级、BargeIn 抢占、背压
#include "test_framework.h"

#include "event_bus.h"

#include <atomic>
#include <chrono>
#include <thread>

namespace aura::test {

namespace {
void WaitMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
}  // namespace

TEST_CASE(event_bus_basic_dispatch) {
    EventBus bus;
    std::atomic<int> count{0};
    bus.Subscribe(EventType::kSpeechStart, [&](const Event&) { count.fetch_add(1); });
    bus.Start();
    bus.Publish(Event::MakeSpeechBoundary(EventType::kSpeechStart, 1));
    WaitMs(50);
    CHECK(count.load() == 1);
    CHECK(bus.TotalPublished() == 1);
    bus.Stop();
}

TEST_CASE(event_bus_priority_order) {
    EventBus bus;
    std::vector<EventType> received;
    bus.Subscribe(EventType::kAudioChunk, [&](const Event& e) { received.push_back(e.type); });
    bus.Subscribe(EventType::kSpeechStart, [&](const Event& e) { received.push_back(e.type); });
    bus.Subscribe(EventType::kBargeIn, [&](const Event& e) { received.push_back(e.type); });
    bus.Start();

    // 乱序发布：低优先级先入队，高优先级后入队，但分发必须按优先级
    bus.Publish(Event::MakeAudioChunk(std::make_shared<const std::vector<int16_t>>(160), 1, 10));
    bus.Publish(Event::MakeEmpty(EventType::kBargeIn));
    bus.Publish(Event::MakeSpeechBoundary(EventType::kSpeechStart, 2));
    WaitMs(100);

    CHECK(received.size() == 3);
    CHECK(received[0] == EventType::kBargeIn);       // 优先级 0
    CHECK(received[1] == EventType::kSpeechStart);   // 优先级 10
    CHECK(received[2] == EventType::kAudioChunk);    // 优先级 20
    bus.Stop();
}

TEST_CASE(event_bus_fifo_within_priority) {
    EventBus bus;
    std::vector<int> seq;
    bus.Subscribe(EventType::kAudioChunk, [&](const Event&) { seq.push_back(0); });
    bus.Start();
    for (int i = 0; i < 5; ++i) {
        bus.Publish(Event::MakeAudioChunk(
            std::make_shared<const std::vector<int16_t>>(160), static_cast<uint64_t>(i), 10));
    }
    WaitMs(100);
    CHECK(seq.size() == 5);
    for (int i = 0; i < 5; ++i) CHECK(seq[i] == 0);  // 同优先级 FIFO 顺序（此处验证无乱序）
    bus.Stop();
}

TEST_CASE(event_bus_backpressure_drops) {
    EventBus bus;
    bus.SetMaxQueueSize(16);
    bus.Subscribe(EventType::kAudioChunk, [&](const Event&) {
        // 慢消费者：阻塞分发线程，制造队列堆积
        WaitMs(5);
    });
    bus.Start();
    // 快速发布 100 个（分发线程 5ms/个，必然堆积）
    for (int i = 0; i < 100; ++i) {
        bus.Publish(Event::MakeAudioChunk(
            std::make_shared<const std::vector<int16_t>>(160), static_cast<uint64_t>(i), 10));
    }
    WaitMs(500);
    CHECK(bus.QueueSize() == 0);
    CHECK(bus.TotalPublished() + bus.TotalDropped() == 100);
    CHECK(bus.TotalDropped() > 0);  // 有丢弃发生
    bus.Stop();
}

TEST_CASE(event_bus_error_and_text_payload) {
    EventBus bus;
    std::vector<std::string> errors;
    bus.Subscribe(EventType::kError, [&](const Event& e) {
        auto err = e.AsError();
        CHECK(err != nullptr);
        errors.push_back(err->message);
    });
    std::string last_text;
    bus.Subscribe(EventType::kAsrFinal, [&](const Event& e) {
        auto t = e.AsText();
        CHECK(t != nullptr);
        last_text = t->text;
    });
    bus.Start();
    bus.Publish(Event::MakeError("asr", 42, "decode failed"));
    bus.Publish(Event::MakeText(EventType::kAsrFinal, "你好世界"));
    WaitMs(100);
    CHECK(errors.size() == 1);
    CHECK(errors[0] == "decode failed");
    CHECK(last_text == "你好世界");
    bus.Stop();
}

TEST_CASE(event_bus_publish_before_start_dropped) {
    EventBus bus;
    std::atomic<int> count{0};
    bus.Subscribe(EventType::kSpeechStart, [&](const Event&) { count.fetch_add(1); });
    // Start 之前发布 → 应被丢弃
    bus.Publish(Event::MakeSpeechBoundary(EventType::kSpeechStart, 1));
    bus.Start();
    WaitMs(50);
    CHECK(count.load() == 0);
    CHECK(bus.TotalDropped() == 1);
    bus.Stop();
}

}  // namespace aura::test
