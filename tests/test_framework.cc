#include "test_framework.h"

namespace aura::test {

std::vector<TestCase>& Registry() {
    static std::vector<TestCase> registry;
    return registry;
}

Registrar::Registrar(const char* name, std::function<void()> fn) {
    Registry().push_back(TestCase{name, std::move(fn)});
}

int g_checks = 0;
int g_failed_checks = 0;

void ReportFailure(const char* file, int line, const char* expr) {
    ++g_failed_checks;
    std::printf("  FAIL %s:%d: %s\n", file, line, expr);
}

int RunAll() {
    std::printf("[INFO] %zu test cases registered\n", Registry().size());
    fflush(stdout);
    int failed_cases = 0;
    for (const auto& tc : Registry()) {
        int checks_before = g_checks;
        int failures_before = g_failed_checks;
        std::printf("[RUN] %s\n", tc.name.c_str());
        fflush(stdout);
        tc.fn();
        fflush(stdout);
        int case_failures = g_failed_checks - failures_before;
        if (case_failures == 0) {
            std::printf("[PASS] %s (%d checks)\n", tc.name.c_str(),
                        g_checks - checks_before);
        } else {
            std::printf("[FAIL] %s (%d/%d checks failed)\n", tc.name.c_str(),
                        case_failures, g_checks - checks_before);
            ++failed_cases;
        }
        fflush(stdout);
    }
    std::printf("\nTotal: %zu cases, %d checks, %d failures\n", Registry().size(),
                g_checks, g_failed_checks);
    return failed_cases;
}

}  // namespace aura::test
