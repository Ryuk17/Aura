/* OSAL 单元测试：任务/队列/互斥/信号量/定时器/时间 */
#include "aura_test.h"

#include "osal/osal.h"

/* ---- 时间 ---- */

static void test_time_monotonic(void)
{
    uint64_t t0 = aura_osal_time_us();
    aura_test_sleep_ms(20);
    uint64_t t1 = aura_osal_time_us();
    AURA_CHECK(t1 > t0);
    AURA_CHECK(t1 - t0 >= 10000); /* 至少 10ms（20ms 睡眠允许调度误差） */
}

/* ---- 互斥 ---- */

static int  g_counter = 0;
static aura_mutex_t *g_counter_mutex = NULL;

static void counter_task(void *arg)
{
    (void)arg;
    for (int i = 0; i < 1000; i++) {
        aura_osal_mutex_lock(g_counter_mutex);
        g_counter++;
        aura_osal_mutex_unlock(g_counter_mutex);
    }
}

static void test_mutex_contention(void)
{
    g_counter_mutex = aura_osal_mutex_create();
    AURA_ASSERT(g_counter_mutex != NULL);
    aura_task_t *tasks[4];
    for (int i = 0; i < 4; i++) {
        tasks[i] = aura_osal_task_create("cnt", 8192, 3, counter_task, NULL);
        AURA_ASSERT(tasks[i] != NULL);
    }
    for (int i = 0; i < 4; i++) {
        AURA_ASSERT(aura_osal_task_join(tasks[i], 5000) == AURA_OK);
        aura_osal_task_destroy(tasks[i]);
    }
    AURA_ASSERT_EQ(g_counter, 4000);
    aura_osal_mutex_destroy(g_counter_mutex);
}

/* ---- 队列 ---- */

static void test_queue_fifo(void)
{
    aura_queue_t *q = aura_osal_queue_create(8, sizeof(int32_t));
    AURA_ASSERT(q != NULL);
    for (int32_t i = 0; i < 8; i++) {
        AURA_ASSERT(aura_osal_queue_push(q, &i, AURA_NO_WAIT) == AURA_OK);
    }
    /* 满了：非阻塞应返回 FULL */
    int32_t v = 99;
    AURA_ASSERT(aura_osal_queue_push(q, &v, AURA_NO_WAIT) == AURA_ERR_FULL);
    for (int32_t i = 0; i < 8; i++) {
        AURA_ASSERT(aura_osal_queue_pop(q, &v, AURA_NO_WAIT) == AURA_OK);
        AURA_ASSERT_EQ(v, i);
    }
    /* 空了：非阻塞应返回 EMPTY */
    AURA_ASSERT(aura_osal_queue_pop(q, &v, AURA_NO_WAIT) == AURA_ERR_EMPTY);
    /* 超时等待 */
    uint64_t t0 = aura_osal_time_us();
    AURA_ASSERT(aura_osal_queue_pop(q, &v, 30) == AURA_ERR_TIMEOUT);
    AURA_CHECK(aura_osal_time_us() - t0 >= 20000);
    AURA_ASSERT(aura_osal_queue_capacity(q) == 8);
    aura_osal_queue_destroy(q);
}

static aura_queue_t *g_blocking_q = NULL;

static void producer_task(void *arg)
{
    (void)arg;
    aura_test_sleep_ms(50);
    int32_t v = 42;
    aura_osal_queue_push(g_blocking_q, &v, AURA_WAIT_FOREVER);
}

static void test_queue_blocking(void)
{
    g_blocking_q = aura_osal_queue_create(1, sizeof(int32_t));
    AURA_ASSERT(g_blocking_q != NULL);
    aura_task_t *t = aura_osal_task_create("prod", 8192, 3, producer_task, NULL);
    AURA_ASSERT(t != NULL);
    int32_t v = 0;
    /* 阻塞等待 500ms，生产者 50ms 后入队 */
    AURA_ASSERT(aura_osal_queue_pop(g_blocking_q, &v, 500) == AURA_OK);
    AURA_ASSERT_EQ(v, 42);
    aura_osal_task_destroy(t);
    aura_osal_queue_destroy(g_blocking_q);
}

/* ---- 信号量 ---- */

static aura_sem_t *g_sem = NULL;

static void sem_poster(void *arg)
{
    (void)arg;
    aura_test_sleep_ms(50);
    aura_osal_sem_post(g_sem);
}

static void test_semaphore(void)
{
    g_sem = aura_osal_sem_create(0, 1);
    AURA_ASSERT(g_sem != NULL);
    aura_task_t *t = aura_osal_task_create("post", 8192, 3, sem_poster, NULL);
    AURA_ASSERT(t != NULL);
    AURA_ASSERT(aura_osal_sem_wait(g_sem, 500) == AURA_OK);
    aura_osal_task_destroy(t);
    /* 计数满后 post 应拒绝 */
    AURA_ASSERT(aura_osal_sem_post(g_sem) == AURA_OK);
    AURA_ASSERT(aura_osal_sem_post(g_sem) == AURA_ERR_FULL);
    aura_osal_sem_destroy(g_sem);
}

/* ---- 任务 ---- */

static void quick_task(void *arg)
{
    int *flag = (int *)arg;
    *flag = 1;
}

static void test_task_join(void)
{
    int flag = 0;
    aura_task_t *t = aura_osal_task_create("quick", 8192, 3, quick_task, &flag);
    AURA_ASSERT(t != NULL);
    AURA_ASSERT(aura_osal_task_join(t, 1000) == AURA_OK);
    AURA_ASSERT_EQ(flag, 1);
    AURA_CHECK(aura_osal_task_self() != aura_osal_task_id(t));
    aura_osal_task_destroy(t);
}

/* ---- 定时器 ---- */

static volatile int g_timer_fired = 0;

static void timer_cb(void *arg)
{
    (void)arg;
    g_timer_fired++;
}

static void test_timer(void)
{
    aura_timer_t *tm = aura_osal_timer_create("t1", 30, true, timer_cb, NULL);
    AURA_ASSERT(tm != NULL);
    AURA_ASSERT(aura_osal_timer_start(tm) == AURA_OK);
    aura_test_sleep_ms(120);
    AURA_ASSERT(aura_osal_timer_stop(tm) == AURA_OK);
    AURA_CHECK(g_timer_fired >= 2); /* 120ms / 30ms 至少触发 2 次 */
    aura_osal_timer_destroy(tm);
}

AURA_TEST(test_time_monotonic);
AURA_TEST(test_mutex_contention);
AURA_TEST(test_queue_fifo);
AURA_TEST(test_queue_blocking);
AURA_TEST(test_semaphore);
AURA_TEST(test_task_join);
AURA_TEST(test_timer);

AURA_TEST_MAIN();
