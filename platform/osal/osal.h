/* Aura — OS 抽象层（OSAL）
 *
 * 唯一目的：把 OS 的 task/queue/mutex/sem/timer/时间 收敛成一套固定接口，
 * 隔离上层与本机线程 API。除本文件与 osal/ 下的实现外，其余代码不得直接
 * include pthread.h / windows.h。
 *
 * 后端：只有 posix 一个 —— Linux 与 Windows(MinGW-w64 + winpthreads) 共用，
 * PC 开发与仿真都跑它。不做 RTOS 后端（已明确不移植 FreeRTOS）。
 *
 * 约束：本层只做原语封装，不含任何业务语义。
 */
#ifndef AURA_PLATFORM_OSAL_OSAL_H
#define AURA_PLATFORM_OSAL_OSAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "utils/error.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 等待超时取值 */
#define AURA_WAIT_FOREVER 0xFFFFFFFFu
#define AURA_NO_WAIT      0u

/* ------------------------------------------------------------------ 时间 */

/* 单调时钟，微秒。起点未指定，仅保证单调递增（回绕周期 > 数百年）。 */
uint64_t aura_osal_time_us(void);

/* 单调时钟，毫秒（= time_us/1000 的便捷封装）。 */
uint64_t aura_osal_time_ms(void);

/* 挂起当前任务/线程至少 ms 毫秒。ms=0 表示主动让出 CPU。 */
void aura_osal_sleep_ms(uint32_t ms);

/* --------------------------------------------------------------- 日志底座 */

typedef enum {
    AURA_LOG_LVL_ERROR = 0,
    AURA_LOG_LVL_WARN  = 1,
    AURA_LOG_LVL_INFO  = 2,
    AURA_LOG_LVL_DEBUG = 3,
    AURA_LOG_LVL_TRACE = 4,
    AURA_LOG_LVL_NONE  = 5,
} aura_log_level_t;

/* 原始输出通道（落到 stderr）。不做格式化、不做限速，
 * 格式化与限速属于 utils/logger。本接口线程安全、可在中断外的任意上下文调用。 */
void aura_osal_console_write(const char *str, size_t len);

/* ------------------------------------------------------------------ 互斥 */

typedef struct aura_mutex aura_mutex_t;

aura_mutex_t *aura_osal_mutex_create(void);
void          aura_osal_mutex_destroy(aura_mutex_t *m);

/* 返回 AURA_OK / AURA_ERR_INVALID_ARG。均为非递归锁。 */
aura_err_t aura_osal_mutex_lock(aura_mutex_t *m);
aura_err_t aura_osal_mutex_unlock(aura_mutex_t *m);

/* ------------------------------------------------------------------ 信号量 */

typedef struct aura_sem aura_sem_t;

/* initial: 初始计数；max_count: 上限（post 超过上限返回 AURA_ERR_FULL）。 */
aura_sem_t *aura_osal_sem_create(uint32_t initial, uint32_t max_count);
void        aura_osal_sem_destroy(aura_sem_t *s);

aura_err_t aura_osal_sem_post(aura_sem_t *s);
aura_err_t aura_osal_sem_wait(aura_sem_t *s, uint32_t timeout_ms);
uint32_t   aura_osal_sem_count(const aura_sem_t *s);

/* -------------------------------------------------------------------- 队列
 *
 * 固定容量、定长元素的 FIFO。元素按值拷贝（item_size 字节），
 * 因此入队/出队的对象必须是 POD。
 *
 * 内存纪律：队列存储与内部对象在 create 时一次性分配（初始化阶段），
 * 运行期 push/pop 不产生任何 malloc/free —— 音频热路径依赖此保证。
 */
typedef struct aura_queue aura_queue_t;

aura_queue_t *aura_osal_queue_create(uint32_t capacity, uint32_t item_size);
void          aura_osal_queue_destroy(aura_queue_t *q);

/* 入队；队列满时最多等待 timeout_ms（AURA_WAIT_FOREVER 为永久等待）。 */
aura_err_t aura_osal_queue_push(aura_queue_t *q, const void *item, uint32_t timeout_ms);

/* 出队；队列空时最多等待 timeout_ms。 */
aura_err_t aura_osal_queue_pop(aura_queue_t *q, void *item, uint32_t timeout_ms);

/* 只看队首不出队（非阻塞）。 */
aura_err_t aura_osal_queue_peek(aura_queue_t *q, void *item);

uint32_t aura_osal_queue_count(const aura_queue_t *q);
uint32_t aura_osal_queue_capacity(const aura_queue_t *q);
bool     aura_osal_queue_empty(const aura_queue_t *q);

/* 清空队列（不释放存储）。 */
void aura_osal_queue_reset(aura_queue_t *q);

/* ------------------------------------------------------------ 任务 / 线程 */

typedef void (*aura_task_fn_t)(void *arg);

/* 任务标识。POSIX 下即 pthread_self() 的值，故用 64 位承载，不做截断。 */
typedef uint64_t aura_task_id_t;

typedef struct aura_task aura_task_t;

/* stack_bytes：当前忽略（用系统默认栈）；保留该参数是为了将来真需要控栈时
 * 不必改动所有调用点。
 * priority：当前忽略（不做实时调度）。
 * 返回 NULL 表示创建失败。创建即开始运行。 */
aura_task_t *aura_osal_task_create(const char *name, uint32_t stack_bytes, int priority,
                                   aura_task_fn_t fn, void *arg);

/* 等待任务结束。任务自身若已结束则立即返回。 */
aura_err_t aura_osal_task_join(aura_task_t *t, uint32_t timeout_ms);

/* 回收任务对象。任务必须已结束（先 join 或 detached）。 */
void aura_osal_task_destroy(aura_task_t *t);

/* 当前任务 ID（用于断言/归属判定，如"回调不得在音频线程执行"）。 */
aura_task_id_t aura_osal_task_self(void);

/* 指定任务的 ID。 */
aura_task_id_t aura_osal_task_id(const aura_task_t *t);

/* ------------------------------------------------------------------ 定时器 */

typedef void (*aura_timer_fn_t)(void *arg);
typedef struct aura_timer aura_timer_t;

/* 回调在定时器内部线程上下文执行 —— 回调内禁止长阻塞。 */
aura_timer_t *aura_osal_timer_create(const char *name, uint32_t period_ms, bool periodic,
                                     aura_timer_fn_t fn, void *arg);
aura_err_t aura_osal_timer_start(aura_timer_t *t);
aura_err_t aura_osal_timer_stop(aura_timer_t *t);
void       aura_osal_timer_destroy(aura_timer_t *t);

#ifdef __cplusplus
}
#endif

#endif /* AURA_PLATFORM_OSAL_OSAL_H */
