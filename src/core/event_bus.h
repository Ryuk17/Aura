// 事件总线：优先级队列 + 单分发线程（自研轻量，零第三方依赖）
// 规则：
//   - Subscribe 必须在 Start() 之前调用（handler 表由分发线程独占访问）
//   - Publish 任意线程可调，按优先级入队（kBargeIn 优先级最高）
//   - 背压：队列满时丢弃新事件并计数（对 AudioChunk 最友好，重负载下优先丢音频）
#pragma once

#include "aura/core/events.h"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <map>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace aura {

class EventBus {
public:
    using Handler = std::function<void(const Event&)>;

    EventBus() = default;
    ~EventBus();

    EventBus(const EventBus&) = delete;
    EventBus& operator=(const EventBus&) = delete;

    // 启动分发线程（Subscribe 之后再调用）
    void Start();
    void Stop();

    // 订阅某类事件；必须在 Start() 前调用
    void Subscribe(EventType type, Handler handler);

    // 发布事件（线程安全）
    void Publish(Event ev);

    // 状态查询
    size_t QueueSize() const;
    uint64_t TotalPublished() const { return published_.load(); }
    uint64_t TotalDropped() const { return dropped_.load(); }
    bool IsRunning() const { return running_.load(); }

    // 队列容量（默认 2048）
    void SetMaxQueueSize(size_t n) { max_queue_ = n; }

private:
    struct Entry {
        int priority;
        uint64_t seq;
        Event event;
    };
    struct Cmp {
        // 小顶堆：priority 小者优先；同优先级按 seq（先入先出）
        bool operator()(const Entry& a, const Entry& b) const {
            if (a.priority != b.priority) return a.priority > b.priority;
            return a.seq > b.seq;
        }
    };

    void DispatchLoop();

    std::priority_queue<Entry, std::vector<Entry>, Cmp> queue_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::atomic<bool> running_{false};
    uint64_t seq_ = 0;
    size_t max_queue_ = 2048;
    size_t warn_threshold_ = 100;  // 每丢弃这么多条打一次 WARN

    std::map<EventType, std::vector<Handler>> handlers_;
    std::thread thread_;

    std::atomic<uint64_t> published_{0};
    std::atomic<uint64_t> dropped_{0};
};

}  // namespace aura
