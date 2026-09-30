/* Aura — 打点与延迟profiler
 *
 * 设计动机（todo.md 4.1-4）：首响延迟与首包延迟是最核心产品指标，
 * 必须从第一天就能打点。Phase 1 起每个关键节点都要埋点，Phase 4/5 才可能优化。
 *
 * 用法：
 *   aura_profiler_begin_session();       // 一次交互开始时（收到音频/唤醒前）
 *   aura_profiler_mark(AURA_PROF_KWS_HIT);
 *   ...
 *   aura_profiler_report();              // 打印本轮的阶段耗时
 *
 * 打点本身只做一次时间读取 + 一次数组写入，可在音频线程调用。
 */
#ifndef AURA_UTILS_PROFILER_PROFILER_H
#define AURA_UTILS_PROFILER_PROFILER_H

#include <stdbool.h>
#include <stdint.h>

#include "utils/error.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    AURA_PROF_IDLE_WINDOW = 0,     /* 会话起点（KWS 之前的音频起点） */
    AURA_PROF_KWS_HIT,             /* 唤醒词命中 */
    AURA_PROF_VOICEPRINT_DONE,     /* 声纹校验完成 */
    AURA_PROF_ASR_FIRST_PARTIAL,   /* ASR 首个中间结果 */
    AURA_PROF_ASR_FINAL,           /* ASR 最终结果（送入 LLM） */
    AURA_PROF_LLM_FIRST_TOKEN,     /* LLM 首 token */
    AURA_PROF_LLM_DONE,            /* LLM 生成结束 */
    AURA_PROF_TTS_FIRST_CHUNK,     /* TTS 首个音频包 */
    AURA_PROF_PLAYBACK_START,      /* 实际出声 */
    AURA_PROF_BARGE_IN,            /* 有效打断判定 */
    AURA_PROF_PLAYBACK_STOP,       /* 打断后停播 */
    AURA_PROF_ERROR,
    AURA_PROF_MARK_COUNT
} aura_prof_mark_t;

/* 一轮交互的打点快照。时间基为 osal 单调毫秒。 */
typedef struct {
    uint64_t ts_ms[AURA_PROF_MARK_COUNT];
    bool     valid[AURA_PROF_MARK_COUNT];
} aura_prof_session_t;

void aura_profiler_init(void);
void aura_profiler_reset(void);

/* 开始新一轮：清零并记录 IDLE_WINDOW。 */
void aura_profiler_begin_session(void);
void aura_profiler_mark(aura_prof_mark_t mark);

/* 拷贝当前快照（线程安全；用于测试断言与上报）。 */
aura_err_t aura_profiler_snapshot(aura_prof_session_t *out);

/* 两个打点之间的耗时（ms）。任一无效返回 0。 */
uint64_t aura_prof_snapshot_delta_ms(const aura_prof_session_t *s, aura_prof_mark_t from,
                                     aura_prof_mark_t to);

/* 打印关键指标（对照 todo.md 第 6 节验收指标）。 */
void aura_profiler_report(void);

/* 供日志/上报使用的可读名称。 */
const char *aura_prof_mark_name(aura_prof_mark_t mark);

#ifdef __cplusplus
}
#endif

#endif /* AURA_UTILS_PROFILER_PROFILER_H */
