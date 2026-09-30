/* Aura — OSAL POSIX 后端
 *
 * 同时服务两类 PC 目标：
 *   - Linux（gcc/clang）
 *   - Windows（MinGW-w64 + winpthreads，需 -lpthread）
 * 只用 POSIX.1-2008 的 pthread/sem 子集，两者行为一致；不用任何平台私有 API。
 *
 * 所有等待都用 pthread_cond_timedwait 的绝对超时（CLOCK_REALTIME），
 * 与 winpthreads / glibc 语义一致。
 */

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "osal/osal.h"

/* ------------------------------------------------------------------ 时间 */

uint64_t aura_osal_time_us(void)
{
    struct timespec ts;
#if defined(CLOCK_MONOTONIC)
    clock_gettime(CLOCK_MONOTONIC, &ts);
#else
    clock_gettime(CLOCK_REALTIME, &ts);
#endif
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)(ts.tv_nsec / 1000);
}

uint64_t aura_osal_time_ms(void)
{
    return aura_osal_time_us() / 1000ull;
}

void aura_osal_sleep_ms(uint32_t ms)
{
    if (ms == 0) {
        sched_yield();
        return;
    }
    struct timespec ts;
    ts.tv_sec  = (time_t)(ms / 1000u);
    ts.tv_nsec = (long)(ms % 1000u) * 1000000L;
    /* 被信号打断时按剩余时间续睡。 */
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR) {
    }
}

/* --------------------------------------------------------------- 内部工具 */

/* 把相对超时换算成 cond 等待所需的绝对时间。 */
static void abs_timeout(uint32_t timeout_ms, struct timespec *ts)
{
    clock_gettime(CLOCK_REALTIME, ts);
    ts->tv_sec += (time_t)(timeout_ms / 1000u);
    ts->tv_nsec += (long)(timeout_ms % 1000u) * 1000000L;
    if (ts->tv_nsec >= 1000000000L) {
        ts->tv_sec += 1;
        ts->tv_nsec -= 1000000000L;
    }
}

/* --------------------------------------------------------------- 日志底座 */

static pthread_mutex_t g_console_lock = PTHREAD_MUTEX_INITIALIZER;

void aura_osal_console_write(const char *str, size_t len)
{
    if (str == NULL || len == 0) {
        return;
    }
    pthread_mutex_lock(&g_console_lock);
    fwrite(str, 1, len, stderr);
    fflush(stderr);
    pthread_mutex_unlock(&g_console_lock);
}

/* ------------------------------------------------------------------ 互斥 */

struct aura_mutex {
    pthread_mutex_t handle;
};

aura_mutex_t *aura_osal_mutex_create(void)
{
    aura_mutex_t *m = (aura_mutex_t *)malloc(sizeof(*m));
    if (m == NULL) {
        return NULL;
    }
    if (pthread_mutex_init(&m->handle, NULL) != 0) {
        free(m);
        return NULL;
    }
    return m;
}

void aura_osal_mutex_destroy(aura_mutex_t *m)
{
    if (m == NULL) {
        return;
    }
    pthread_mutex_destroy(&m->handle);
    free(m);
}

aura_err_t aura_osal_mutex_lock(aura_mutex_t *m)
{
    if (m == NULL) {
        return AURA_ERR_INVALID_ARG;
    }
    return (pthread_mutex_lock(&m->handle) == 0) ? AURA_OK : AURA_ERR_FAIL;
}

aura_err_t aura_osal_mutex_unlock(aura_mutex_t *m)
{
    if (m == NULL) {
        return AURA_ERR_INVALID_ARG;
    }
    return (pthread_mutex_unlock(&m->handle) == 0) ? AURA_OK : AURA_ERR_FAIL;
}

/* ---------------------------------------------------------------- 信号量 */

struct aura_sem {
    pthread_mutex_t lock;
    pthread_cond_t  cond;
    uint32_t        count;
    uint32_t        max_count;
};

aura_sem_t *aura_osal_sem_create(uint32_t initial, uint32_t max_count)
{
    if (max_count == 0 || initial > max_count) {
        return NULL;
    }
    aura_sem_t *s = (aura_sem_t *)malloc(sizeof(*s));
    if (s == NULL) {
        return NULL;
    }
    if (pthread_mutex_init(&s->lock, NULL) != 0) {
        free(s);
        return NULL;
    }
    if (pthread_cond_init(&s->cond, NULL) != 0) {
        pthread_mutex_destroy(&s->lock);
        free(s);
        return NULL;
    }
    s->count     = initial;
    s->max_count = max_count;
    return s;
}

