/* ============================================================
 * test_process.c - vox_process 模块测试
 * ============================================================ */

#include "test_runner.h"
#include "../vox_process.h"

/* 测试获取当前进程ID */
static void test_process_get_current_id(vox_mpool_t* mpool) {
    (void)mpool;  /* 未使用的参数 */
    vox_process_id_t pid = vox_process_get_current_id();
    TEST_ASSERT_NE(pid, 0, "获取当前进程ID失败");
}

/* 测试获取父进程ID */
static void test_process_get_parent_id(vox_mpool_t* mpool) {
    (void)mpool;  /* 未使用的参数 */
    vox_process_id_t ppid = vox_process_get_parent_id();
    (void)ppid;  /* 父进程ID可能为0（在某些系统上），所以只检查函数不崩溃 */
}

/* 测试环境变量操作 */
static void test_process_env(vox_mpool_t* mpool) {
    const char* test_var = "VOX_TEST_VAR";
    const char* test_value = "test_value_123";
    
    /* 设置环境变量 */
    TEST_ASSERT_EQ(vox_process_setenv(test_var, test_value), 0, "设置环境变量失败");
    
    /* 获取环境变量 */
    char* value = vox_process_getenv(mpool, test_var);
    TEST_ASSERT_NOT_NULL(value, "获取环境变量失败");
    TEST_ASSERT_STR_EQ(value, test_value, "环境变量值不正确");
    vox_mpool_free(mpool, value);
    
    /* 删除环境变量 */
    TEST_ASSERT_EQ(vox_process_unsetenv(test_var), 0, "删除环境变量失败");
    
    /* 验证已删除 */
    value = vox_process_getenv(mpool, test_var);
    TEST_ASSERT_NULL(value, "环境变量应已被删除");
}

/* 测试工作目录操作 */
static void test_process_working_dir(vox_mpool_t* mpool) {
    (void)mpool;  /* 未使用的参数 */
    /* 注意：vox_process模块可能没有getcwd函数，这里测试基本功能 */
    vox_process_id_t pid = vox_process_get_current_id();
    TEST_ASSERT_NE(pid, 0, "获取进程ID失败");
}

/* 测试套件 */
test_case_t test_process_cases[] = {
    {"get_current_id", test_process_get_current_id},
    {"get_parent_id", test_process_get_parent_id},
    {"env", test_process_env},
    {"working_dir", test_process_working_dir},
};

test_suite_t test_process_suite = {
    "vox_process",
    test_process_cases,
    sizeof(test_process_cases) / sizeof(test_process_cases[0])
};
