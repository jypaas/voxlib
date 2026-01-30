/*
 * coroutine_pool_example.c - 协程系统综合测试示例
 *
 * 测试点覆盖:
 * 1. 基本协程创建/恢复/挂起
 * 2. 池化协程
 * 3. 调度器集成
 * 4. 上下文切换性能
 * 5. 高并发场景
 */

#include "../coroutine/vox_coroutine.h"
#include "../coroutine/vox_coroutine_pool.h"
#include "../coroutine/vox_coroutine_scheduler.h"
#include "../vox_os.h"
#include "../vox_loop.h"
#include "../vox_timer.h"
#include <stdio.h>

/* ===== 测试配置 ===== */
#define TEST_BASIC_COUNT        10
#define TEST_POOL_COUNT         100
#define TEST_HIGH_CONCURRENCY   1000
#define TEST_PERF_ITERATIONS    100000

/* ===== 测试统计 ===== */
static int g_test_passed = 0;
static int g_test_failed = 0;
//static int g_coroutine_completed = 0;

#define TEST_ASSERT(cond, msg) do { \
    if (cond) { \
        printf("  [PASS] %s\n", msg); \
        g_test_passed++; \
    } else { \
        printf("  [FAIL] %s\n", msg); \
        g_test_failed++; \
    } \
} while(0)

/* ===== 测试1: 基本协程功能 ===== */

static int g_basic_value = 0;

VOX_COROUTINE_ENTRY(basic_coroutine, void* data) {
    int* value = (int*)user_data;
    (void)co;

    /* 修改值 */
    *value = 42;
    g_basic_value = 42;
}

VOX_COROUTINE_ENTRY(yield_coroutine, void* data) {
    int* counter = (int*)user_data;

    (*counter)++;
    vox_coroutine_yield(co);

    (*counter)++;
    vox_coroutine_yield(co);

    (*counter)++;
}

static void test_basic_coroutine(vox_loop_t* loop) {
    printf("\n=== 测试1: 基本协程功能 ===\n");

    /* 测试1.1: 创建和执行 */
    int value = 0;
    vox_coroutine_t* co = vox_coroutine_create(loop, basic_coroutine, &value, 0);
    TEST_ASSERT(co != NULL, "协程创建成功");
    TEST_ASSERT(vox_coroutine_get_state(co) == VOX_COROUTINE_READY, "初始状态为READY");

    int ret = vox_coroutine_resume(co);
    TEST_ASSERT(ret == 0, "协程恢复成功");
    TEST_ASSERT(value == 42, "协程执行修改了值");
    TEST_ASSERT(vox_coroutine_get_state(co) == VOX_COROUTINE_COMPLETED, "执行后状态为COMPLETED");

    vox_coroutine_destroy(co);

    /* 测试1.2: yield功能 */
    int counter = 0;
    co = vox_coroutine_create(loop, yield_coroutine, &counter, 0);
    TEST_ASSERT(co != NULL, "yield协程创建成功");

    vox_coroutine_resume(co);
    TEST_ASSERT(counter == 1, "第一次resume后counter=1");
    TEST_ASSERT(vox_coroutine_get_state(co) == VOX_COROUTINE_SUSPENDED, "yield后状态为SUSPENDED");

    vox_coroutine_resume(co);
    TEST_ASSERT(counter == 2, "第二次resume后counter=2");

    vox_coroutine_resume(co);
    TEST_ASSERT(counter == 3, "第三次resume后counter=3");
    TEST_ASSERT(vox_coroutine_get_state(co) == VOX_COROUTINE_COMPLETED, "完成后状态为COMPLETED");

    vox_coroutine_destroy(co);

    /* 测试1.3: 获取当前协程 */
    TEST_ASSERT(vox_coroutine_current() == NULL, "非协程上下文中current为NULL");
}

/* ===== 测试2: 协程池功能 ===== */

static int g_pool_completed = 0;

VOX_COROUTINE_ENTRY(pooled_coroutine, void* data) {
    int* id = (int*)user_data;
    (void)co;
    (void)id;
    g_pool_completed++;
}