void aura_osal_sem_destroy(aura_sem_t *s)
{
    if (s == NULL) {
        return;
    }
    pthread_cond_destroy(&s->cond);
    pthread_mutex_destroy(&s->lock);
    free(s);
}

aura_err_t aura_osal_sem_post(aura_sem_t *s)
{
    if (s == NULL) {
        return AURA_ERR_INVALID_ARG;
    }
    pthread_mutex_lock(&s->lock);
    if (s->count >= s->max_count) {
        pthread_mutex_unlock(&s->lock);
        return AURA_ERR_FULL;
    }
    s->count++;
    pthread_cond_signal(&s->cond);
    pthread_mutex_unlock(&s->lock);
    return AURA_OK;
}

aura_err_t aura_osal_sem_wait(aura_sem_t *s, uint32_t timeout_ms)
{
    if (s == NULL) {
        return AURA_ERR_INVALID_ARG;
    }
    pthread_mutex_lock(&s->lock);
    while (s->count == 0) {
        if (timeout_ms == AURA_NO_WAIT) {
            pthread_mutex_unlock(&s->lock);
            return AURA_ERR_TIMEOUT;
        }
        if (timeout_ms == AURA_WAIT_FOREVER) {
            pthread_cond_wait(&s->cond, &s->lock);
            continue;
        }
        struct timespec ts;
        abs_timeout(timeout_ms, &ts);
        int rc = pthread_cond_timedwait(&s->cond, &s->lock, &ts);
        if (rc == ETIMEDOUT && s->count == 0) {
            pthread_mutex_unlock(&s->lock);
            return AURA_ERR_TIMEOUT;
        }
    }
    s->count--;
    pthread_mutex_unlock(&s->lock);
    return AURA_OK;
}

uint32_t aura_osal_sem_count(const aura_sem_t *s)
{
    if (s == NULL) {
        return 0;
    }
    aura_sem_t *m = (aura_sem_t *)s; /* 只为加锁，逻辑上 const */
    pthread_mutex_lock(&m->lock);
    uint32_t c = m->count;
    pthread_mutex_unlock(&m->lock);
    return c;
}

/* ------------------------------------------------------------------ 队列 */

struct aura_queue {
    uint8_t        *buf;
    uint32_t        capacity;
    uint32_t        item_size;
    uint32_t        head;
    uint32_t        count;
    pthread_mutex_t lock;
    pthread_cond_t  not_empty;
    pthread_cond_t  not_full;
};

aura_queue_t *aura_osal_queue_create(uint32_t capacity, uint32_t item_size)
{
    if (capacity == 0 || item_size == 0) {
        return NULL;
    }
    aura_queue_t *q = (aura_queue_t *)calloc(1, sizeof(*q));
    if (q == NULL) {
        return NULL;
    }
    q->buf = (uint8_t *)malloc((size_t)capacity * item_size);
    if (q->buf == NULL) {
        free(q);
        return NULL;
    }
    q->capacity  = capacity;
    q->item_size = item_size;
    if (pthread_mutex_init(&q->lock, NULL) != 0) {
        free(q->buf);
        free(q);
        return NULL;
    }
    pthread_cond_init(&q->not_empty, NULL);
    pthread_cond_init(&q->not_full, NULL);
    return q;
}

void aura_osal_queue_destroy(aura_queue_t *q)
{
    if (q == NULL) {
        return;
    }
    pthread_cond_destroy(&q->not_full);
    pthread_cond_destroy(&q->not_empty);
    pthread_mutex_destroy(&q->lock);
    free(q->buf);
    free(q);
}

aura_err_t aura_osal_queue_push(aura_queue_t *q, const void *item, uint32_t timeout_ms)
{
    if (q == NULL || item == NULL) {
        return AURA_ERR_INVALID_ARG;
    }
    pthread_mutex_lock(&q->lock);
    while (q->count == q->capacity) {
        if (timeout_ms == AURA_NO_WAIT) {
            pthread_mutex_unlock(&q->lock);
            return AURA_ERR_FULL;
        }
        if (timeout_ms == AURA_WAIT_FOREVER) {
            pthread_cond_wait(&q->not_full, &q->lock);
            continue;
        }
        struct timespec ts;
        abs_timeout(timeout_ms, &ts);
        int rc = pthread_cond_timedwait(&q->not_full, &q->lock, &ts);
        if (rc == ETIMEDOUT && q->count == q->capacity) {
            pthread_mutex_unlock(&q->lock);
            return AURA_ERR_TIMEOUT;
        }
    }
    uint32_t slot = (q->head + q->count) % q->capacity;
    memcpy(q->buf + (size_t)slot * q->item_size, item, q->item_size);
    q->count++;
    pthread_cond_signal(&q->not_empty);
    pthread_mutex_unlock(&q->lock);
    return AURA_OK;
}

