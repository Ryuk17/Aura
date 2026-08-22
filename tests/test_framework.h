// 迷你测试框架（零依赖）：静态注册 + 断言计数 + 汇总退出码
#pragma once

#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace aura::test {

struct TestCase {
    std::string name;
    std::function<void()> fn;
};

std::vector<TestCase>& Registry();

struct Registrar {
    Registrar(const char* name, std::function<void()> fn);
};

// 汇总结果：返回失败用例数
int RunAll();

extern int g_checks;
extern int g_failed_checks;

void ReportFailure(const char* file, int line, const char* expr);

}  // namespace aura::test

#define TEST_CASE(name)                                                        \
    static void aura_test_fn_##name();                                         \
    static aura::test::Registrar aura_test_reg_##name(#name, aura_test_fn_##name); \
    static void aura_test_fn_##name()

#define CHECK(cond)                                                       \
    do {                                                                  \
        ++aura::test::g_checks;                                           \
        if (!(cond)) aura::test::ReportFailure(__FILE__, __LINE__, #cond); \
    } while (0)

#define CHECK_EQ(a, b)                                                        \
    do {                                                                      \
        ++aura::test::g_checks;                                               \
        if (!((a) == (b))) {                                                  \
            aura::test::ReportFailure(__FILE__, __LINE__, #a " == " #b);      \
        }                                                                     \
    } while (0)
