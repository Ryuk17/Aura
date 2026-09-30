#include "utils/mem_pool/mem_pool.h"

#include <stdlib.h>
#include <string.h>

/* 空闲链表节点直接复用块自身的头部，不额外占内存。
 * 因此 block_size 必须 >= sizeof(void*) 且块起始地址 8 字节对齐。 */
#define AURA_MEM_POOL_ALIGN 8u

static uint32_t align_up(uint32_t v, uint32_t a)
{
    return (v + a - 1u) & ~(a - 1u);
}

uint32_t aura_mem_pool_storage_size(uint32_t block_size, uint32_t block_count)
{
    if (block_size < sizeof(void *)) {
        block_size = (uint32_t)sizeof(void *);
    }
    return align_up(block_size, AURA_MEM_POOL_ALIGN) * block_count;
}

aura_mem_pool_t *aura_mem_pool_init_static(aura_mem_pool_t *pool, void *storage,
                                           uint32_t block_size, uint32_t block_count)
{
    if (pool == NULL || storage == NULL || block_count == 0) {
        return NULL;
    }
    if (block_size < sizeof(void *)) {
        block_size = (uint32_t)sizeof(void *);
    }
    block_size = align_up(block_size, AURA_MEM_POOL_ALIGN);
    if (((uintptr_t)storage % AURA_MEM_POOL_ALIGN) != 0) {
        return NULL;
    }

    memset(pool, 0, sizeof(*pool));
    pool->storage     = (uint8_t *)storage;
    pool->block_size  = block_size;
    pool->block_count = block_count;
    pool->owns_storage = false;

    /* 串空闲链表：全部块依次入链。 */
    free_node_t *head = NULL;
    for (uint32_t i = block_count; i > 0; i--) {
        free_node_t *n = (free_node_t *)(pool->storage + (size_t)(i - 1) * block_size);
        n->next  = head;
        head     = n;
    }
    pool->free_list = head;

    pool->lock = aura_osal_mutex_create();
    pool->avail_sem = aura_osal_sem_create(block_count, block_count);
    if (pool->lock == NULL || pool->avail_sem == NULL) {
        if (pool->lock != NULL) {
            aura_osal_mutex_destroy(pool->lock);
        }
        if (pool->avail_sem != NULL) {
            aura_osal_sem_destroy(pool->avail_sem);
        }
        return NULL;
    }
    return pool;
}

aura_mem_pool_t *aura_mem_pool_create(uint32_t block_size, uint32_t block_count)
{
    uint32_t bytes = aura_mem_pool_storage_size(block_size, block_count);
    if (bytes == 0) {
        return NULL;
    }
    aura_mem_pool_t *pool = (aura_mem_pool_t *)malloc(sizeof(*pool));
    if (pool == NULL) {
        return NULL;
    }
    void *storage = malloc(bytes);
    if (storage == NULL) {
        free(pool);
        return NULL;
    }
    if (aura_mem_pool_init_static(pool, storage, block_size, block_count) == NULL) {
        free(storage);
        free(pool);
        return NULL;
    }
    pool->owns_storage = true;
    return pool;
}

void aura_mem_pool_destroy(aura_mem_pool_t *pool)
{
    if (pool == NULL) {
        return;
    }
    if (pool->in_use != 0) {
        /* 有块未归还：说明上层生命周期管理有 bug，这里只告警不越权释放。 */
        aura_osal_console_write("[mem_pool] destroy with blocks in use\n", 38);
    }
    aura_osal_sem_destroy(pool->avail_sem);
    aura_osal_mutex_destroy(pool->lock);
    if (pool->owns_storage) {
        free(pool->storage);
        free(pool);
    } else {
        memset(pool, 0, sizeof(*pool));
    }
}

void *aura_mem_pool_alloc(aura_mem_pool_t *pool, uint32_t timeout_ms)
{
    if (pool == NULL) {
        return NULL;
    }
    if (aura_osal_sem_wait(pool->avail_sem, timeout_ms) != AURA_OK) {
        return NULL;
    }
    aura_osal_mutex_lock(pool->lock);
    free_node_t *n = pool->free_list;
    if (n == NULL) {
        /* 信号量计数与链表不一致：只可能由误用（如重复 free）导致。 */
        aura_osal_mutex_unlock(pool->lock);
        return NULL;
    }
    pool->free_list = n->next;
    pool->in_use++;
    if (pool->in_use > pool->peak) {
        pool->peak = pool->in_use;
    }
    aura_osal_mutex_unlock(pool->lock);
    return (void *)n;
}

aura_err_t aura_mem_pool_free(aura_mem_pool_t *pool, void *ptr)
{
    if (pool == NULL || ptr == NULL) {
        return AURA_ERR_INVALID_ARG;
    }
    uint8_t *p = (uint8_t *)ptr;
    if (p < pool->storage || p >= pool->storage + (size_t)pool->block_size * pool->block_count) {
        return AURA_ERR_INVALID_ARG;
    }
    if (((size_t)(p - pool->storage) % pool->block_size) != 0) {
        return AURA_ERR_INVALID_ARG; /* 非块首地址 */
    }

    aura_osal_mutex_lock(pool->lock);
    free_node_t *n = (free_node_t *)ptr;
    n->next         = pool->free_list;
    pool->free_list = n;
    if (pool->in_use > 0) {
        pool->in_use--;
    }
    aura_osal_mutex_unlock(pool->lock);

    aura_osal_sem_post(pool->avail_sem);
    return AURA_OK;
}

uint32_t aura_mem_pool_block_size(const aura_mem_pool_t *pool)
{
    return (pool == NULL) ? 0 : pool->block_size;
}

uint32_t aura_mem_pool_total(const aura_mem_pool_t *pool)
{
    return (pool == NULL) ? 0 : pool->block_count;
}

uint32_t aura_mem_pool_in_use(const aura_mem_pool_t *pool)
{
    if (pool == NULL) {
        return 0;
    }
    aura_mem_pool_t *m = (aura_mem_pool_t *)pool;
    aura_osal_mutex_lock(m->lock);
    uint32_t v = m->in_use;
    aura_osal_mutex_unlock(m->lock);
    return v;
}

uint32_t aura_mem_pool_available(const aura_mem_pool_t *pool)
{
    return (pool == NULL) ? 0 : (pool->block_count - aura_mem_pool_in_use(pool));
}

uint32_t aura_mem_pool_peak(const aura_mem_pool_t *pool)
{
    return (pool == NULL) ? 0 : pool->peak;
}

/* ------------------------------------------------------------- 全局帧池 */

static aura_mem_pool_t *g_pool = NULL;

aura_err_t aura_mem_pool_global_init(uint32_t block_size, uint32_t block_count)
{
    if (g_pool != NULL) {
        return AURA_ERR_EXIST;
    }
    g_pool = aura_mem_pool_create(block_size, block_count);
    return (g_pool == NULL) ? AURA_ERR_NOMEM : AURA_OK;
}

aura_mem_pool_t *aura_mem_pool_global(void)
{
    return g_pool;
}

void aura_mem_pool_global_deinit(void)
{
    if (g_pool != NULL) {
        aura_mem_pool_destroy(g_pool);
        g_pool = NULL;
    }
}
