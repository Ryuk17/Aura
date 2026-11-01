/* test_zipformer_util — context_graph（AC 自动机）+ symbol_table 自检
 *
 * context_graph 的用例直接搬 sherpa-mnn context-graph-test.cc 的
 * TestBasic / TestCustomize（同词条、同分数规则、同期望值）—— 两边语义对齐的
 * 最硬证据就是同一组输入给同一组输出。再加一组"命中/元信息"用例：
 *   词条带 @名字 与逐词条阈值 → is_matched/level/threshold/phrase 全对得上。
 *
 * symbol_table 用临时 tokens.txt 验：双向查找、vocab_size 过滤、
 * keywords 行 tokenize（含 @名字）。
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aura_test.h"
#include "dsp/zipformer/context_graph.h"
#include "dsp/zipformer/symbol_table.h"

/* sherpa TestHelper：contexts = S/HE/SHE/SHELL/HIS/HERS/HELLO/THIS/THEM（字符码
 * 直接当 token id），scores 按 round(score/len*100)/100 逐词条铺。
 * 总分 = Σ 每步分数贡献 + Finalize（= -终态 node_score），与 sherpa 一致。 */
static float run_query(aura_context_graph_t *g, const char *query)
{
    int32_t state = aura_context_graph_root(g);
    float   total = 0.0f;
    for (const char *q = query; *q != '\0'; q++) {
        int32_t next = 0;
        total += aura_context_graph_forward(g, state, (int32_t)*q, &next, NULL);
        state = next;
    }
    total -= aura_context_graph_node_score(g, state);
    return total;
}

static aura_context_graph_t *make_sherpa_graph(float score)
{
    const char *words[] = {"S", "HE", "SHE", "SHELL", "HIS",
                           "HERS", "HELLO", "THIS", "THEM"};
    const int32_t n = (int32_t)(sizeof(words) / sizeof(words[0]));

    int32_t  ids[9][8];
    int32_t  lens[9];
    float    scores[9];
    const int32_t *id_ptrs[9];

    for (int32_t i = 0; i < n; i++) {
        int32_t len = (int32_t)strlen(words[i]);
        lens[i] = len;
        for (int32_t j = 0; j < len; j++) {
            ids[i][j] = (int32_t)words[i][j];
        }
        scores[i] = roundf(score / len * 100.0f) / 100.0f;
        id_ptrs[i] = ids[i];
    }
    return aura_context_graph_create(id_ptrs, lens, n, 1.0f, 0.0f, scores, NULL,
                                     NULL);
}

static void check_queries(const char *tag,
                          const char *const *queries, const float *expect,
                          int32_t n, float score)
{
    aura_context_graph_t *g = make_sherpa_graph(score);
    AURA_ASSERT(g != NULL);
    if (g == NULL) {
        return;
    }
    for (int32_t i = 0; i < n; i++) {
        float got = run_query(g, queries[i]);
        if (fabsf(got - expect[i]) > 0.01f) {
            printf("    %s[%s]: 期望 %.2f 实得 %.2f\n", tag, queries[i],
                   (double)expect[i], (double)got);
        }
        AURA_CHECK(fabsf(got - expect[i]) <= 0.01f);
    }
    aura_context_graph_destroy(g);
}

AURA_TEST(test_context_graph_basic)
{
    /* sherpa context-graph-test.cc TestBasic（score=0 → 每 token 分数 1） */
    const char *queries[] = {"HEHERSHE", "HERSHE", "HISHE", "SHED", "SHELF",
                             "HELL",     "HELLO",  "DHRHISQ", "THEN"};
    const float expect[] = {14, 12, 9, 6, 6, 2, 7, 4, 2};
    check_queries("basic", queries, expect, 9, 0.0f);
}

AURA_TEST(test_context_graph_customize)
{
    /* sherpa context-graph-test.cc TestCustomize（score=5） */
    const char *queries[] = {"HEHERSHE", "HERSHE", "HISHE", "SHED", "SHELF",
                             "HELL",     "HELLO",  "DHRHISQ", "THEN"};
    const float expect[] = {35.84f, 30.84f, 24.18f, 18.34f, 18.34f,
                            5.0f,   13.0f,  10.84f, 5.0f};
    check_queries("custom", queries, expect, 9, 5.0f);
}

