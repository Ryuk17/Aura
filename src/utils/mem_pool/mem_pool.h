/* Aura — 定长块内存池
 *
 * 目的（todo.md 4.4）：运行期**禁止 malloc/free**，音频帧、事件、文本 chunk
 * 等所有高频对象的存储都从内存池取还。
 *
 * 实现：块内嵌空闲链表（无额外元数据开销），分配/释放为 O(1)；
 * 线程安全（互斥量 + 计数信号量），支持带超时的阻塞分配 —— 池空时分配者等待
 * 其他线程归还，而不是失败退出。
 *
 * 内存占用 = block_size * block_count（对齐后），init 后不再变化。
 */
#ifndef AURA_UTILS_MEM_POOL_MEM_POOL_H
#define AURA_UTILS_MEM_POOL_MEM_POOL_H

#include <stdbool.h>
#include <stdint.h>

#include "osal/osal.h"
#include "utils/error.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct free_node {
    struct free_node *next;
} free_node_t;

/* 结构体定义公开（静态实例化需要完整类型），但字段视为私有：
 * 除 aura_mem_pool_init_static 外，调用方不得直接读写任何字段。 */
typedef struct aura_mem_pool {
    uint8_t      *storage;      /* 块区起始 */
    uint32_t      block_size;   /* 对齐后 */
    uint32_t      block_count;
    uint32_t      in_use;
    uint32_t      peak;
    free_node_t  *free_list;
    aura_mutex_t *lock;
    aura_sem_t   *avail_sem;    /* 可用块计数，用于阻塞分配 */
    bool          owns_storage;
} aura_mem_pool_t;

/* 调用方提供存储（静态分配，全程无 malloc）。storage 需 8 字节对齐，
 * 大小 >= aura_mem_pool_storage_size(block_size, block_count)。 */
aura_mem_pool_t *aura_mem_pool_init_static(aura_mem_pool_t *pool, void *storage,
                                           uint32_t block_size, uint32_t block_count);

/* 自持存储版本；内部一次性分配，运行期不再分配。 */
aura_mem_pool_t *aura_mem_pool_create(uint32_t block_size, uint32_t block_count);

void aura_mem_pool_destroy(aura_mem_pool_t *pool);

/* 取一块。无可用块时最多等待 timeout_ms（AURA_WAIT_FOREVER 为永久等待）。
 * 返回的块内容未清零。 */
void *aura_mem_pool_alloc(aura_mem_pool_t *pool, uint32_t timeout_ms);

/* 归还。ptr 必须来自本池，否则行为未定义（Debug 下断言）。 */
aura_err_t aura_mem_pool_free(aura_mem_pool_t *pool, void *ptr);

uint32_t aura_mem_pool_block_size(const aura_mem_pool_t *pool);
uint32_t aura_mem_pool_total(const aura_mem_pool_t *pool);
uint32_t aura_mem_pool_in_use(const aura_mem_pool_t *pool);
uint32_t aura_mem_pool_available(const aura_mem_pool_t *pool);

/* 峰值占用块数，用于内存账复核（memory_budget.md）。 */
uint32_t aura_mem_pool_peak(const aura_mem_pool_t *pool);

/* 所需存储字节数（含对齐）。 */
uint32_t aura_mem_pool_storage_size(uint32_t block_size, uint32_t block_count);

/* ------------------------------------------------------------- 全局帧池
 * pipeline 的音频帧缓冲与事件对象统一走全局池，避免每个 node 各建一个池
 * 造成碎片（todo.md 4.4：MNN MemoryPool 全局统一、多模型复用 —— 框架侧同理）。
 */
aura_err_t aura_mem_pool_global_init(uint32_t block_size, uint32_t block_count);
aura_mem_pool_t *aura_mem_pool_global(void);
void aura_mem_pool_global_deinit(void);

#ifdef __cplusplus
}
#endif

#endif /* AURA_UTILS_MEM_POOL_MEM_POOL_H */
