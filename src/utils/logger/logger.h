/* Aura — 日志前端
 *
 * 分层：本文件负责**等级过滤 / 格式化 / 限速**，实际输出落到 osal 的
 * `aura_osal_console_write`（落到 stderr）。这样 core/utils 不直接依赖 printf。
 *
 * 限速（todo.md 4.1-7：防 flash 磨损 / 防日志淹没实时链路）：
 * 每个 tag 在 1 秒窗口内最多输出 AURA_LOG_RATE_LIMIT 条，超出部分被抑制并
 * 在窗口结束时补一条 `[suppressed N]` 汇总。音频热路径可安全打日志。
 *
 * 编译期裁剪：定义 AURA_LOG_COMPILE_LEVEL 可把低等级日志整个编译掉
 * （省体积与 CPU），默认保留全部等级。
 */
#ifndef AURA_UTILS_LOGGER_LOGGER_H
#define AURA_UTILS_LOGGER_LOGGER_H

#include <stdbool.h>
#include <stdint.h>

#include "osal/osal.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef AURA_LOG_COMPILE_LEVEL
#define AURA_LOG_COMPILE_LEVEL AURA_LOG_LVL_TRACE
#endif

/* 每个 tag 每秒最多输出的条数；0 表示不限速。 */
#ifndef AURA_LOG_RATE_LIMIT
#define AURA_LOG_RATE_LIMIT 64
#endif

/* 日志初始化：只在进程/应用启动时调用一次；未调用时首次打印会自动初始化。 */
void aura_log_init(void);
void aura_log_set_level(aura_log_level_t level);
aura_log_level_t aura_log_get_level(void);
bool aura_log_enabled(aura_log_level_t level);

/* 供宏展开调用的实现，不直接使用。 */
void aura_log_emit(aura_log_level_t level, const char *tag, const char *file, int line,
                   const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 5, 6)))
#endif
    ;

#define AURA_LOG_IMPL(lvl, tag, ...)                                              \
    do {                                                                          \
        if ((lvl) <= AURA_LOG_COMPILE_LEVEL && aura_log_enabled(lvl)) {           \
            aura_log_emit((lvl), (tag), __FILE__, __LINE__, __VA_ARGS__);         \
        }                                                                         \
    } while (0)

#define AURA_LOGE(tag, ...) AURA_LOG_IMPL(AURA_LOG_LVL_ERROR, tag, __VA_ARGS__)
#define AURA_LOGW(tag, ...) AURA_LOG_IMPL(AURA_LOG_LVL_WARN, tag, __VA_ARGS__)
#define AURA_LOGI(tag, ...) AURA_LOG_IMPL(AURA_LOG_LVL_INFO, tag, __VA_ARGS__)
#define AURA_LOGD(tag, ...) AURA_LOG_IMPL(AURA_LOG_LVL_DEBUG, tag, __VA_ARGS__)
#define AURA_LOGT(tag, ...) AURA_LOG_IMPL(AURA_LOG_LVL_TRACE, tag, __VA_ARGS__)

/* 日志限速累计被抑制的条数（测试与压测用）。 */
uint64_t aura_log_suppressed_count(void);

#ifdef __cplusplus
}
#endif

#endif /* AURA_UTILS_LOGGER_LOGGER_H */
