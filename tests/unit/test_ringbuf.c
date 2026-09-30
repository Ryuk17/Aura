/* ringbuf 单元测试：SPSC 字节环 */
#include "aura_test.h"

#include "utils/ringbuf/ringbuf.h"

static void test_basic_write_read(void)
{
    aura_ringbuf_t rb;
    AURA_ASSERT(aura_ringbuf_create(&rb, 100) == AURA_OK);
    AURA_ASSERT_EQ(aura_ringbuf_capacity(&rb), 128); /* 2 的幂向上取整 */

    uint8_t in[64];
    uint8_t out[64];
    for (int i = 0; i < 64; i++) {
        in[i] = (uint8_t)i;
    }
    AURA_ASSERT_EQ(aura_ringbuf_write(&rb, in, 64), 64);
    AURA_ASSERT_EQ(aura_ringbuf_avail_read(&rb), 64);
    AURA_ASSERT_EQ(aura_ringbuf_read(&rb, out, 64), 64);
    AURA_ASSERT(memcmp(in, out, 64) == 0);
    AURA_ASSERT(aura_ringbuf_empty(&rb));
    aura_ringbuf_deinit(&rb);
}

static void test_wraparound(void)
{
    aura_ringbuf_t rb;
    AURA_ASSERT(aura_ringbuf_create(&rb, 16) == AURA_OK);
    uint8_t in[32];
    uint8_t out[32];
    for (int i = 0; i < 32; i++) {
        in[i] = (uint8_t)(i * 7);
    }
    /* 交替读写制造环绕：写 12 / 读 8 / 写 12（越过边界）… */
    AURA_ASSERT_EQ(aura_ringbuf_write(&rb, in, 12), 12);
    AURA_ASSERT_EQ(aura_ringbuf_read(&rb, out, 8), 8);
    AURA_ASSERT_EQ(aura_ringbuf_write(&rb, in + 12, 12), 12);
    AURA_ASSERT_EQ(aura_ringbuf_avail_read(&rb), 16);
    AURA_ASSERT_EQ(aura_ringbuf_read(&rb, out, 16), 16);
    AURA_ASSERT(memcmp(out, in + 8, 16) == 0);
    aura_ringbuf_deinit(&rb);
}

static void test_overwrite_policy(void)
{
    aura_ringbuf_t rb;
    AURA_ASSERT(aura_ringbuf_create(&rb, 16) == AURA_OK);
    uint8_t in[32];
    memset(in, 0xAB, sizeof(in));
    /* 写满 16 后继续写：只写进能放下的，其余计入 dropped（不覆盖旧数据）。 */
    AURA_ASSERT_EQ(aura_ringbuf_write(&rb, in, 16), 16);
    AURA_ASSERT_EQ(aura_ringbuf_write(&rb, in, 16), 0);
    AURA_ASSERT_EQ(aura_ringbuf_avail_read(&rb), 16);
    AURA_ASSERT_EQ(rb.total_dropped, 16);
    uint8_t out[32];
    AURA_ASSERT_EQ(aura_ringbuf_read(&rb, out, 32), 16);
    for (int i = 0; i < 16; i++) {
        AURA_ASSERT_EQ(out[i], 0xAB);
    }
    aura_ringbuf_deinit(&rb);
}

static void test_skip_and_reset(void)
{
    aura_ringbuf_t rb;
    AURA_ASSERT(aura_ringbuf_create(&rb, 32) == AURA_OK);
    uint8_t in[32];
    for (int i = 0; i < 32; i++) {
        in[i] = (uint8_t)i;
    }
    AURA_ASSERT_EQ(aura_ringbuf_write(&rb, in, 32), 32);
    AURA_ASSERT_EQ(aura_ringbuf_skip(&rb, 10), 10);
    AURA_ASSERT_EQ(aura_ringbuf_avail_read(&rb), 22);
    uint8_t out[32];
    AURA_ASSERT_EQ(aura_ringbuf_read(&rb, out, 32), 22);
    AURA_ASSERT(memcmp(out, in + 10, 22) == 0);
    aura_ringbuf_reset(&rb);
    AURA_ASSERT(aura_ringbuf_empty(&rb));
    AURA_ASSERT_EQ(aura_ringbuf_write(&rb, in, 32), 32);
    aura_ringbuf_deinit(&rb);
}

static void test_static_storage(void)
{
    /* 静态分配：调用方提供存储 */
    static uint8_t storage[64];
    aura_ringbuf_t rb;
    AURA_ASSERT(aura_ringbuf_init(&rb, storage, 48) == AURA_OK);
    AURA_ASSERT_EQ(aura_ringbuf_capacity(&rb), 64);
    uint8_t in[64];
    memset(in, 0x5A, sizeof(in));
    AURA_ASSERT_EQ(aura_ringbuf_write(&rb, in, 64), 64);
    uint8_t out[64];
    AURA_ASSERT_EQ(aura_ringbuf_read(&rb, out, 64), 64);
    AURA_ASSERT(memcmp(in, out, 64) == 0);
    aura_ringbuf_deinit(&rb); /* 不释放 storage */
}

AURA_TEST(test_basic_write_read);
AURA_TEST(test_wraparound);
AURA_TEST(test_overwrite_policy);
AURA_TEST(test_skip_and_reset);
AURA_TEST(test_static_storage);

AURA_TEST_MAIN();
