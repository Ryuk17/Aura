// 日志模块：分级输出 + 线程安全 + 环形缓冲（保留最近 N 条供查询）
// 用法：ALOG_INFO("event_bus", "published %zu events", n);
#pragma once

#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

namespace aura {

enum class LogLevel : uint8_t {
    kTrace = 0,
    kDebug,
    kInfo,
    kWarn,
    kError,
};

const char* LogLevelName(LogLevel level);

class Log {
public:
    // 环形缓冲中的一条记录
    struct Record {
        LogLevel level;
        uint64_t timestamp_ms;
        std::string tag;
        std::string message;
    };

    static Log& Instance();

    // level: 低于该级别的不输出（默认 kDebug）
    void SetLevel(LogLevel level);
    LogLevel level() const { return level_; }

    // 是否同时写入文件（默认关闭）
    bool SetLogFile(const std::string& path);

    void Write(LogLevel level, const char* tag, const char* file, int line,
               const char* fmt, ...) __attribute__((format(printf, 6, 7)));

    // 查询环形缓冲中的最近记录（按时间正序）
    std::vector<Record> Recent(size_t max_count) const;

    static uint64_t NowMs();

private:
    Log() = default;
    Log(const Log&) = delete;
    Log& operator=(const Log&) = delete;

    void Emit(const Record& rec);
    static std::string FormatTimestamp(uint64_t ms);

    LogLevel level_ = LogLevel::kDebug;
    mutable std::mutex mutex_;
    std::vector<Record> ring_;   // 环形缓冲，容量 ring_capacity_
    size_t ring_capacity_ = 256;
    size_t ring_head_ = 0;       // 下一个写入位置
    bool ring_full_ = false;
    FILE* file_ = nullptr;
};

}  // namespace aura

#define ALOG_TRACE(tag, ...) \
    aura::Log::Instance().Write(aura::LogLevel::kTrace, tag, __FILE__, __LINE__, __VA_ARGS__)
#define ALOG_DEBUG(tag, ...) \
    aura::Log::Instance().Write(aura::LogLevel::kDebug, tag, __FILE__, __LINE__, __VA_ARGS__)
#define ALOG_INFO(tag, ...) \
    aura::Log::Instance().Write(aura::LogLevel::kInfo, tag, __FILE__, __LINE__, __VA_ARGS__)
#define ALOG_WARN(tag, ...) \
    aura::Log::Instance().Write(aura::LogLevel::kWarn, tag, __FILE__, __LINE__, __VA_ARGS__)
#define ALOG_ERROR(tag, ...) \
    aura::Log::Instance().Write(aura::LogLevel::kError, tag, __FILE__, __LINE__, __VA_ARGS__)
