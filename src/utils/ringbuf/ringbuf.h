/* Aura — 环形缓冲（字节流）
 *
 * 定位：音频帧 / 文本 chunk 的搬运底座。刻意做成 SPSC（单生产者单消费者）、
 * 无锁、零 malloc —— 音频热路径上不允许出现锁竞争与堆分配。
 *
 * 使用约定：
 *   - 生产者线程只调 aura_ringbuf_write，消费者线程只调 aura_ringbuf_read；
 *     多生产者/多消费者需外层自行加锁。
 *   - 容量按 2 的幂向上取整（内部用掩码取模）。
 *   - 写满时不覆盖旧数据，返回实际写入字节数（调用方据此丢弃或告警）。
 */
#ifndef AURA_UTILS_RINGBUF_RINGBUF_H
#define AURA_UTILS_RINGBUF_RINGBUF_H

#include <stdbool.h>
#include <stdint.h>

#include "utils/error.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct aura_ringbuf {
    uint8_t *buf;
    uint32_t capacity;      /* 2 的幂 */
    uint32_t mask;
    volatile uint32_t head; /* 写序号（单调递增，不回绕到 0，溢出由无符号运算自然处理） */
    volatile uint32_t tail; /* 读序号 */
    bool     owns_buf;      /* destroy 时是否需要释放 buf */
    uint64_t total_written;
    uint64_t total_read;
    uint64_t total_dropped; /* 因空间不足被拒绝的字节数 */
} aura_ringbuf_t;

/* 调用方提供存储：用于静态分配场景（全程无 malloc）。capacity 会向上取整到 2 的幂，
 * 因此实际可用容量可能大于传入值（用 aura_ringbuf_capacity 查询）。
 * storage 必须按 8 字节对齐且不小于 capacity * 2（向上取整后）。 */
aura_err_t aura_ringbuf_init(aura_ringbuf_t *rb, void *storage, uint32_t capacity);

/* 内部 malloc 存储的便捷版本（仅限初始化阶段调用）。 */
aura_err_t aura_ringbuf_create(aura_ringbuf_t *rb, uint32_t capacity);

/* 释放 create 版本持有的存储；init 版本只清状态。 */
void aura_ringbuf_deinit(aura_ringbuf_t *rb);

/* 写入，返回实际写入字节数（< len 表示空间不足，未写入部分计入 dropped）。 */
uint32_t aura_ringbuf_write(aura_ringbuf_t *rb, const void *data, uint32_t len);

/* 读出，返回实际读出字节数。 */
uint32_t aura_ringbuf_read(aura_ringbuf_t *rb, void *out, uint32_t len);

/* 丢弃 len 字节（只移动读指针）。返回实际丢弃字节数。 */
uint32_t aura_ringbuf_skip(aura_ringbuf_t *rb, uint32_t len);

uint32_t aura_ringbuf_avail_read(const aura_ringbuf_t *rb);
uint32_t aura_ringbuf_avail_write(const aura_ringbuf_t *rb);
uint32_t aura_ringbuf_capacity(const aura_ringbuf_t *rb);
bool     aura_ringbuf_empty(const aura_ringbuf_t *rb);

/* 清空内容（保留存储）。生产者/消费者都不在读写时才可调用。 */
void aura_ringbuf_reset(aura_ringbuf_t *rb);

#ifdef __cplusplus
}
#endif

#endif /* AURA_UTILS_RINGBUF_RINGBUF_H */
