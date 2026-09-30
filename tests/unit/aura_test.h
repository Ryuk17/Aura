/* Aura — 极简单测框架（零依赖，C11）
 *
 * 不引入 gtest 等外部依赖的理由：构建要保持轻量，单测能脱离 CMake 单独编译。
 * 测试注册用 __attribute__((constructor))，GCC/Clang 通用。
 *
 * 用法：
 *   #include "aura_test.h"
 *   static void test_foo(void) { AURA_ASSERT(1 + 1 == 2); }
 *   AURA_TEST(test_foo);
 */
#ifndef AURA_TESTS_UNIT_AURA_TEST_H
#define AURA_TESTS_UNIT_AURA_TEST_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "osal/osal.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AURA_TEST_MAX_TESTS 256
#define AURA_TEST_MAX_FAILS 64

typedef void (*aura_test_fn)(void);

typedef struct {
    const char   *name;
    aura_test_fn  fn;
} aura_test_case_t;

static aura_test_case_t  g_test_cases[AURA_TEST_MAX_TESTS];
static int               g_test_count = 0;

static int               g_current_fails = 0;
static int               g_total_fails   = 0;
static int               g_total_checks  = 0;

/* 注册一个测试（用宏，别直接调）。 */
static void aura_test_register(const char *name, aura_test_fn fn)
{
    if (g_test_count < AURA_TEST_MAX_TESTS) {
        g_test_cases[g_test_count].name = name;
        g_test_cases[g_test_count].fn   = fn;
        g_test_count++;
    }
}

/* 用 __LINE__ 作 constructor 优先级：保证同一文件内**按声明顺序**注册执行
 * （GCC 同优先级 constructor 的顺序不做保证，导致依赖顺序的测试随机化）。 */
#define AURA_TEST(fn)                                                     \
    static void fn(void);                                                 \
    static void fn##_registrar(void) __attribute__((constructor(__LINE__))); \
    static void fn##_registrar(void) { aura_test_register(#fn, fn); }     \
    static void fn(void)

#define AURA_CHECK(cond)                                                   \
    do {                                                                   \
        g_total_checks++;                                                  \
        if (!(cond)) {                                                     \
            g_current_fails++;                                             \
            g_total_fails++;                                               \
            printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
        }                                                                  \
    } while (0)

/* 与 CHECK 同义，读起来更自然。 */
#define AURA_ASSERT(cond) AURA_CHECK(cond)

#define AURA_ASSERT_EQ(a, b)                                                     \
    do {                                                                         \
        g_total_checks++;                                                        \
        long long _a = (long long)(a), _b = (long long)(b);                      \
        if (_a != _b) {                                                          \
            g_current_fails++;                                                   \
            g_total_fails++;                                                     \
            printf("    FAIL %s:%d: %s == %s  (%lld != %lld)\n", __FILE__,       \
                   __LINE__, #a, #b, _a, _b);                                    \
        }                                                                        \
    } while (0)

#define AURA_ASSERT_STREQ(a, b)                                                  \
    do {                                                                         \
        g_total_checks++;                                                        \
        const char *_a = (a), *_b = (b);                                         \
        if (_a == NULL || _b == NULL || strcmp(_a, _b) != 0) {                   \
            g_current_fails++;                                                   \
            g_total_fails++;                                                     \
            printf("    FAIL %s:%d: \"%s\" != \"%s\"\n", __FILE__, __LINE__,     \
                   _a ? _a : "(null)", _b ? _b : "(null)");                      \
        }                                                                        \
    } while (0)

/* 单测 main。每个测试文件在末尾用 AURA_TEST_MAIN() 生成。 */
#define AURA_TEST_MAIN()                                                       \
    int main(void)                                                             \
    {                                                                          \
        printf("== %s: %d test(s)\n", __FILE__, g_test_count);                 \
        fflush(stdout);                                                        \
        for (int i = 0; i < g_test_count; i++) {                               \
            printf("-- %s\n", g_test_cases[i].name);                           \
            fflush(stdout);                                                    \
            g_current_fails = 0;                                               \
            g_test_cases[i].fn();                                              \
            printf("%s %s (%d fail)\n", g_current_fails ? "FAILED" : "ok",     \
                   g_test_cases[i].name, g_current_fails);                     \
            fflush(stdout);                                                    \
        }                                                                      \
        printf("== %d checks, %d fail(s)\n", g_total_checks, g_total_fails);   \
        return (g_total_fails == 0) ? 0 : 1;                                   \
    }

/* 跨平台"等一小会儿"（测试同步用）。 */
static void aura_test_sleep_ms(int ms)
{
    aura_osal_sleep_ms((uint32_t)(ms > 0 ? ms : 0));
}

#ifdef __cplusplus
}
#endif

#endif /* AURA_TESTS_UNIT_AURA_TEST_H */