AURA_TEST(test_context_graph_match_meta)
{
    /* 两个词条："AB"（阈值 0.5）与 "ABC"（阈值 0.9），带显示名。
     * 前缀关系：走到 C 时 AB 也是接受态 —— 验 is_matched/level/phrase/threshold。 */
    int32_t kw1[2] = {'A', 'B'};
    int32_t kw2[3] = {'A', 'B', 'C'};
    const int32_t *ids[2] = {kw1, kw2};
    int32_t lens[2] = {2, 3};
    const char *phrases[2] = {"词条AB", "词条ABC"};
    float ths[2] = {0.5f, 0.9f};

    aura_context_graph_t *g =
        aura_context_graph_create(ids, lens, 2, 1.0f, 0.25f, NULL, phrases, ths);
    AURA_ASSERT(g != NULL);
    if (g == NULL) {
        return;
    }

    int32_t s = aura_context_graph_root(g);
    int32_t m = -1;

    /* A：无命中 */
    (void)aura_context_graph_forward(g, s, 'A', &s, &m);
    AURA_CHECK(m == -1);
    AURA_CHECK(aura_context_graph_is_matched(g, s) == -1);

    /* A B：命中 AB */
    (void)aura_context_graph_forward(g, s, 'B', &s, &m);
    AURA_CHECK(m != -1);
    AURA_ASSERT_EQ(aura_context_graph_is_matched(g, s), m);
    AURA_ASSERT_EQ(aura_context_graph_level(g, m), 2);
    AURA_CHECK(fabsf(aura_context_graph_threshold(g, m) - 0.5f) < 1e-6f);
    AURA_CHECK(aura_context_graph_phrase(g, m) != NULL);
    AURA_ASSERT_STREQ(aura_context_graph_phrase(g, m), "词条AB");

    /* 继续 C：命中 ABC（output 弧：当前节点不是终态但经过 ABC） */
    (void)aura_context_graph_forward(g, s, 'C', &s, &m);
    AURA_CHECK(m != -1);
    AURA_ASSERT_EQ(aura_context_graph_level(g, m), 3);
    AURA_CHECK(fabsf(aura_context_graph_threshold(g, m) - 0.9f) < 1e-6f);
    AURA_ASSERT_STREQ(aura_context_graph_phrase(g, m), "词条ABC");

    /* 无关 token：回到根（KWS 借"回到根"重置假设历史） */
    (void)aura_context_graph_forward(g, s, 'Z', &s, &m);
    AURA_ASSERT_EQ(s, aura_context_graph_root(g));
    AURA_CHECK(m == -1);

    /* 从根起步的 fail 链：X A B 里的 AB 也能命中（fail 转移） */
    s = aura_context_graph_root(g);
    (void)aura_context_graph_forward(g, s, 'X', &s, &m);
    AURA_ASSERT_EQ(s, aura_context_graph_root(g));
    (void)aura_context_graph_forward(g, s, 'A', &s, &m);
    AURA_CHECK(m == -1);
    (void)aura_context_graph_forward(g, s, 'B', &s, &m);
    AURA_CHECK(m != -1);
    AURA_ASSERT_EQ(aura_context_graph_level(g, m), 2);

    aura_context_graph_destroy(g);
}

AURA_TEST(test_symbol_table)
{
    /* 临时 tokens.txt：含空白符条目（单独 id 行）、越界条目（id >= vocab）。 */
    const char *path = "tokens_tmp_test.txt";
    FILE *fp = fopen(path, "wb");
    AURA_ASSERT(fp != NULL);
    if (fp == NULL) {
        return;
    }
    fputs("<blk> 0\n<sos/eos> 1\n<unk> 2\nA 3\n5\nx 22\niǎo 23\n#0 197\n", fp);
    fclose(fp);

    aura_symbol_table_t *st = aura_symbol_table_create(path, 197);
    remove(path);
    AURA_ASSERT(st != NULL);
    if (st == NULL) {
        return;
    }

    /* 双向查找 */
    AURA_ASSERT_EQ(aura_symbol_table_lookup(st, "<blk>"), 0);
    AURA_ASSERT_EQ(aura_symbol_table_lookup(st, "A"), 3);
    AURA_ASSERT_EQ(aura_symbol_table_lookup(st, "iǎo"), 23);
    AURA_ASSERT_STREQ(aura_symbol_table_symbol(st, 1), "<sos/eos>");
    AURA_ASSERT_STREQ(aura_symbol_table_symbol(st, 5), " "); /* 单独 id 行 = 空格 */

    /* vocab_size 过滤：#0 (197) 越界不收录；不存在的符号返回 -1 */
    AURA_ASSERT_EQ(aura_symbol_table_lookup(st, "#0"), -1);
    AURA_ASSERT_EQ(aura_symbol_table_lookup(st, "zzz"), -1);
    AURA_CHECK(aura_symbol_table_symbol(st, 197) == NULL);

    /* keywords 行 tokenize（含 @名字 与无 @ 两种） */
    int32_t ids[16];
    char    name[64];
    int32_t n = aura_symbol_table_tokenize(st, "x iǎo A x iǎo A @小A小A", ids, 16,
                                           name, sizeof(name));
    AURA_ASSERT_EQ(n, 6);
    if (n == 6) {
        const int32_t expect[6] = {22, 23, 3, 22, 23, 3};
        for (int32_t i = 0; i < 6; i++) {
            AURA_ASSERT_EQ(ids[i], expect[i]);
        }
    }
    AURA_ASSERT_STREQ(name, "小A小A");

    n = aura_symbol_table_tokenize(st, "x iǎo A", ids, 16, name, sizeof(name));
    AURA_ASSERT_EQ(n, 3);
    AURA_ASSERT_STREQ(name, "");

    /* 未知符号 → -1（词条不可用） */
    AURA_ASSERT_EQ(aura_symbol_table_tokenize(st, "x zzz", ids, 16, NULL, 0), -1);

    aura_symbol_table_destroy(st);
}

AURA_TEST_MAIN()