static void test_coroutine_pool(vox_loop_t* loop) {
    printf("\n=== 测试2: 协程池功能 ===\n");

    /* 测试2.1: 创建池 */
    vox_coroutine_pool_config_t config;
    vox_coroutine_pool_config_default(&config);
    config.initial_count = 16;
    config.stack_size = 16 * 1024;  /* 16KB栈 */
    config.use_guard_pages = false;  /* 测试时禁用guard page */

    vox_coroutine_pool_t* pool = vox_coroutine_pool_create(loop, &config);
    TEST_ASSERT(pool != NULL, "协程池创建成功");

    /* 测试2.2: 获取统计 */
    vox_coroutine_pool_stats_t stats;
    vox_coroutine_pool_get_stats(pool, &stats);
    TEST_ASSERT(stats.total_created == 16, "预分配了16个槽");
    TEST_ASSERT(stats.current_free == 16, "16个槽空闲");
    TEST_ASSERT(stats.stack_size == 16 * 1024, "栈大小为16KB");

    /* 测试2.3: 池化协程创建 */
    g_pool_completed = 0;
    int ids[TEST_POOL_COUNT];
    vox_coroutine_t* coroutines[TEST_POOL_COUNT];

    for (int i = 0; i < TEST_POOL_COUNT; i++) {
        ids[i] = i;
        coroutines[i] = vox_coroutine_create_pooled(loop, pool, pooled_coroutine, &ids[i]);
    }
    TEST_ASSERT(coroutines[0] != NULL, "池化协程创建成功");
    TEST_ASSERT(vox_coroutine_is_pooled(coroutines[0]), "协程标记为池化");

    /* 执行所有协程 */
    for (int i = 0; i < TEST_POOL_COUNT; i++) {
        if (coroutines[i]) {
            vox_coroutine_resume(coroutines[i]);
        }
    }
    TEST_ASSERT(g_pool_completed == TEST_POOL_COUNT, "所有池化协程执行完成");

    /* 销毁协程(归还到池) */
    for (int i = 0; i < TEST_POOL_COUNT; i++) {
        if (coroutines[i]) {
            vox_coroutine_destroy(coroutines[i]);
        }
    }

    /* 检查池统计 */
    vox_coroutine_pool_get_stats(pool, &stats);
    printf("  池统计: created=%zu, acquired=%zu, released=%zu, peak=%zu\n",
           stats.total_created, stats.total_acquired, stats.total_released, stats.peak_in_use);

    vox_coroutine_pool_destroy(pool);
    printf("  [PASS] 协程池销毁成功\n");
    g_test_passed++;
}

/* ===== 测试3: 调度器功能 ===== */

static int g_sched_completed = 0;

VOX_COROUTINE_ENTRY(scheduled_coroutine, void* data) {
    int* id = (int*)user_data;
    (void)co;
    (void)id;
    g_sched_completed++;
}

static void test_scheduler(vox_loop_t* loop) {
    printf("\n=== 测试3: 调度器功能 ===\n");

    /* 测试3.1: 创建调度器 */
    vox_coroutine_scheduler_config_t config;
    vox_coroutine_scheduler_config_default(&config);
    config.ready_queue_capacity = 1024;
    config.max_resume_per_tick = 32;

    vox_coroutine_scheduler_t* sched = vox_coroutine_scheduler_create(loop, &config);
    TEST_ASSERT(sched != NULL, "调度器创建成功");
    TEST_ASSERT(vox_coroutine_scheduler_empty(sched), "初始调度器为空");

    /* 测试3.2: 调度协程 */
    g_sched_completed = 0;
    int ids[64];
    vox_coroutine_t* coroutines[64];

    for (int i = 0; i < 64; i++) {
        ids[i] = i;
        coroutines[i] = vox_coroutine_create(loop, scheduled_coroutine, &ids[i], 0);
        vox_coroutine_schedule(sched, coroutines[i]);
    }

    TEST_ASSERT(vox_coroutine_scheduler_ready_count(sched) == 64, "64个协程在就绪队列");

    /* 测试3.3: 执行tick */
    size_t resumed = vox_coroutine_scheduler_tick(sched);
    TEST_ASSERT(resumed == 32, "第一次tick恢复32个协程");
    TEST_ASSERT(g_sched_completed == 32, "32个协程执行完成");

    resumed = vox_coroutine_scheduler_tick(sched);
    TEST_ASSERT(resumed == 32, "第二次tick恢复32个协程");
    TEST_ASSERT(g_sched_completed == 64, "64个协程全部执行完成");

    TEST_ASSERT(vox_coroutine_scheduler_empty(sched), "调度器队列为空");

    /* 获取统计 */
    vox_coroutine_scheduler_stats_t stats;
    vox_coroutine_scheduler_get_stats(sched, &stats);
    printf("  调度器统计: scheduled=%zu, resumed=%zu, ticks=%zu\n",
           stats.total_scheduled, stats.total_resumed, stats.ticks);

    /* 清理 */
    for (int i = 0; i < 64; i++) {
        vox_coroutine_destroy(coroutines[i]);
    }
    vox_coroutine_scheduler_destroy(sched);
}

