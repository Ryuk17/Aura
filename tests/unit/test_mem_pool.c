/* mem_pool 单元测试：定长块池 */
#include "aura_test.h"

#include "utils/mem_pool/mem_pool.h"

static void test_alloc_free_cycle(void)
{
    aura_mem_pool_t *pool = aura_mem_pool_create(64, 8);
    AURA_ASSERT(pool != NULL);
    AURA_ASSERT_EQ(aura_mem_pool_total(pool), 8);
    AURA_ASSERT_EQ(aura_mem_pool_in_use(pool), 0);

    void *ptrs[8];
    for (int i = 0; i < 8; i++) {
        ptrs[i] = aura_mem_pool_alloc(pool, AURA_WAIT_FOREVER);
        AURA_ASSERT(ptrs[i] != NULL);
    }
    AURA_ASSERT_EQ(aura_mem_pool_in_use(pool), 8);
    AURA_ASSERT_EQ(aura_mem_pool_peak(pool), 8);

    /* 池空：非阻塞分配应失败 */
    AURA_ASSERT(aura_mem_pool_alloc(pool, AURA_NO_WAIT) == NULL);

    for (int i = 0; i < 8; i++) {
        AURA_ASSERT(aura_mem_pool_free(pool, ptrs[i]) == AURA_OK);
    }
    AURA_ASSERT_EQ(aura_mem_pool_in_use(pool), 0);

    /* 归还后可重新分配，且无泄漏（峰值不回退是预期语义） */
    void *again = aura_mem_pool_alloc(pool, AURA_NO_WAIT);
    AURA_ASSERT(again != NULL);
    AURA_ASSERT(aura_mem_pool_free(pool, again) == AURA_OK);
    aura_mem_pool_destroy(pool);
}

static void test_block_data_integrity(void)
{
    aura_mem_pool_t *pool = aura_mem_pool_create(128, 4);
    AURA_ASSERT(pool != NULL);
    /* 每块写满独特内容，全部归还后再取出来验证顺序复用 */
    uint8_t *blocks[4];
    for (int i = 0; i < 4; i++) {
        blocks[i] = (uint8_t *)aura_mem_pool_alloc(pool, AURA_NO_WAIT);
        AURA_ASSERT(blocks[i] != NULL);
        memset(blocks[i], (int)(0x10 + i), 128);
    }
    for (int i = 0; i < 4; i++) {
        AURA_ASSERT(aura_mem_pool_free(pool, blocks[i]) == AURA_OK);
    }
    /* 空闲链表 LIFO：最后归还的块（blocks[3]，内容 0x13）最先被取出。
     * 注意：前 sizeof(void*) 字节是空闲链表指针的载体，归还时被改写 ——
     * 只能校验偏移 sizeof(void*) 之后的内容。 */
    uint8_t *b = (uint8_t *)aura_mem_pool_alloc(pool, AURA_NO_WAIT);
    AURA_ASSERT(b != NULL);
    for (int j = (int)sizeof(void *); j < 128; j++) {
        AURA_ASSERT_EQ(b[j], 0x13);
    }
    aura_mem_pool_free(pool, b);
    aura_mem_pool_destroy(pool);
}

static void test_invalid_free_rejected(void)
{
    aura_mem_pool_t *pool = aura_mem_pool_create(64, 4);
    AURA_ASSERT(pool != NULL);
    /* 池外指针：必须拒绝 */
    int stack_val = 0;
    AURA_ASSERT(aura_mem_pool_free(pool, &stack_val) == AURA_ERR_INVALID_ARG);
    /* 块中段地址（非块首）：必须拒绝 */
    void *blk = aura_mem_pool_alloc(pool, AURA_NO_WAIT);
    AURA_ASSERT(blk != NULL);
    uint8_t *mid = (uint8_t *)blk + 16;
    AURA_ASSERT(aura_mem_pool_free(pool, mid) == AURA_ERR_INVALID_ARG);
    AURA_ASSERT(aura_mem_pool_free(pool, blk) == AURA_OK);
    aura_mem_pool_destroy(pool);
}

static void test_static_init(void)
{
    static uint8_t storage[4 * 64];
    static aura_mem_pool_t pool;
    AURA_ASSERT(aura_mem_pool_init_static(&pool, storage, 64, 4) != NULL);
    void *a = aura_mem_pool_alloc(&pool, AURA_NO_WAIT);
    void *b = aura_mem_pool_alloc(&pool, AURA_NO_WAIT);
    AURA_ASSERT(a != NULL && b != NULL && a != b);
    AURA_ASSERT(aura_mem_pool_free(&pool, a) == AURA_OK);
    AURA_ASSERT(aura_mem_pool_free(&pool, b) == AURA_OK);
    aura_mem_pool_destroy(&pool);
}

AURA_TEST(test_alloc_free_cycle);
AURA_TEST(test_block_data_integrity);
AURA_TEST(test_invalid_free_rejected);
AURA_TEST(test_static_init);

AURA_TEST_MAIN();
