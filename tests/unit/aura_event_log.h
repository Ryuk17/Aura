/* Aura — 测试用事件收集器（header-only，随 aura_test.h 一起用）
 *
 * 干什么：**先订阅、后触发、再等计数**。测试要断言"喂这段音频会产生 KWS_HIT"，
 * 唯一稳妥的顺序是先挂上收集器，再喂数据，最后等收集到的条数到齐。
 *
 * 为什么不用 aura_event_bus_wait()：
 * 它是**水平触发**的 —— 登记之前发布的事件不补发。而节点任务与喂数据的
 * 测试线程是并发的，"先喂数据再 wait"必然抢时序：机器快一点（或被调度到
 * 同一核）时，节点在 wait 登记之前就把边沿事件发完了，wait 于是等满超时。
 * 这个 flake 在 test_dsp_adapter/test_nn_adapter 上实测约 40% 复现率，
 * 且**不会**因为改对了被测代码而消失 —— 它测的是测试自己的时序运气。
 *
 * 收集器把顺序固定下来：aura_event_log_subscribe() 在触发之前完成注册，
 * 之后 aura_event_log_wait() 只等"已经攒够几条"，与谁先谁后无关。
 *
 * 用法：
 *     aura_event_log_t log;
 *     aura_event_log_init(&log, aura_pipeline_bus(p), mask);   // 先订阅
 *     feed_pts(p, 3, 0);                                       // 后触发
 *     AURA_ASSERT_EQ(aura_event_log_wait(&log, 1, 2000), AURA_OK);
 *     AURA_ASSERT_EQ(log.ev[0].type, AURA_EVENT_KWS_HIT);
 *     aura_event_log_fini(&log);
 *
 * 注意：回调在**总线分发任务**里跑，ev/count 会被那个线程写 —— 所以
 * count 只增不减、wait 用信号量同步，读 ev[i] 必须在 wait 成功之后
 * （wait 成功即"那条已经写完"）。
 */
#ifndef AURA_TESTS_UNIT_AURA_EVENT_LOG_H
#define AURA_TESTS_UNIT_AURA_EVENT_LOG_H

#include <string.h>

#include "core/event_bus/event_bus.h"
#include "osal/osal.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef AURA_EVENT_LOG_MAX
#define AURA_EVENT_LOG_MAX 64
#endif

typedef struct aura_event_log {
    aura_event_t      ev[AURA_EVENT_LOG_MAX];
    volatile uint32_t count;   /* 已收集条数（只增） */
    uint32_t          dropped; /* 超出容量丢掉的条数 —— 非 0 说明断言依据不全 */
    aura_event_bus_t *bus;
    aura_sem_t       *sem;
    const char       *name;    /* 出错信息里用，可 NULL */
} aura_event_log_t;

static void aura_event_log_cb(const aura_event_t *event, void *user)
{
    aura_event_log_t *log = (aura_event_log_t *)user;
    uint32_t          i   = log->count;
    if (i < AURA_EVENT_LOG_MAX) {
        log->ev[i] = *event;
    } else {
        log->dropped++;
    }
    log->count = i + 1;
    /* 每次投一次信号量：wait(min) 循环等 count >= min 就够了，
     * 多投没害处（信号量上限 1 时 post 会失败，忽略即可）。 */
    (void)aura_osal_sem_post(log->sem);
}

/* 订阅 mask 内的事件。必须在触发（喂数据/发事件）**之前**调用。 */
static inline aura_err_t aura_event_log_init(aura_event_log_t *log, aura_event_bus_t *bus,
                                             uint32_t type_mask)
{
    memset(log, 0, sizeof(*log));
    log->bus = bus;
    log->sem = aura_osal_sem_create(0, 1);
    if (log->sem == NULL) {
        return AURA_ERR_NOMEM;
    }
    return aura_event_bus_subscribe(bus, type_mask, aura_event_log_cb, log);
}

static inline void aura_event_log_fini(aura_event_log_t *log)
{
    if (log->bus != NULL) {
        (void)aura_event_bus_unsubscribe(log->bus, aura_event_log_cb);
    }
    if (log->sem != NULL) {
        aura_osal_sem_destroy(log->sem);
        log->sem = NULL;
    }
}

/* 等到至少 min 条匹配事件。返回 AURA_ERR_TIMEOUT 表示超时（事件没来）。 */
static inline aura_err_t aura_event_log_wait(aura_event_log_t *log, uint32_t min,
                                             uint32_t timeout_ms)
{
    uint64_t deadline = aura_osal_time_ms() + timeout_ms;
    while (log->count < min) {
        int64_t left = (int64_t)deadline - (int64_t)aura_osal_time_ms();
        if (left <= 0) {
            return AURA_ERR_TIMEOUT;
        }
        /* 超时只用来"醒来看一眼 count"：信号量可能被前一次投递留下计数，
         * 也可能这次 post 时上限已满而失败，所以以 count 为准而不是以返回值。 */
        (void)aura_osal_sem_wait(log->sem, (uint32_t)left);
    }
    return AURA_OK;
}

/* 找第 n 个（从 0 起）指定类型的事件；没有则返回 NULL。 */
static inline const aura_event_t *aura_event_log_find(const aura_event_log_t *log, int type,
                                                      uint32_t nth)
{
    uint32_t seen = 0;
    for (uint32_t i = 0; i < log->count && i < AURA_EVENT_LOG_MAX; i++) {
        if ((int)log->ev[i].type == type) {
            if (seen == nth) {
                return &log->ev[i];
            }
            seen++;
        }
    }
    return NULL;
}

/* 指定类型的事件出现了几次。 */
static inline uint32_t aura_event_log_count(const aura_event_log_t *log, int type)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < log->count && i < AURA_EVENT_LOG_MAX; i++) {
        if ((int)log->ev[i].type == type) {
            n++;
        }
    }
    return n;
}

#ifdef __cplusplus
}
#endif

#endif /* AURA_TESTS_UNIT_AURA_EVENT_LOG_H */
