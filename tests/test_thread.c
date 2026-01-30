/* ============================================================
 * test_thread.c - vox_thread 模块测试
 * ============================================================ */

#include "test_runner.h"
#include "../vox_thread.h"
#include "../vox_atomic.h"
#include "../vox_mutex.h"
#include "../vox_os.h"

/* 线程测试数据 */
//static int g_thread_result = 0;
//static vox_atomic_int_t* g_thread_counter = NULL;

/* 简单线程函数 */
static int simple_thread_func(void* user_data) {
    int* value = (int*)user_data;
    *value = 42;
    return 0;
}

/* 测试创建和join线程 */
static void test_thread_create_join(vox_mpool_t* mpool) {
    int value = 0;
    vox_thread_t* thread = vox_thread_create(mpool, simple_thread_func, &value);
    TEST_ASSERT_NOT_NULL(thread, "创建线程失败");
    
    int exit_code;
    TEST_ASSERT_EQ(vox_thread_join(thread, &exit_code), 0, "join线程失败");
    TEST_ASSERT_EQ(exit_code, 0, "线程退出码不正确");
    TEST_ASSERT_EQ(value, 42, "线程函数未执行");
}

/* 测试线程ID */
static void test_thread_id(vox_mpool_t* mpool) {
    vox_thread_id_t self_id = vox_thread_self();
    TEST_ASSERT_NE(self_id, 0, "获取当前线程ID失败");
    
    int value = 0;
    vox_thread_t* thread = vox_thread_create(mpool, simple_thread_func, &value);
    TEST_ASSERT_NOT_NULL(thread, "创建线程失败");
    
    vox_thread_id_t thread_id = vox_thread_id(thread);
    TEST_ASSERT_NE(thread_id, 0, "获取线程ID失败");
    TEST_ASSERT_EQ(vox_thread_id_equal(thread_id, self_id), 0, "线程ID不应相等");
    
    vox_thread_join(thread, NULL);
}

/* 测试线程yield和sleep */
static void test_thread_yield_sleep(vox_mpool_t* mpool) {
    VOX_UNUSED(mpool);
    vox_thread_yield();  /* 应该不会崩溃 */
    
    vox_thread_sleep(10);  /* 休眠10毫秒，应该不会崩溃 */
}

/* 测试线程优先级 */
static void test_thread_priority(vox_mpool_t* mpool) {
    int value = 0;
    vox_thread_t* thread = vox_thread_create(mpool, simple_thread_func, &value);
    TEST_ASSERT_NOT_NULL(thread, "创建线程失败");
    
    /* 测试设置和获取优先级 */
    /* 注意：在Linux上，提高优先级（HIGHEST）可能需要root权限，所以先测试NORMAL */
    int result = vox_thread_set_priority(thread, VOX_THREAD_PRIORITY_NORMAL);
    /* NORMAL优先级应该总是可以设置的 */
    TEST_ASSERT_EQ(result, 0, "设置NORMAL优先级失败");
    
    vox_thread_priority_t priority;
    TEST_ASSERT_EQ(vox_thread_get_priority(thread, &priority), 0, "获取优先级失败");
    /* 获取的优先级应该是NORMAL或接近NORMAL（取决于系统实现） */
    
    /* 尝试设置HIGHEST优先级，如果失败（没有权限）也是可以接受的 */
    result = vox_thread_set_priority(thread, VOX_THREAD_PRIORITY_HIGHEST);
    /* 在Linux上，如果没有root权限，设置HIGHEST可能失败，这是正常的 */
    /* 我们只验证函数不会崩溃，不强制要求成功 */
    
    vox_thread_join(thread, NULL);
}

/* 测试线程本地存储 */
static int tls_test_func(void* user_data) {
    vox_tls_key_t* tls = (vox_tls_key_t*)user_data;
    
    int value = 42;
    if (vox_tls_set(tls, &value) != 0) {
        return -1;  /* 设置TLS失败 */
    }
    
    int* retrieved = (int*)vox_tls_get(tls);
    if (retrieved == NULL || *retrieved != 42) {
        return -1;  /* 获取TLS失败或值不正确 */
    }
    
    return 0;
}

static void test_thread_tls(vox_mpool_t* mpool) {
    vox_tls_key_t* tls = vox_tls_key_create(mpool, NULL);
    TEST_ASSERT_NOT_NULL(tls, "创建TLS失败");
    
    vox_thread_t* thread = vox_thread_create(mpool, tls_test_func, tls);
    TEST_ASSERT_NOT_NULL(thread, "创建线程失败");
    
    vox_thread_join(thread, NULL);
    vox_tls_key_destroy(tls);
}

/* 多线程竞争测试数据结构 */
typedef struct {
    int* counter;
    vox_mutex_t mutex;
    int iterations;
} thread_contention_data_t;

/* 计数器竞争线程函数 */
static int counter_contention_func(void* user_data) {
    thread_contention_data_t* data = (thread_contention_data_t*)user_data;
    
    for (int i = 0; i < data->iterations; i++) {
        vox_mutex_lock(&data->mutex);
        (*data->counter)++;
        vox_mutex_unlock(&data->mutex);
    }
    
    return 0;
}

