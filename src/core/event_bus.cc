#include "event_bus.h"

#include "log.h"

#include <algorithm>
#include <chrono>

#ifndef _WIN32
#include <pthread.h>
#endif

namespace aura {

namespace {
constexpr const char* kTag = "event_bus";
}

EventBus::~EventBus() { Stop(); }

void EventBus::Subscribe(EventType type, Handler handler) {
    std::lock_guard<std::mutex> lock(mutex_);
    handlers_[type].push_back(std::move(handler));
}

void EventBus::Publish(Event ev) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_) {
            // 未启动时分发线程不消费，直接丢弃
            dropped_.fetch_add(1);
            return;
        }
        if (queue_.size() >= max_queue_) {
            // 背压：丢弃新事件
            dropped_.fetch_add(1);
            if (dropped_.load() % warn_threshold_ == 0) {
                ALOG_WARN(kTag, "queue full, dropping events (total dropped=%llu, queue=%zu)",
                          static_cast<unsigned long long>(dropped_.load()), queue_.size());
            }
            return;
        }
        Entry entry{EventPriority(ev.type), seq_++, std::move(ev)};
        queue_.push(std::move(entry));
        published_.fetch_add(1);
    }
    cv_.notify_one();
}

void EventBus::Start() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (running_) return;
    running_ = true;
    thread_ = std::thread(&EventBus::DispatchLoop, this);
    ALOG_DEBUG(kTag, "dispatch thread started");
}

void EventBus::Stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_) {
            ALOG_DEBUG(kTag, "stop: already stopped, skip");
            return;
        }
        ALOG_DEBUG(kTag, "stop: setting running_=false, queue=%zu", queue_.size());
        running_ = false;
    }
    cv_.notify_all();
    ALOG_DEBUG(kTag, "stop: notified, joining dispatch thread...");
    if (thread_.joinable()) {
        thread_.join();
    }
    ALOG_DEBUG(kTag, "stop: dispatch thread joined");
}

size_t EventBus::QueueSize() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size();
}

void EventBus::DispatchLoop() {
    // 分发线程线程名（调试用）
#ifdef _WIN32
    // MinGW 下无 pthread_setname_np，跳过
#else
    pthread_setname_np(pthread_self(), "aura-event-bus");
#endif

    for (;;) {
        Entry entry;
        {
            std::unique_lock<std::mutex> lock(mutex_);
#ifdef _WIN32
            // MinGW winpthread 条件变量超时/唤醒存在 bug（gcc 15.2 实测挂死），
            // Windows 分支用轮询：事件粒度最小 10ms（音频帧），2ms 轮询延迟可忽略。
            if (queue_.empty()) {
                if (!running_) {
                    ALOG_DEBUG(kTag, "dispatch loop exiting (queue empty, stopped)");
                    break;
                }
                lock.unlock();
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                continue;
            }
#else
            // Linux（部署平台）pthread 条件变量正常，用条件等待避免忙轮询
            cv_.wait_for(lock, std::chrono::milliseconds(500),
                         [this] { return !queue_.empty() || !running_; });
            if (queue_.empty() && !running_) {
                ALOG_DEBUG(kTag, "dispatch loop exiting (queue empty, stopped)");
                break;
            }
            if (queue_.empty()) {
                continue;  // 超时唤醒，无事件且仍运行
            }
#endif
            entry = std::move(const_cast<Entry&>(queue_.top()));
            queue_.pop();
        }
        // 回调不在锁内执行，避免回调内 Publish 死锁
        auto it = handlers_.find(entry.event.type);
        if (it != handlers_.end()) {
            for (const auto& h : it->second) {
                h(entry.event);
            }
        }
    }
}

}  // namespace aura
