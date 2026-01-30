/* ============================================================
 * test_log.c - vox_log 模块测试
 * ============================================================ */

#include "test_runner.h"
#include "../vox_log.h"
#include "../vox_os.h"

/* 测试设置和获取日志级别 */
static void test_log_level(vox_mpool_t* mpool) {
    VOX_UNUSED(mpool);
    vox_log_level_t original = vox_log_get_level();
    
    vox_log_set_level(VOX_LOG_DEBUG);
    TEST_ASSERT_EQ(vox_log_get_level(), VOX_LOG_DEBUG, "设置日志级别失败");
    
    vox_log_set_level(VOX_LOG_ERROR);
    TEST_ASSERT_EQ(vox_log_get_level(), VOX_LOG_ERROR, "设置日志级别失败");
    
    vox_log_set_level(VOX_LOG_TRACE);
    TEST_ASSERT_EQ(vox_log_get_level(), VOX_LOG_TRACE, "设置日志级别失败");
    
    /* 恢复原始级别 */
    vox_log_set_level(original);
}

/* 测试日志输出 */
static void test_log_write(vox_mpool_t* mpool) {
    VOX_UNUSED(mpool);
    /* 测试各种级别的日志输出 */
    VOX_LOG_TRACE("这是TRACE级别的日志");
    VOX_LOG_DEBUG("这是DEBUG级别的日志");
    VOX_LOG_INFO("这是INFO级别的日志");
    VOX_LOG_WARN("这是WARN级别的日志");
    VOX_LOG_ERROR("这是ERROR级别的日志");
    VOX_LOG_FATAL("这是FATAL级别的日志");
}

/* 测试日志回调 */
static int g_callback_called = 0;
static void test_log_callback(const char *level, const char *file, int line, 
                              const char *func, const char *msg, void *userdata) {
    VOX_UNUSED(file);
    VOX_UNUSED(line);
    VOX_UNUSED(func);
    VOX_UNUSED(userdata);
    g_callback_called = 1;
    TEST_ASSERT_NOT_NULL(level, "日志级别为空");
    TEST_ASSERT_NOT_NULL(msg, "日志消息为空");
}

static void test_log_callback_set(vox_mpool_t* mpool) {
    VOX_UNUSED(mpool);
    g_callback_called = 0;
    vox_log_set_callback(test_log_callback, NULL);
    
    VOX_LOG_INFO("测试回调");
    TEST_ASSERT_EQ(g_callback_called, 1, "日志回调未被调用");
    
    /* 清除回调 */
    vox_log_set_callback(NULL, NULL);
}

/* 测试日志级别过滤 */
static void test_log_level_filter(vox_mpool_t* mpool) {
    VOX_UNUSED(mpool);
    vox_log_level_t original = vox_log_get_level();
    
    /* 设置级别为ERROR，应该只输出ERROR和FATAL */
    vox_log_set_level(VOX_LOG_ERROR);
    VOX_LOG_DEBUG("这条DEBUG日志不应该显示");
    VOX_LOG_INFO("这条INFO日志不应该显示");
    VOX_LOG_WARN("这条WARN日志不应该显示");
    VOX_LOG_ERROR("这条ERROR日志应该显示");
    VOX_LOG_FATAL("这条FATAL日志应该显示");
    
    /* 恢复原始级别 */
    vox_log_set_level(original);
}

/* 测试套件 */
test_case_t test_log_cases[] = {
    {"level", test_log_level},
    {"write", test_log_write},
    {"callback", test_log_callback_set},
    {"level_filter", test_log_level_filter},
};

test_suite_t test_log_suite = {
    "vox_log",
    test_log_cases,
    sizeof(test_log_cases) / sizeof(test_log_cases[0])
};