/* ===== 测试4: 上下文切换性能 ===== */

static int g_perf_counter = 0;

VOX_COROUTINE_ENTRY(perf_coroutine, void* data) {
    int iterations = *(int*)user_data;
    for (int i = 0; i < iterations; i++) {
        g_perf_counter++;
        vox_coroutine_yield(co);
    }
}

static void test_context_switch_performance(vox_loop_t* loop) {
    printf("\n=== 测试4: 上下文切换性能 ===\n");

    int iterations = 10000;
    g_perf_counter = 0;

    vox_coroutine_t* co = vox_coroutine_create(loop, perf_coroutine, &iterations, 0);
    TEST_ASSERT(co != NULL, "性能测试协程创建成功");

    uint64_t start = vox_time_now();

    /* 执行多次上下文切换 */
    while (vox_coroutine_get_state(co) != VOX_COROUTINE_COMPLETED) {
        vox_coroutine_resume(co);
    }

    uint64_t end = vox_time_now();
    uint64_t elapsed_us = end - start;

    TEST_ASSERT(g_perf_counter == iterations, "所有迭代完成");

    double ns_per_switch = (double)(elapsed_us * 1000) / (iterations * 2);
    printf("  %d次上下文切换, 耗时: %llu us\n", iterations * 2, (unsigned long long)elapsed_us);
    printf("  平均每次切换: %.2f ns\n", ns_per_switch);

    vox_coroutine_destroy(co);
}

/* ===== 测试5: 高并发场景 ===== */

VOX_COROUTINE_ENTRY(high_concurrency_coroutine, void* data) {
    int* counter = (int*)user_data;
    (void)co;
    (*counter)++;
}

static void test_high_concurrency(vox_loop_t* loop) {
    printf("\n=== 测试5: 高并发场景 ===\n");

    /* 创建大容量池 */
    vox_coroutine_pool_config_t pool_config;
    vox_coroutine_pool_config_default(&pool_config);
    pool_config.initial_count = 64;
    pool_config.max_count = 0;  /* 无限制 */
    pool_config.stack_size = 8 * 1024;  /* 8KB小栈 */
    pool_config.use_guard_pages = false;

    vox_coroutine_pool_t* pool = vox_coroutine_pool_create(loop, &pool_config);
    TEST_ASSERT(pool != NULL, "高并发池创建成功");

    int counter = 0;
    uint64_t start = vox_time_now();

    /* 创建大量协程 */
    for (int i = 0; i < TEST_HIGH_CONCURRENCY; i++) {
        vox_coroutine_t* co = vox_coroutine_create_pooled(
            loop, pool, high_concurrency_coroutine, &counter);
        if (co) {
            vox_coroutine_resume(co);
            vox_coroutine_destroy(co);
        }
    }

    uint64_t end = vox_time_now();
    uint64_t elapsed_us = end - start;

    TEST_ASSERT(counter == TEST_HIGH_CONCURRENCY, "所有高并发协程执行完成");
    printf("  %d个协程创建/执行/销毁, 耗时: %llu us\n",
           TEST_HIGH_CONCURRENCY, (unsigned long long)elapsed_us);
    printf("  平均每个协程: %.2f us\n",
           (double)elapsed_us / TEST_HIGH_CONCURRENCY);

    vox_coroutine_pool_stats_t stats;
    vox_coroutine_pool_get_stats(pool, &stats);
    printf("  池统计: peak_in_use=%zu, total_created=%zu\n",
           stats.peak_in_use, stats.total_created);

    vox_coroutine_pool_destroy(pool);
}

/* ===== 主函数 ===== */

int main(void) {
    printf("========================================\n");
    printf("  VoxLib 协程系统综合测试\n");
    printf("========================================\n");

    /* 创建事件循环 */
    vox_loop_t* loop = vox_loop_create();
    if (!loop) {
        printf("Failed to create event loop\n");
        return 1;
    }

    /* 运行所有测试 */
    test_basic_coroutine(loop);
    test_coroutine_pool(loop);
    test_scheduler(loop);
    test_context_switch_performance(loop);
    test_high_concurrency(loop);

    /* 打印测试结果 */
    printf("\n========================================\n");
    printf("  测试结果: %d 通过, %d 失败\n", g_test_passed, g_test_failed);
    printf("========================================\n");

    vox_loop_destroy(loop);

    return g_test_failed > 0 ? 1 : 0;
}