aura_err_t aura_osal_queue_pop(aura_queue_t *q, void *item, uint32_t timeout_ms)
{
    if (q == NULL || item == NULL) {
        return AURA_ERR_INVALID_ARG;
    }
    pthread_mutex_lock(&q->lock);
    while (q->count == 0) {
        if (timeout_ms == AURA_NO_WAIT) {
            pthread_mutex_unlock(&q->lock);
            return AURA_ERR_EMPTY;
        }
        if (timeout_ms == AURA_WAIT_FOREVER) {
            pthread_cond_wait(&q->not_empty, &q->lock);
            continue;
        }
        struct timespec ts;
        abs_timeout(timeout_ms, &ts);
        int rc = pthread_cond_timedwait(&q->not_empty, &q->lock, &ts);
        if (rc == ETIMEDOUT && q->count == 0) {
            pthread_mutex_unlock(&q->lock);
            return AURA_ERR_TIMEOUT;
        }
    }
    memcpy(item, q->buf + (size_t)q->head * q->item_size, q->item_size);
    q->head = (q->head + 1) % q->capacity;
    q->count--;
    pthread_cond_signal(&q->not_full);
    pthread_mutex_unlock(&q->lock);
    return AURA_OK;
}

aura_err_t aura_osal_queue_peek(aura_queue_t *q, void *item)
{
    if (q == NULL || item == NULL) {
        return AURA_ERR_INVALID_ARG;
    }
    pthread_mutex_lock(&q->lock);
    if (q->count == 0) {
        pthread_mutex_unlock(&q->lock);
        return AURA_ERR_EMPTY;
    }
    memcpy(item, q->buf + (size_t)q->head * q->item_size, q->item_size);
    pthread_mutex_unlock(&q->lock);
    return AURA_OK;
}

uint32_t aura_osal_queue_count(const aura_queue_t *q)
{
    if (q == NULL) {
        return 0;
    }
    aura_queue_t *m = (aura_queue_t *)q;
    pthread_mutex_lock(&m->lock);
    uint32_t c = m->count;
    pthread_mutex_unlock(&m->lock);
    return c;
}

uint32_t aura_osal_queue_capacity(const aura_queue_t *q)
{
    return (q == NULL) ? 0 : q->capacity;
}

bool aura_osal_queue_empty(const aura_queue_t *q)
{
    return aura_osal_queue_count(q) == 0;
}

void aura_osal_queue_reset(aura_queue_t *q)
{
    if (q == NULL) {
        return;
    }
    pthread_mutex_lock(&q->lock);
    q->head  = 0;
    q->count = 0;
    pthread_cond_broadcast(&q->not_full);
    pthread_mutex_unlock(&q->lock);
}

/* ------------------------------------------------------------ 任务 / 线程 */

struct aura_task {
    pthread_t      tid;
    aura_sem_t    *done;   /* 线程退出前 post，用于带超时的 join */
    aura_task_fn_t fn;
    void          *arg;
    char           name[16];
    volatile bool  exited; /* 线程函数已返回（trampoline 置位） */
    bool           joined; /* pthread_join 是否已执行 */
};

static void *task_trampoline(void *p)
{
    aura_task_t *t = (aura_task_t *)p;
    t->fn(t->arg);
    t->exited = true;
    aura_osal_sem_post(t->done);
    return NULL;
}

aura_task_t *aura_osal_task_create(const char *name, uint32_t stack_bytes, int priority,
                                   aura_task_fn_t fn, void *arg)
{
    (void)stack_bytes;
    (void)priority;
    if (fn == NULL) {
        return NULL;
    }
    aura_task_t *t = (aura_task_t *)calloc(1, sizeof(*t));
    if (t == NULL) {
        return NULL;
    }
    t->fn  = fn;
    t->arg = arg;
    t->done = aura_osal_sem_create(0, 1);
    if (t->done == NULL) {
        free(t);
        return NULL;
    }
    if (name != NULL) {
        snprintf(t->name, sizeof(t->name), "%s", name);
    }
    if (pthread_create(&t->tid, NULL, task_trampoline, t) != 0) {
        aura_osal_sem_destroy(t->done);
        free(t);
        return NULL;
    }
    return t;
}

