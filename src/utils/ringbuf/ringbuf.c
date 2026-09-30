#include "utils/ringbuf/ringbuf.h"

#include <stdlib.h>
#include <string.h>

/* SPSC 内存序：先写数据、再发布 head；读侧先读 head 的 acquire 语义再读数据。
 * 在 x86 上编译器屏障已足够；在 ARM（树莓派）上需要真正的屏障指令，
 * 故统一使用 __atomic_thread_fence（GCC/Clang 支持，与平台无关）。 */
#define AURA_SMP_FENCE() __atomic_thread_fence(__ATOMIC_SEQ_CST)

static uint32_t round_up_pow2(uint32_t v)
{
    if (v < 2) {
        return 2;
    }
    v--;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
    return v + 1;
}

aura_err_t aura_ringbuf_init(aura_ringbuf_t *rb, void *storage, uint32_t capacity)
{
    if (rb == NULL || storage == NULL || capacity == 0) {
        return AURA_ERR_INVALID_ARG;
    }
    uint32_t cap = round_up_pow2(capacity);
    memset(rb, 0, sizeof(*rb));
    rb->buf      = (uint8_t *)storage;
    rb->capacity = cap;
    rb->mask     = cap - 1;
    rb->owns_buf = false;
    return AURA_OK;
}

aura_err_t aura_ringbuf_create(aura_ringbuf_t *rb, uint32_t capacity)
{
    if (rb == NULL || capacity == 0) {
        return AURA_ERR_INVALID_ARG;
    }
    uint32_t cap = round_up_pow2(capacity);
    uint8_t *buf = (uint8_t *)malloc(cap);
    if (buf == NULL) {
        return AURA_ERR_NOMEM;
    }
    aura_err_t rc = aura_ringbuf_init(rb, buf, cap);
    if (rc != AURA_OK) {
        free(buf);
        return rc;
    }
    rb->owns_buf = true;
    return AURA_OK;
}

void aura_ringbuf_deinit(aura_ringbuf_t *rb)
{
    if (rb == NULL) {
        return;
    }
    if (rb->owns_buf && rb->buf != NULL) {
        free(rb->buf);
    }
    memset(rb, 0, sizeof(*rb));
}

uint32_t aura_ringbuf_write(aura_ringbuf_t *rb, const void *data, uint32_t len)
{
    if (rb == NULL || data == NULL || len == 0) {
        return 0;
    }
    const uint32_t head  = rb->head;
    const uint32_t tail  = rb->tail;
    const uint32_t used  = head - tail;
    uint32_t space       = rb->capacity - used;
    if (len > space) {
        rb->total_dropped += (uint64_t)(len - space);
        len = space;
    }
    if (len == 0) {
        return 0;
    }
    const uint32_t idx   = head & rb->mask;
    const uint32_t first = rb->capacity - idx; /* 到缓冲区尾部的连续空间 */
    const uint8_t *src   = (const uint8_t *)data;
    if (len <= first) {
        memcpy(rb->buf + idx, src, len);
    } else {
        memcpy(rb->buf + idx, src, first);
        memcpy(rb->buf, src + first, len - first);
    }
    AURA_SMP_FENCE();
    rb->head = head + len;
    rb->total_written += len;
    return len;
}

uint32_t aura_ringbuf_read(aura_ringbuf_t *rb, void *out, uint32_t len)
{
    if (rb == NULL || out == NULL || len == 0) {
        return 0;
    }
    const uint32_t head = rb->head;
    const uint32_t tail = rb->tail;
    uint32_t avail      = head - tail;
    if (len > avail) {
        len = avail;
    }
    if (len == 0) {
        return 0;
    }
    AURA_SMP_FENCE();
    const uint32_t idx   = tail & rb->mask;
    const uint32_t first = rb->capacity - idx;
    uint8_t *dst         = (uint8_t *)out;
    if (len <= first) {
        memcpy(dst, rb->buf + idx, len);
    } else {
        memcpy(dst, rb->buf + idx, first);
        memcpy(dst + first, rb->buf, len - first);
    }
    rb->tail = tail + len;
    rb->total_read += len;
    return len;
}

uint32_t aura_ringbuf_skip(aura_ringbuf_t *rb, uint32_t len)
{
    if (rb == NULL || len == 0) {
        return 0;
    }
    uint32_t avail = rb->head - rb->tail;
    if (len > avail) {
        len = avail;
    }
    rb->tail = rb->tail + len;
    rb->total_read += len;
    return len;
}

uint32_t aura_ringbuf_avail_read(const aura_ringbuf_t *rb)
{
    return (rb == NULL) ? 0 : (rb->head - rb->tail);
}

uint32_t aura_ringbuf_avail_write(const aura_ringbuf_t *rb)
{
    return (rb == NULL) ? 0 : (rb->capacity - (rb->head - rb->tail));
}

uint32_t aura_ringbuf_capacity(const aura_ringbuf_t *rb)
{
    return (rb == NULL) ? 0 : rb->capacity;
}

bool aura_ringbuf_empty(const aura_ringbuf_t *rb)
{
    return aura_ringbuf_avail_read(rb) == 0;
}

void aura_ringbuf_reset(aura_ringbuf_t *rb)
{
    if (rb == NULL) {
        return;
    }
    rb->head = 0;
    rb->tail = 0;
}
