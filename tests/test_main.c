/* ============================================================
 * test_main.c - 测试入口文件
 * 运行所有测试套件
 * ============================================================ */

#include "test_runner.h"
#include "../vox_log.h"
#include "../vox_mpool.h"

/* 外部测试套件声明 */
extern test_suite_t test_mpool_suite;
extern test_suite_t test_log_suite;
extern test_suite_t test_vector_suite;
extern test_suite_t test_string_suite;
extern test_suite_t test_queue_suite;
extern test_suite_t test_htable_suite;
extern test_suite_t test_time_suite;
extern test_suite_t test_atomic_suite;
extern test_suite_t test_rbtree_suite;
extern test_suite_t test_mheap_suite;
extern test_suite_t test_crypto_suite;
extern test_suite_t test_scanner_suite;
extern test_suite_t test_file_suite;
extern test_suite_t test_json_suite;
extern test_suite_t test_xml_suite;
extern test_suite_t test_toml_suite;
extern test_suite_t test_thread_suite;
extern test_suite_t test_mutex_suite;
extern test_suite_t test_socket_suite;
extern test_suite_t test_process_suite;
extern test_suite_t test_tpool_suite;
extern test_suite_t test_regex_suite;
extern test_suite_t test_http_router_suite;
extern test_suite_t test_http_middleware_suite;
extern test_suite_t test_http_ws_suite;

#ifdef VOX_USE_SQLITE3
extern test_suite_t test_db_sqlite3_suite;
#endif
#ifdef VOX_USE_DUCKDB
extern test_suite_t test_db_duckdb_suite;
#endif

int main(void) {
    /* 设置日志级别 */
    vox_log_set_level(VOX_LOG_INFO);
    
    VOX_LOG_INFO("========================================");
    VOX_LOG_INFO("VoxLib 单元测试框架");
    VOX_LOG_INFO("========================================");
    
    /* 创建内存池用于测试 */
    vox_mpool_t* mpool = vox_mpool_create();
    if (!mpool) {
        VOX_LOG_FATAL("创建内存池失败，无法运行测试");
        return 1;
    }
    
    /* 收集所有测试套件 */
    test_suite_t suites[] = {
        test_log_suite,
        test_mpool_suite,
        test_vector_suite,
        test_string_suite,
        test_queue_suite,
        test_htable_suite,
        test_time_suite,
        test_atomic_suite,
        test_rbtree_suite,
        test_mheap_suite,
        test_crypto_suite,
        test_scanner_suite,
        test_file_suite,
        test_json_suite,
        test_xml_suite,
        test_toml_suite,
        test_thread_suite,
        test_mutex_suite,
        test_socket_suite,
        test_process_suite,
        test_tpool_suite,
        test_regex_suite,
        test_http_router_suite,
        test_http_middleware_suite,
        test_http_ws_suite,
        #ifdef VOX_USE_SQLITE3
        test_db_sqlite3_suite,
        #endif
        #ifdef VOX_USE_DUCKDB
        test_db_duckdb_suite,
        #endif
        /* 在这里添加更多测试套件 */
    };
    
    size_t suite_count = sizeof(suites) / sizeof(suites[0]);
    
    /* 运行所有测试 */
    test_run_all(suites, suite_count, mpool);
    
    /* 打印统计信息 */
    test_stats_t* stats = test_get_stats();
    test_print_stats(stats);
    
    /* 清理 */
    vox_mpool_destroy(mpool);
    
    /* 返回测试结果 */
    if (stats->failed_tests == 0 && stats->failed_suites == 0) {
        VOX_LOG_INFO("所有测试通过！");
        return 0;
    } else {
        VOX_LOG_ERROR("部分测试失败！");
        return 1;
    }
}
