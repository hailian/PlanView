// PvTest — 极简单测框架（零依赖，替代 doctest）。
// 用法与 doctest 近似：
//   #include "PvTest.h"
//   TEST_CASE("名称") { CHECK(a == b); REQUIRE(!v.empty()); }
// 每个测试文件自行 #define PV_TEST_MAIN 后 include 即成为独立测试可执行文件。
#pragma once

#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

#ifdef _MSC_VER
#include <crtdbg.h>
#include <stdlib.h>
#endif

namespace pv_test {

struct TestCase {
    const char* name;
    void (*fn)();
};

inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> r;
    return r;
}
inline int& failureCount() { static int n = 0; return n; }
inline bool& currentFailed() { static bool f = false; return f; }

struct Registrar {
    Registrar(const char* name, void (*fn)()) { registry().push_back({name, fn}); }
};

inline int runAll(const char* exe) {
    int total = (int)registry().size();
    std::printf("[PvTest] %s: %d 个测试\n", exe, total);
    for (const TestCase& tc : registry()) {
        currentFailed() = false;
        std::printf("  RUN   %s\n", tc.name);
        tc.fn();
        if (currentFailed()) {
            std::printf("  FAIL  %s\n\n", tc.name);
        } else {
            std::printf("  PASS  %s\n", tc.name);
        }
    }
    if (failureCount() > 0)
        std::printf("[PvTest] 失败断言 %d 个\n", failureCount());
    else
        std::printf("[PvTest] 全部通过\n");
    std::printf("\n");
    return failureCount() == 0 ? 0 : 1;
}

} // namespace pv_test

// ---- 断言宏 ----
#define PV_TEST_IMPL(check, exprText, fatal)                                        \
    do {                                                                               \
        if (!(check)) {                                                                \
            ::pv_test::failureCount()++;                                            \
            ::pv_test::currentFailed() = true;                                      \
            std::printf("    %s:%d  CHECK 失败: %s\n", __FILE__, __LINE__, exprText);  \
            if (fatal) return;                                                         \
        }                                                                              \
    } while (0)

#define CHECK(expr) PV_TEST_IMPL((expr), #expr, false)
#define REQUIRE(expr) PV_TEST_IMPL((expr), #expr, true)

// ---- 用例注册 ----
#define PV_TEST_CAT_(a, b) a##b
#define PV_TEST_CAT(a, b) PV_TEST_CAT_(a, b)
#define PV_TEST_UNIQUE_FN PV_TEST_CAT(pv_test_fn_, __LINE__)
#define PV_TEST_REG PV_TEST_CAT(pv_test_reg_, __LINE__)

#define TEST_CASE(name)                                                                 \
    static void PV_TEST_UNIQUE_FN(void);                                            \
    static ::pv_test::Registrar PV_TEST_REG(                                     \
        name, &PV_TEST_UNIQUE_FN);                                                  \
    static void PV_TEST_UNIQUE_FN(void)

#ifdef PV_TEST_MAIN
int main() {
    setvbuf(stdout, nullptr, _IONBF, 0); // 无缓冲：挂起时能看到停在哪
#ifdef _MSC_VER
    // Debug 断言输出到 stderr 而非弹窗（无头环境定位问题用）
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE | _CRTDBG_MODE_DEBUG);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _set_abort_behavior(0, _CALL_REPORTFAULT);
#endif
    return ::pv_test::runAll("pv_tests");
}
#endif
