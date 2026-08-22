#include "log.h"

#include <cstdarg>
#include <ctime>
#include <cstring>

#ifdef _WIN32
#include <windows.h>
#endif

namespace aura {

const char* LogLevelName(LogLevel level) {
    switch (level) {
        case LogLevel::kTrace: return "TRACE";
        case LogLevel::kDebug: return "DEBUG";
        case LogLevel::kInfo:  return "INFO ";
        case LogLevel::kWarn:  return "WARN ";
        case LogLevel::kError: return "ERROR";
    }
    return "?????";
}

Log& Log::Instance() {
    static Log instance;
    return instance;
}

uint64_t Log::NowMs() {
#ifdef _WIN32
    return static_cast<uint64_t>(GetTickCount64());
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
#endif
}

void Log::SetLevel(LogLevel level) {
    std::lock_guard<std::mutex> lock(mutex_);
    level_ = level;
}

bool Log::SetLogFile(const std::string& path) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (file_) {
        fclose(file_);
        file_ = nullptr;
    }
    if (!path.empty()) {
        file_ = fopen(path.c_str(), "a");
        if (!file_) {
            return false;
        }
    }
    return true;
}

void Log::Write(LogLevel level, const char* tag, const char* file, int line,
                const char* fmt, ...) {
    char message[2048];
    va_list args;
    va_start(args, fmt);
    vsnprintf(message, sizeof(message), fmt, args);
    va_end(args);

    Record rec;
    rec.level = level;
    rec.timestamp_ms = NowMs();
    rec.tag = tag ? tag : "";
    rec.message = message;

    std::lock_guard<std::mutex> lock(mutex_);
    if (level < level_) {
        return;  // 低于输出级别，仅记录到环形缓冲
    }
    Emit(rec);

    if (file_) {
        // YYYY-MM-DD HH:MM:SS.mmm [LEVEL] [tag] (file:line) message
        fprintf(file_, "%s [%s] [%s] (%s:%d) %s\n",
                FormatTimestamp(rec.timestamp_ms).c_str(), LogLevelName(level), tag,
                file, line, message);
        fflush(file_);
    }
}

void Log::Emit(const Record& rec) {
    if (ring_.size() < ring_capacity_) {
        ring_.push_back(rec);
    } else {
        ring_[ring_head_] = rec;
        ring_head_ = (ring_head_ + 1) % ring_capacity_;
        ring_full_ = true;
    }

    // 输出到 stdout
    std::string ts = FormatTimestamp(rec.timestamp_ms);
    fprintf(stdout, "%s [%s] [%s] %s\n", ts.c_str(), LogLevelName(rec.level),
            rec.tag.c_str(), rec.message.c_str());
}

std::vector<Log::Record> Log::Recent(size_t max_count) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Record> out;
    out.reserve(max_count);
    size_t n = ring_full_ ? ring_capacity_ : ring_.size();
    size_t start = ring_full_ ? ring_head_ : 0;
    size_t count = (max_count == 0 || n < max_count) ? n : max_count;
    for (size_t i = 0; i < count; ++i) {
        out.push_back(ring_[(start + i) % ring_capacity_]);
    }
    return out;
}

std::string Log::FormatTimestamp(uint64_t ms) {
    // 格式：HH:MM:SS.mmm（相对日志内的可读性，使用本地时间）
    time_t secs = static_cast<time_t>(ms / 1000);
    struct tm tmv;
#ifdef _WIN32
    localtime_s(&tmv, &secs);
#else
    localtime_r(&secs, &tmv);
#endif
    char buf[32];
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d.%03u", tmv.tm_hour, tmv.tm_min,
             tmv.tm_sec, static_cast<unsigned>(ms % 1000));
    return std::string(buf);
}

}  // namespace aura
