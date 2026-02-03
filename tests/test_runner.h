/* ============================================================
 * test_runner.h - 测试框架核心头文件
 * ============================================================ */

#ifndef TEST_RUNNER_H
#define TEST_RUNNER_H

#include "../vox_log.h"
#include "../vox_mpool.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <limits.h>

/* 测试用例函数类型 */
typedef void (*test_case_func_t)(vox_mpool_t* mpool);

/* 测试用例结构 */
typedef struct {
    const char* name;           /* 测试用例名称 */
    test_case_func_t func;      /* 测试函数 */
} test_case_t;

/* 测试套件结构 */
typedef struct {
    const char* name;            /* 测试套件名称 */
    test_case_t* cases;          /* 测试用例数组 */
    size_t case_count;           /* 测试用例数量 */
} test_suite_t;

/* 测试统计信息 */
typedef struct {
    size_t total_tests;          /* 总测试数 */
    size_t passed_tests;         /* 通过的测试数 */
    size_t failed_tests;         /* 失败的测试数 */
    size_t total_suites;         /* 总测试套件数 */
    size_t passed_suites;        /* 通过的测试套件数 */
    size_t failed_suites;        /* 失败的测试套件数 */
} test_stats_t;

/* 测试失败标记（线程局部存储，简化版本使用全局变量） */
extern int g_test_failed;

/* 初始化测试失败标记 */
void test_init_failure_flag(void);

/* 检查测试是否失败 */
int test_check_failure(void);

/* 测试断言宏 */
#define TEST_ASSERT(condition, msg) \
    do { \
        if (!(condition)) { \
            VOX_LOG_ERROR("断言失败: %s (文件: %s, 行: %d)", msg, __FILE__, __LINE__); \
            g_test_failed = 1; \
            return; \
        } \
    } while (0)

#define TEST_ASSERT_EQ(a, b, msg) \
    do { \
        if ((a) != (b)) { \
            VOX_LOG_ERROR("断言失败: %s (期望: %lld, 实际: %lld, 文件: %s, 行: %d)", \
                         msg, (long long)(a), (long long)(b), __FILE__, __LINE__); \
            g_test_failed = 1; \
            return; \
        } \
    } while (0)

#define TEST_ASSERT_NE(a, b, msg) \
    do { \
        if ((a) == (b)) { \
            VOX_LOG_ERROR("断言失败: %s (值不应相等: %lld, 文件: %s, 行: %d)", \
                         msg, (long long)(a), __FILE__, __LINE__); \
            g_test_failed = 1; \
            return; \
        } \
    } while (0)

#define TEST_ASSERT_STR_EQ(a, b, msg) \
    do { \
        if (strcmp((a), (b)) != 0) { \
            VOX_LOG_ERROR("断言失败: %s (期望: \"%s\", 实际: \"%s\", 文件: %s, 行: %d)", \
                         msg, (a), (b), __FILE__, __LINE__); \
            g_test_failed = 1; \
            return; \
        } \
    } while (0)

#define TEST_ASSERT_NOT_NULL(ptr, msg) \
    do { \
        if ((ptr) == NULL) { \
            VOX_LOG_ERROR("断言失败: %s (指针为NULL, 文件: %s, 行: %d)", \
                         msg, __FILE__, __LINE__); \
            g_test_failed = 1; \
            return; \
        } \
    } while (0)

#define TEST_ASSERT_NULL(ptr, msg) \
    do { \
        if ((ptr) != NULL) { \
            VOX_LOG_ERROR("断言失败: %s (指针不应为NULL, 文件: %s, 行: %d)", \
                         msg, __FILE__, __LINE__); \
            g_test_failed = 1; \
            return; \
        } \
    } while (0)

#define TEST_ASSERT_GT(a, b, msg) \
    do { \
        if (!((a) > (b))) { \
            VOX_LOG_ERROR("断言失败: %s (期望: %lld > %lld, 实际: %lld <= %lld, 文件: %s, 行: %d)", \
                         msg, (long long)(a), (long long)(b), (long long)(a), (long long)(b), __FILE__, __LINE__); \
            g_test_failed = 1; \
            return; \
        } \
    } while (0)

#define TEST_ASSERT_GE(a, b, msg) \
    do { \
        if (!((a) >= (b))) { \
            VOX_LOG_ERROR("断言失败: %s (期望: %lld >= %lld, 实际: %lld < %lld, 文件: %s, 行: %d)", \
                         msg, (long long)(a), (long long)(b), (long long)(a), (long long)(b), __FILE__, __LINE__); \
            g_test_failed = 1; \
            return; \
        } \
    } while (0)

#define TEST_ASSERT_LT(a, b, msg) \
    do { \
        if (!((a) < (b))) { \
            VOX_LOG_ERROR("断言失败: %s (期望: %lld < %lld, 实际: %lld >= %lld, 文件: %s, 行: %d)", \
                         msg, (long long)(a), (long long)(b), (long long)(a), (long long)(b), __FILE__, __LINE__); \
            g_test_failed = 1; \
            return; \
        } \
    } while (0)

#define TEST_ASSERT_LE(a, b, msg) \
    do { \
        if (!((a) <= (b))) { \
            VOX_LOG_ERROR("断言失败: %s (期望: %lld <= %lld, 实际: %lld > %lld, 文件: %s, 行: %d)", \
                         msg, (long long)(a), (long long)(b), (long long)(a), (long long)(b), __FILE__, __LINE__); \
            g_test_failed = 1; \
            return; \
        } \
    } while (0)

#define TEST_ASSERT_TRUE(condition, msg) TEST_ASSERT(condition, msg)
#define TEST_ASSERT_FALSE(condition, msg) TEST_ASSERT(!(condition), msg)

/* 运行单个测试用例 */
int test_run_case(const char* suite_name, const char* case_name, test_case_func_t func, vox_mpool_t* mpool);

/* 运行测试套件 */
int test_run_suite(test_suite_t* suite, vox_mpool_t* mpool);

/* 运行所有测试套件 */
int test_run_all(test_suite_t* suites, size_t suite_count, vox_mpool_t* mpool);

/* 打印测试统计信息 */
void test_print_stats(test_stats_t* stats);

/* 获取全局测试统计信息 */
test_stats_t* test_get_stats(void);

#endif /* TEST_RUNNER_H */
