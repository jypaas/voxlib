/* ============================================================
 * test_runner.c - 测试框架核心实现
 * ============================================================ */

#include "test_runner.h"
#include <string.h>
#include <stdio.h>

/* 全局测试统计信息 */
static test_stats_t g_test_stats = {0};

/* 测试失败标记 */
int g_test_failed = 0;

/* 初始化测试失败标记 */
void test_init_failure_flag(void) {
    g_test_failed = 0;
}

/* 检查测试是否失败 */
int test_check_failure(void) {
    return g_test_failed;
}

/* 运行单个测试用例 */
int test_run_case(const char* suite_name, const char* case_name, test_case_func_t func, vox_mpool_t* mpool) {
    VOX_LOG_INFO("运行测试: [%s] %s", suite_name, case_name);
    
    g_test_stats.total_tests++;
    test_init_failure_flag();
    
    /* 执行测试函数 */
    func(mpool);
    
    /* 检查是否失败 */
    if (test_check_failure()) {
        g_test_stats.failed_tests++;
        VOX_LOG_ERROR("测试失败: [%s] %s", suite_name, case_name);
        return 1;
    } else {
        g_test_stats.passed_tests++;
        VOX_LOG_INFO("测试通过: [%s] %s", suite_name, case_name);
        return 0;
    }
}

/* 运行测试套件 */
int test_run_suite(test_suite_t* suite, vox_mpool_t* mpool) {
    if (!suite || !suite->cases || suite->case_count == 0) {
        VOX_LOG_WARN("测试套件无效: %s", suite ? suite->name : "(NULL)");
        return 1;
    }
    
    VOX_LOG_INFO("========================================");
    VOX_LOG_INFO("运行测试套件: %s", suite->name);
    VOX_LOG_INFO("========================================");
    
    g_test_stats.total_suites++;
    int failed = 0;
    size_t suite_passed = 0;
    size_t suite_failed = 0;
    
    for (size_t i = 0; i < suite->case_count; i++) {
        test_case_t* test_case = &suite->cases[i];
        
        /* 运行测试用例 */
        int result = test_run_case(suite->name, test_case->name, test_case->func, mpool);
        
        if (result == 0) {
            suite_passed++;
        } else {
            suite_failed++;
            failed = 1;
            g_test_stats.failed_tests++;
            g_test_stats.passed_tests--;  /* 之前已经加过了，需要减回来 */
        }
    }
    
    if (failed) {
        g_test_stats.failed_suites++;
        VOX_LOG_ERROR("测试套件失败: %s (通过: %zu/%zu)", suite->name, suite_passed, suite->case_count);
    } else {
        g_test_stats.passed_suites++;
        VOX_LOG_INFO("测试套件通过: %s (通过: %zu/%zu)", suite->name, suite_passed, suite->case_count);
    }
    
    VOX_LOG_INFO("========================================");
    
    return failed ? 1 : 0;
}

/* 运行所有测试套件 */
int test_run_all(test_suite_t* suites, size_t suite_count, vox_mpool_t* mpool) {
    if (!suites || suite_count == 0) {
        VOX_LOG_ERROR("没有测试套件可运行");
        return 1;
    }
    
    VOX_LOG_INFO("========================================");
    VOX_LOG_INFO("开始运行所有测试");
    VOX_LOG_INFO("========================================");
    
    int failed = 0;
    
    for (size_t i = 0; i < suite_count; i++) {
        int result = test_run_suite(&suites[i], mpool);
        if (result != 0) {
            failed = 1;
        }
    }
    
    VOX_LOG_INFO("========================================");
    VOX_LOG_INFO("所有测试运行完成");
    VOX_LOG_INFO("========================================");
    
    return failed ? 1 : 0;
}

/* 打印测试统计信息 */
void test_print_stats(test_stats_t* stats) {
    if (!stats) {
        stats = &g_test_stats;
    }
    
    VOX_LOG_INFO("========================================");
    VOX_LOG_INFO("测试统计信息");
    VOX_LOG_INFO("========================================");
    VOX_LOG_INFO("测试套件: 总计 %zu, 通过 %zu, 失败 %zu", 
                 stats->total_suites, stats->passed_suites, stats->failed_suites);
    VOX_LOG_INFO("测试用例: 总计 %zu, 通过 %zu, 失败 %zu", 
                 stats->total_tests, stats->passed_tests, stats->failed_tests);
    
    if (stats->failed_tests == 0 && stats->failed_suites == 0) {
        VOX_LOG_INFO("所有测试通过！");
    } else {
        VOX_LOG_ERROR("部分测试失败！");
    }
    VOX_LOG_INFO("========================================");
}

/* 获取全局测试统计信息 */
test_stats_t* test_get_stats(void) {
    return &g_test_stats;
}