aura_err_t aura_osal_task_join(aura_task_t *t, uint32_t timeout_ms)
{
    if (t == NULL) {
        return AURA_ERR_INVALID_ARG;
    }
    /* done 信号量只被 trampoline post 一次、只允许一个消费者 —— join 是唯一入口；
     * destroy 内部不再重复消费（否则 join 后 destroy 会永久等待）。 */
    aura_err_t rc = aura_osal_sem_wait(t->done, timeout_ms);
    if (rc != AURA_OK) {
        return rc;
    }
    pthread_join(t->tid, NULL);
    t->joined = true;
    return AURA_OK;
}

void aura_osal_task_destroy(aura_task_t *t)
{
    if (t == NULL) {
        return;
    }
    /* 未 join 的任务在此处补 join，避免泄漏线程句柄。
     * 已 join 的直接释放（done 已被 join 消费，不能再等）。 */
    if (!t->joined && aura_osal_sem_wait(t->done, AURA_WAIT_FOREVER) == AURA_OK) {
        pthread_join(t->tid, NULL);
        t->joined = true;
    }
    aura_osal_sem_destroy(t->done);
    free(t);
}

aura_task_id_t aura_osal_task_self(void)
{
    return (aura_task_id_t)(uintptr_t)pthread_self();
}

aura_task_id_t aura_osal_task_id(const aura_task_t *t)
{
    return (t == NULL) ? 0 : (aura_task_id_t)(uintptr_t)t->tid;
}

/* ------------------------------------------------------------------ 定时器 */

struct aura_timer {
    pthread_t        tid;
    pthread_mutex_t  lock;
    pthread_cond_t   wake;
    aura_timer_fn_t  fn;
    void            *arg;
    uint32_t         period_ms;
    bool             periodic;
    bool             running;
    bool             stop_req;
    char             name[16];
};

static void *timer_trampoline(void *p)
{
    aura_timer_t *t = (aura_timer_t *)p;
    pthread_mutex_lock(&t->lock);
    while (!t->stop_req) {
        if (!t->running) {
            pthread_cond_wait(&t->wake, &t->lock);
            continue;
        }
        uint32_t period = t->period_ms;
        struct timespec ts;
        abs_timeout(period, &ts);
        pthread_cond_timedwait(&t->wake, &t->lock, &ts);
        if (t->stop_req || !t->running) {
            continue;
        }
        aura_timer_fn_t fn = t->fn;
        void *arg          = t->arg;
        if (!t->periodic) {
            t->running = false;
        }
        pthread_mutex_unlock(&t->lock);
        fn(arg); /* 回调在锁外执行，允许回调内 stop/start */
        pthread_mutex_lock(&t->lock);
    }
    pthread_mutex_unlock(&t->lock);
    return NULL;
}

aura_timer_t *aura_osal_timer_create(const char *name, uint32_t period_ms, bool periodic,
                                     aura_timer_fn_t fn, void *arg)
{
    if (fn == NULL || period_ms == 0) {
        return NULL;
    }
    aura_timer_t *t = (aura_timer_t *)calloc(1, sizeof(*t));
    if (t == NULL) {
        return NULL;
    }
    t->fn        = fn;
    t->arg       = arg;
    t->period_ms = period_ms;
    t->periodic  = periodic;
    if (name != NULL) {
        snprintf(t->name, sizeof(t->name), "%s", name);
    }
    pthread_mutex_init(&t->lock, NULL);
    pthread_cond_init(&t->wake, NULL);
    if (pthread_create(&t->tid, NULL, timer_trampoline, t) != 0) {
        pthread_cond_destroy(&t->wake);
        pthread_mutex_destroy(&t->lock);
        free(t);
        return NULL;
    }
    return t;
}

aura_err_t aura_osal_timer_start(aura_timer_t *t)
{
    if (t == NULL) {
        return AURA_ERR_INVALID_ARG;
    }
    pthread_mutex_lock(&t->lock);
    t->running = true;
    pthread_cond_signal(&t->wake);
    pthread_mutex_unlock(&t->lock);
    return AURA_OK;
}

aura_err_t aura_osal_timer_stop(aura_timer_t *t)
{
    if (t == NULL) {
        return AURA_ERR_INVALID_ARG;
    }
    pthread_mutex_lock(&t->lock);
    t->running = false;
    pthread_cond_signal(&t->wake);
    pthread_mutex_unlock(&t->lock);
    return AURA_OK;
}

void aura_osal_timer_destroy(aura_timer_t *t)
{
    if (t == NULL) {
        return;
    }
    pthread_mutex_lock(&t->lock);
    t->stop_req = true;
    t->running  = false;
    pthread_cond_signal(&t->wake);
    pthread_mutex_unlock(&t->lock);
    pthread_join(t->tid, NULL);
    pthread_cond_destroy(&t->wake);
    pthread_mutex_destroy(&t->lock);
    free(t);
}