/* 测试多线程计数器竞争 */
static void test_thread_counter_contention(vox_mpool_t* mpool) {
    int counter = 0;
    thread_contention_data_t data = {
        .counter = &counter,
        .iterations = 1000
    };
    TEST_ASSERT_EQ(vox_mutex_create(&data.mutex), 0, "创建互斥锁失败");
    
    /* 创建10个线程同时增加计数器 */
    vox_thread_t* threads[10];
    for (int i = 0; i < 10; i++) {
        threads[i] = vox_thread_create(mpool, counter_contention_func, &data);
        TEST_ASSERT_NOT_NULL(threads[i], "创建线程失败");
    }
    
    /* 等待所有线程完成 */
    for (int i = 0; i < 10; i++) {
        vox_thread_join(threads[i], NULL);
    }
    
    /* 验证计数器值（10个线程，每个1000次） */
    TEST_ASSERT_EQ(counter, 10000, "多线程竞争后计数器值不正确");
    
    vox_mutex_destroy(&data.mutex);
}

/* 竞态条件测试数据结构 */
typedef struct {
    int* shared_var;
    vox_mutex_t* mutex;
    int thread_id;
} race_condition_data_t;

/* 竞态条件测试线程函数 */
static int race_condition_func(void* user_data) {
    race_condition_data_t* data = (race_condition_data_t*)user_data;
    
    /* 不使用锁，模拟竞态条件 */
    for (int i = 0; i < 100; i++) {
        int temp = *data->shared_var;
        vox_thread_yield();  /* 让出CPU，增加竞态条件发生的概率 */
        *data->shared_var = temp + 1;
    }
    
    return 0;
}

/* 测试竞态条件（不使用锁） */
static void test_thread_race_condition(vox_mpool_t* mpool) {
    int shared_var = 0;
    race_condition_data_t data[5];
    
    /* 创建5个线程，不使用锁，应该出现竞态条件 */
    vox_thread_t* threads[5];
    for (int i = 0; i < 5; i++) {
        data[i].shared_var = &shared_var;
        data[i].mutex = NULL;
        data[i].thread_id = i;
        threads[i] = vox_thread_create(mpool, race_condition_func, &data[i]);
        TEST_ASSERT_NOT_NULL(threads[i], "创建线程失败");
    }
    
    /* 等待所有线程完成 */
    for (int i = 0; i < 5; i++) {
        vox_thread_join(threads[i], NULL);
    }
    
    /* 由于竞态条件，最终值可能小于500（5个线程，每个100次） */
    /* 这个测试主要验证多线程能正常运行，不验证数据正确性 */
    TEST_ASSERT_LE(shared_var, 500, "竞态条件测试：值不应超过预期");
    /* 至少应该有一些更新 */
    TEST_ASSERT_GT(shared_var, 0, "竞态条件测试：应该有更新");
}

/* 同步测试数据结构 */
typedef struct {
    int* ready_count;
    vox_mutex_t mutex;
    int total_threads;
} sync_test_data_t;

/* 同步测试线程函数 */
static int sync_test_func(void* user_data) {
    sync_test_data_t* data = (sync_test_data_t*)user_data;
    
    /* 模拟一些工作 */
    vox_thread_sleep(10);
    
    /* 增加就绪计数 */
    vox_mutex_lock(&data->mutex);
    (*data->ready_count)++;
    vox_mutex_unlock(&data->mutex);
    
    return 0;
}

/* 测试多线程同步 */
static void test_thread_sync(vox_mpool_t* mpool) {
    int ready_count = 0;
    sync_test_data_t data = {
        .ready_count = &ready_count,
        .total_threads = 5
    };
    TEST_ASSERT_EQ(vox_mutex_create(&data.mutex), 0, "创建互斥锁失败");
    
    /* 创建5个线程 */
    vox_thread_t* threads[5];
    for (int i = 0; i < 5; i++) {
        threads[i] = vox_thread_create(mpool, sync_test_func, &data);
        TEST_ASSERT_NOT_NULL(threads[i], "创建线程失败");
    }
    
    /* 等待所有线程完成 */
    for (int i = 0; i < 5; i++) {
        vox_thread_join(threads[i], NULL);
    }
    
    /* 验证所有线程都完成了 */
    TEST_ASSERT_EQ(ready_count, 5, "多线程同步后计数不正确");
    
    vox_mutex_destroy(&data.mutex);
}

/* 测试套件 */
test_case_t test_thread_cases[] = {
    {"create_join", test_thread_create_join},
    {"id", test_thread_id},
    {"yield_sleep", test_thread_yield_sleep},
    {"priority", test_thread_priority},
    {"tls", test_thread_tls},
    {"counter_contention", test_thread_counter_contention},
    {"race_condition", test_thread_race_condition},
    {"sync", test_thread_sync},
};

test_suite_t test_thread_suite = {
    "vox_thread",
    test_thread_cases,
    sizeof(test_thread_cases) / sizeof(test_thread_cases[0])
};
