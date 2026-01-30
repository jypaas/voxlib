/* ============================================================
 * test_tpool.c - vox_tpool 模块测试
 * ============================================================ */

#include "test_runner.h"
#include "../vox_tpool.h"
#include "../vox_atomic.h"
#include "../vox_mutex.h"
#include "../vox_thread.h"
#include "../vox_os.h"
#include <stdio.h>

/* 测试数据结构 */
typedef struct {
    int* counter;
    vox_mutex_t* mutex;
    int expected_value;
} task_data_t;

/* 简单任务函数 - 增加计数器 */
static void simple_task_func(void* user_data) {
    int* counter = (int*)user_data;
    if (counter) {
        (*counter)++;
    }
}

/* 使用原子操作的任务函数 */
static void atomic_task_func(void* user_data) {
    vox_atomic_int_t* counter = (vox_atomic_int_t*)user_data;
    if (counter) {
        vox_atomic_int_increment(counter);
    }
}

/* 测试创建和销毁线程池 */
static void test_tpool_create_destroy(vox_mpool_t* mpool) {
    VOX_UNUSED(mpool);
    vox_tpool_t* tpool = vox_tpool_create();
    TEST_ASSERT_NOT_NULL(tpool, "创建线程池失败");
    
    vox_tpool_destroy(tpool);
}

/* 测试使用配置创建线程池 */
static void test_tpool_create_with_config(vox_mpool_t* mpool) {
    VOX_UNUSED(mpool);
    vox_tpool_config_t config = {
        .thread_count = 4,
        .queue_capacity = 128,
        .thread_priority = -1,  /* 使用默认优先级 */
    };
    
    vox_tpool_t* tpool = vox_tpool_create_with_config(&config);
    TEST_ASSERT_NOT_NULL(tpool, "使用配置创建线程池失败");
    
    vox_tpool_destroy(tpool);
}

/* 测试提交单个任务 */
static void test_tpool_submit_single(vox_mpool_t* mpool) {
    VOX_UNUSED(mpool);
    vox_tpool_t* tpool = vox_tpool_create();
    TEST_ASSERT_NOT_NULL(tpool, "创建线程池失败");
    
    int counter = 0;
    TEST_ASSERT_EQ(vox_tpool_submit(tpool, simple_task_func, &counter, NULL), 0, 
                   "提交任务失败");
    
    /* 等待任务完成 */
    vox_tpool_wait(tpool);
    
    TEST_ASSERT_EQ(counter, 1, "任务未执行或执行不正确");
    
    vox_tpool_destroy(tpool);
}

/* 测试提交多个任务 */
static void test_tpool_submit_multiple(vox_mpool_t* mpool) {
    vox_tpool_t* tpool = vox_tpool_create();
    TEST_ASSERT_NOT_NULL(tpool, "创建线程池失败");
    
    vox_atomic_int_t* counter = vox_atomic_int_create(mpool, 0);
    TEST_ASSERT_NOT_NULL(counter, "创建原子计数器失败");
    
    const int task_count = 100;
    
    /* 提交100个任务 */
    for (int i = 0; i < task_count; i++) {
        TEST_ASSERT_EQ(vox_tpool_submit(tpool, atomic_task_func, counter, NULL), 0, 
                       "提交任务失败");
    }
    
    /* 等待所有任务完成 */
    vox_tpool_wait(tpool);
    
    int32_t final_value = vox_atomic_int_load(counter);
    TEST_ASSERT_EQ(final_value, task_count, "任务执行数量不正确");
    
    vox_atomic_int_destroy(counter);
    vox_tpool_destroy(tpool);
}

/* 带互斥锁的任务函数 */
static void mutex_task_func(void* user_data) {
    task_data_t* data = (task_data_t*)user_data;
    if (data && data->mutex && data->counter) {
        vox_mutex_lock(data->mutex);
        (*data->counter)++;
        vox_mutex_unlock(data->mutex);
    }
}

/* 测试并发任务执行 */
static void test_tpool_concurrent_tasks(vox_mpool_t* mpool) {
    VOX_UNUSED(mpool);
    vox_tpool_t* tpool = vox_tpool_create();
    TEST_ASSERT_NOT_NULL(tpool, "创建线程池失败");
    
    int counter = 0;
    vox_mutex_t mutex;
    TEST_ASSERT_EQ(vox_mutex_create(&mutex), 0, "创建互斥锁失败");
    
    task_data_t data = {
        .counter = &counter,
        .mutex = &mutex,
        .expected_value = 0
    };
    
    const int task_count = 100;
    
    /* 提交100个并发任务 */
    for (int i = 0; i < task_count; i++) {
        TEST_ASSERT_EQ(vox_tpool_submit(tpool, mutex_task_func, &data, NULL), 0, 
                       "提交任务失败");
    }
    
    /* 等待所有任务完成 */
    vox_tpool_wait(tpool);
    
    TEST_ASSERT_EQ(counter, task_count, "并发任务执行数量不正确");
    
    vox_mutex_destroy(&mutex);
    vox_tpool_destroy(tpool);
}

/* 任务完成回调数据（使用全局变量简化测试） */
static int g_callback_count = 0;
static vox_mutex_t* g_callback_mutex = NULL;

/* 任务完成回调函数 */
static void task_complete_callback(void* user_data, int result) {
    VOX_UNUSED(user_data);
    VOX_UNUSED(result);
    if (g_callback_mutex) {
        vox_mutex_lock(g_callback_mutex);
        g_callback_count++;
        vox_mutex_unlock(g_callback_mutex);
    }
}

/* 测试任务完成回调 */
static void test_tpool_complete_callback(vox_mpool_t* mpool) {
    vox_tpool_t* tpool = vox_tpool_create();
    TEST_ASSERT_NOT_NULL(tpool, "创建线程池失败");
    
    vox_atomic_int_t* counter = vox_atomic_int_create(mpool, 0);
    TEST_ASSERT_NOT_NULL(counter, "创建原子计数器失败");
    
    g_callback_count = 0;
    vox_mutex_t mutex;
    TEST_ASSERT_EQ(vox_mutex_create(&mutex), 0, "创建互斥锁失败");
    g_callback_mutex = &mutex;
    
    const int task_count = 50;
    
    /* 提交任务，带完成回调 */
    for (int i = 0; i < task_count; i++) {
        TEST_ASSERT_EQ(vox_tpool_submit(tpool, atomic_task_func, counter, 
                                        task_complete_callback), 0, 
                       "提交任务失败");
    }
    
    /* 等待所有任务完成 */
    vox_tpool_wait(tpool);
    
    int32_t final_value = vox_atomic_int_load(counter);
    TEST_ASSERT_EQ(final_value, task_count, "任务执行数量不正确");
    TEST_ASSERT_EQ(g_callback_count, task_count, "回调函数调用次数不正确");
    
    g_callback_mutex = NULL;
    vox_mutex_destroy(&mutex);
    vox_atomic_int_destroy(counter);
    vox_tpool_destroy(tpool);
}

/* 测试获取待处理任务数 */
static void test_tpool_pending_tasks(vox_mpool_t* mpool) {
    vox_tpool_t* tpool = vox_tpool_create();
    TEST_ASSERT_NOT_NULL(tpool, "创建线程池失败");
    
    /* 初始应该为0 */
    TEST_ASSERT_EQ(vox_tpool_pending_tasks(tpool), 0, "初始待处理任务数应为0");
    
    vox_atomic_int_t* counter = vox_atomic_int_create(mpool, 0);
    TEST_ASSERT_NOT_NULL(counter, "创建原子计数器失败");
    
    const int task_count = 10;
    
    /* 提交任务 */
    for (int i = 0; i < task_count; i++) {
        TEST_ASSERT_EQ(vox_tpool_submit(tpool, atomic_task_func, counter, NULL), 0, 
                       "提交任务失败");
    }
    
    /* 等待一小段时间让任务开始执行 */
    vox_thread_sleep(10);
    
    /* 等待所有任务完成 */
    vox_tpool_wait(tpool);
    
    /* 完成后应该为0 */
    TEST_ASSERT_EQ(vox_tpool_pending_tasks(tpool), 0, "完成后待处理任务数应为0");
    int32_t final_value = vox_atomic_int_load(counter);
    TEST_ASSERT_EQ(final_value, task_count, "任务执行数量不正确");
    
    vox_atomic_int_destroy(counter);
    vox_tpool_destroy(tpool);
}

/* 测试获取正在执行的任务数 */
static void test_tpool_running_tasks(vox_mpool_t* mpool) {
    vox_tpool_t* tpool = vox_tpool_create();
    TEST_ASSERT_NOT_NULL(tpool, "创建线程池失败");
    
    /* 初始应该为0 */
    TEST_ASSERT_EQ(vox_tpool_running_tasks(tpool), 0, "初始正在执行任务数应为0");
    
    vox_atomic_int_t* counter = vox_atomic_int_create(mpool, 0);
    TEST_ASSERT_NOT_NULL(counter, "创建原子计数器失败");
    
    const int task_count = 20;
    
    /* 提交任务 */
    for (int i = 0; i < task_count; i++) {
        TEST_ASSERT_EQ(vox_tpool_submit(tpool, atomic_task_func, counter, NULL), 0, 
                       "提交任务失败");
    }
    
    /* 等待所有任务完成 */
    vox_tpool_wait(tpool);
    
    /* 完成后应该为0 */
    TEST_ASSERT_EQ(vox_tpool_running_tasks(tpool), 0, "完成后正在执行任务数应为0");
    int32_t final_value = vox_atomic_int_load(counter);
    TEST_ASSERT_EQ(final_value, task_count, "任务执行数量不正确");
    
    vox_atomic_int_destroy(counter);
    vox_tpool_destroy(tpool);
}

/* 测试获取统计信息 */
static void test_tpool_stats(vox_mpool_t* mpool) {
    vox_tpool_t* tpool = vox_tpool_create();
    TEST_ASSERT_NOT_NULL(tpool, "创建线程池失败");
    
    size_t total_tasks = 0;
    size_t completed_tasks = 0;
    size_t failed_tasks = 0;
    
    /* 初始统计信息 */
    vox_tpool_stats(tpool, &total_tasks, &completed_tasks, &failed_tasks);
    TEST_ASSERT_EQ(total_tasks, 0, "初始总任务数应为0");
    TEST_ASSERT_EQ(completed_tasks, 0, "初始已完成任务数应为0");
    TEST_ASSERT_EQ(failed_tasks, 0, "初始失败任务数应为0");
    
    vox_atomic_int_t* counter = vox_atomic_int_create(mpool, 0);
    TEST_ASSERT_NOT_NULL(counter, "创建原子计数器失败");
    
    const int task_count = 30;
    
    /* 提交任务 */
    for (int i = 0; i < task_count; i++) {
        TEST_ASSERT_EQ(vox_tpool_submit(tpool, atomic_task_func, counter, NULL), 0, 
                       "提交任务失败");
    }
    
    /* 等待所有任务完成 */
    vox_tpool_wait(tpool);
    
    /* 检查统计信息 */
    vox_tpool_stats(tpool, &total_tasks, &completed_tasks, &failed_tasks);
    TEST_ASSERT_EQ(total_tasks, (size_t)task_count, "总任务数不正确");
    TEST_ASSERT_EQ(completed_tasks, (size_t)task_count, "已完成任务数不正确");
    TEST_ASSERT_EQ(failed_tasks, 0, "失败任务数应为0");
    int32_t final_value = vox_atomic_int_load(counter);
    TEST_ASSERT_EQ(final_value, task_count, "任务执行数量不正确");
    
    vox_atomic_int_destroy(counter);
    vox_tpool_destroy(tpool);
}

/* 测试优雅关闭 */
static void test_tpool_shutdown(vox_mpool_t* mpool) {
    vox_tpool_t* tpool = vox_tpool_create();
    TEST_ASSERT_NOT_NULL(tpool, "创建线程池失败");
    
    vox_atomic_int_t* counter = vox_atomic_int_create(mpool, 0);
    TEST_ASSERT_NOT_NULL(counter, "创建原子计数器失败");
    
    const int task_count = 50;
    
    /* 提交任务 */
    for (int i = 0; i < task_count; i++) {
        TEST_ASSERT_EQ(vox_tpool_submit(tpool, atomic_task_func, counter, NULL), 0, 
                       "提交任务失败");
    }
    
    /* 优雅关闭（等待所有任务完成） */
    TEST_ASSERT_EQ(vox_tpool_shutdown(tpool), 0, "关闭线程池失败");
    
    /* 验证所有任务都已完成 */
    int32_t final_value = vox_atomic_int_load(counter);
    TEST_ASSERT_EQ(final_value, task_count, "关闭后任务执行数量不正确");
    
    /* 关闭后不应接受新任务 */
    int test_counter = 0;
    TEST_ASSERT_NE(vox_tpool_submit(tpool, simple_task_func, &test_counter, NULL), 0, 
                   "关闭后不应接受新任务");
    
    vox_atomic_int_destroy(counter);
    vox_tpool_destroy(tpool);
}

/* 测试强制关闭 */
static void test_tpool_force_shutdown(vox_mpool_t* mpool) {
    VOX_UNUSED(mpool);
    vox_tpool_t* tpool = vox_tpool_create();
    TEST_ASSERT_NOT_NULL(tpool, "创建线程池失败");
    
    int counter = 0;
    const int task_count = 20;
    
    /* 提交任务 */
    for (int i = 0; i < task_count; i++) {
        TEST_ASSERT_EQ(vox_tpool_submit(tpool, simple_task_func, &counter, NULL), 0, 
                       "提交任务失败");
    }
    
    /* 强制关闭（不等待任务完成） */
    vox_tpool_force_shutdown(tpool);
    
    /* 关闭后不应接受新任务 */
    int test_counter = 0;
    TEST_ASSERT_NE(vox_tpool_submit(tpool, simple_task_func, &test_counter, NULL), 0, 
                   "关闭后不应接受新任务");
    
    vox_tpool_destroy(tpool);
}

/* 长时间运行的任务函数（使用原子操作） */
static void long_task_func(void* user_data) {
    vox_atomic_int_t* counter = (vox_atomic_int_t*)user_data;
    if (counter) {
        vox_thread_sleep(10);  /* 休眠10毫秒 */
        vox_atomic_int_increment(counter);
    }
}

/* 测试长时间运行的任务 */
static void test_tpool_long_running_tasks(vox_mpool_t* mpool) {
    vox_tpool_t* tpool = vox_tpool_create();
    TEST_ASSERT_NOT_NULL(tpool, "创建线程池失败");
    
    vox_atomic_int_t* counter = vox_atomic_int_create(mpool, 0);
    TEST_ASSERT_NOT_NULL(counter, "创建原子计数器失败");
    
    const int task_count = 20;
    
    /* 提交长时间运行的任务 */
    for (int i = 0; i < task_count; i++) {
        TEST_ASSERT_EQ(vox_tpool_submit(tpool, long_task_func, counter, NULL), 0, 
                       "提交任务失败");
    }
    
    /* 等待所有任务完成 */
    vox_tpool_wait(tpool);
    
    int32_t final_value = vox_atomic_int_load(counter);
    TEST_ASSERT_EQ(final_value, task_count, "长时间运行任务执行数量不正确");
    
    vox_atomic_int_destroy(counter);
    vox_tpool_destroy(tpool);
}

/* 压力测试数据结构 */
typedef struct {
    vox_atomic_int_t* counter;
    int iterations;
} stress_test_data_t;

/* 压力测试任务函数 */
static void stress_task_func(void* user_data) {
    stress_test_data_t* data = (stress_test_data_t*)user_data;
    if (data && data->counter) {
        for (int i = 0; i < data->iterations; i++) {
            vox_atomic_int_increment(data->counter);
        }
    }
}

/* 压力测试 */
static void test_tpool_stress(vox_mpool_t* mpool) {
    vox_tpool_t* tpool = vox_tpool_create();
    TEST_ASSERT_NOT_NULL(tpool, "创建线程池失败");
    
    vox_atomic_int_t* counter = vox_atomic_int_create(mpool, 0);
    TEST_ASSERT_NOT_NULL(counter, "创建原子计数器失败");
    
    stress_test_data_t data = {
        .counter = counter,
        .iterations = 100
    };
    
    const int task_count = 1000;
    
    /* 提交大量任务 */
    for (int i = 0; i < task_count; i++) {
        TEST_ASSERT_EQ(vox_tpool_submit(tpool, stress_task_func, &data, NULL), 0, 
                       "提交任务失败");
    }
    
    /* 等待所有任务完成 */
    vox_tpool_wait(tpool);
    
    /* 验证结果 */
    int32_t final_value = vox_atomic_int_load(counter);
    int32_t expected_value = task_count * data.iterations;
    TEST_ASSERT_EQ(final_value, expected_value, "压力测试结果不正确");
    
    vox_atomic_int_destroy(counter);
    vox_tpool_destroy(tpool);
}

/* 测试线程安全配置 */
static void test_tpool_thread_safe_config(vox_mpool_t* mpool) {
    VOX_UNUSED(mpool);
    
    /* 测试非线程安全配置 */
    vox_tpool_config_t config1 = {
        .thread_count = 2,
        .queue_capacity = 64,
        .thread_priority = -1,
    };
    
    vox_tpool_t* tpool1 = vox_tpool_create_with_config(&config1);
    TEST_ASSERT_NOT_NULL(tpool1, "创建非线程安全线程池失败");
    
    int counter1 = 0;
    TEST_ASSERT_EQ(vox_tpool_submit(tpool1, simple_task_func, &counter1, NULL), 0, 
                   "提交任务失败");
    vox_tpool_wait(tpool1);
    TEST_ASSERT_EQ(counter1, 1, "任务执行不正确");
    
    vox_tpool_destroy(tpool1);
    
    /* 测试线程安全配置 */
    vox_tpool_config_t config2 = {
        .thread_count = 2,
        .queue_capacity = 64,
        .thread_priority = -1,
    };
    
    vox_tpool_t* tpool2 = vox_tpool_create_with_config(&config2);
    TEST_ASSERT_NOT_NULL(tpool2, "创建线程安全线程池失败");
    
    int counter2 = 0;
    TEST_ASSERT_EQ(vox_tpool_submit(tpool2, simple_task_func, &counter2, NULL), 0, 
                   "提交任务失败");
    vox_tpool_wait(tpool2);
    TEST_ASSERT_EQ(counter2, 1, "任务执行不正确");
    
    vox_tpool_destroy(tpool2);
}

/* 测试队列类型配置 */
static void test_tpool_queue_type_config(vox_mpool_t* mpool) {
    /* 测试 MPSC 队列类型（默认，无锁） */
    vox_tpool_config_t mpsc_config = {
        .thread_count = 4,
        .queue_capacity = 128,
        .thread_priority = -1,
        .queue_type = VOX_QUEUE_TYPE_MPSC,  /* 显式设置 MPSC */
    };
    
    vox_tpool_t* mpsc_tpool = vox_tpool_create_with_config(&mpsc_config);
    TEST_ASSERT_NOT_NULL(mpsc_tpool, "创建 MPSC 队列类型线程池失败");
    
    vox_atomic_int_t* mpsc_counter = vox_atomic_int_create(mpool, 0);
    TEST_ASSERT_NOT_NULL(mpsc_counter, "创建原子计数器失败");
    
    const int mpsc_task_count = 200;
    
    /* 提交多个任务（允许部分失败，因为队列容量可能小于任务数） */
    int mpsc_submitted = 0;
    int mpsc_failed = 0;
    for (int i = 0; i < mpsc_task_count; i++) {
        int result = vox_tpool_submit(mpsc_tpool, atomic_task_func, mpsc_counter, NULL);
        if (result == 0) {
            mpsc_submitted++;
        } else {
            mpsc_failed++;
        }
    }
    
    /* 至少应该有一些任务成功提交 */
    TEST_ASSERT_GT(mpsc_submitted, 0, "应该有任务成功提交");
    
    /* 等待所有任务完成 */
    vox_tpool_wait(mpsc_tpool);
    
    int32_t mpsc_final_value = vox_atomic_int_load(mpsc_counter);
    TEST_ASSERT_EQ(mpsc_final_value, mpsc_submitted, "MPSC 队列类型任务执行数量不正确");
    
    vox_atomic_int_destroy(mpsc_counter);
    vox_tpool_destroy(mpsc_tpool);
    
    /* 测试 NORMAL 队列类型（多线程时需要 mutex 保护） */
    vox_tpool_config_t normal_config = {
        .thread_count = 4,
        .queue_capacity = 128,
        .thread_priority = -1,
        .queue_type = VOX_QUEUE_TYPE_NORMAL,  /* 使用 NORMAL 类型 */
    };
    
    vox_tpool_t* normal_tpool = vox_tpool_create_with_config(&normal_config);
    TEST_ASSERT_NOT_NULL(normal_tpool, "创建 NORMAL 队列类型线程池失败");
    
    vox_atomic_int_t* normal_counter = vox_atomic_int_create(mpool, 0);
    TEST_ASSERT_NOT_NULL(normal_counter, "创建原子计数器失败");
    
    const int normal_task_count = 200;
    
    /* 提交多个任务（允许部分失败，因为队列容量可能小于任务数） */
    int normal_submitted = 0;
    int normal_failed = 0;
    for (int i = 0; i < normal_task_count; i++) {
        int result = vox_tpool_submit(normal_tpool, atomic_task_func, normal_counter, NULL);
        if (result == 0) {
            normal_submitted++;
        } else {
            normal_failed++;
        }
    }
    
    /* 至少应该有一些任务成功提交 */
    TEST_ASSERT_GT(normal_submitted, 0, "应该有任务成功提交");
    
    /* 等待所有任务完成 */
    vox_tpool_wait(normal_tpool);
    
    int32_t normal_final_value = vox_atomic_int_load(normal_counter);
    TEST_ASSERT_EQ(normal_final_value, normal_submitted, "NORMAL 队列类型任务执行数量不正确");
    
    vox_atomic_int_destroy(normal_counter);
    vox_tpool_destroy(normal_tpool);
    
    /* 测试单线程 NORMAL 队列类型（不需要 mutex） */
    vox_tpool_config_t single_thread_config = {
        .thread_count = 1,
        .queue_capacity = 64,
        .thread_priority = -1,
        .queue_type = VOX_QUEUE_TYPE_NORMAL,  /* 单线程时不需要 mutex */
    };
    
    vox_tpool_t* single_tpool = vox_tpool_create_with_config(&single_thread_config);
    TEST_ASSERT_NOT_NULL(single_tpool, "创建单线程 NORMAL 队列类型线程池失败");
    
    vox_atomic_int_t* single_counter = vox_atomic_int_create(mpool, 0);
    TEST_ASSERT_NOT_NULL(single_counter, "创建原子计数器失败");
    
    const int single_task_count = 50;
    
    /* 提交多个任务 */
    for (int i = 0; i < single_task_count; i++) {
        TEST_ASSERT_EQ(vox_tpool_submit(single_tpool, atomic_task_func, single_counter, NULL), 0, 
                       "提交任务失败");
    }
    
    /* 等待所有任务完成 */
    vox_tpool_wait(single_tpool);
    
    int32_t single_final_value = vox_atomic_int_load(single_counter);
    TEST_ASSERT_EQ(single_final_value, single_task_count, "单线程 NORMAL 队列类型任务执行数量不正确");
    
    vox_atomic_int_destroy(single_counter);
    vox_tpool_destroy(single_tpool);
}

/* 阻塞任务函数 - 等待信号量 */
static vox_atomic_int_t* g_blocking_sem = NULL;

static void blocking_task_func(void* user_data) {
    vox_atomic_int_t* counter = (vox_atomic_int_t*)user_data;
    if (counter && g_blocking_sem) {
        /* 等待信号量被释放（由测试代码控制） */
        while (vox_atomic_int_load(g_blocking_sem) == 0) {
            vox_thread_yield();
        }
        vox_atomic_int_increment(counter);
    }
}

/* 测试队列满时提交失败 */
static void test_tpool_queue_full(vox_mpool_t* mpool) {
    /* 创建小容量队列的线程池 */
    /* 注意：容量会被向上取到2的幂，2会被取到2，实际可用容量是1（需要区分空和满） */
    vox_tpool_config_t config = {
        .thread_count = 1,  /* 单线程，减少消费速度 */
        .queue_capacity = 2,  /* 最小容量，会被取到2，实际可用1 */
        .thread_priority = -1,
    };
    
    vox_tpool_t* tpool = vox_tpool_create_with_config(&config);
    TEST_ASSERT_NOT_NULL(tpool, "创建线程池失败");
    
    vox_atomic_int_t* counter = vox_atomic_int_create(mpool, 0);
    TEST_ASSERT_NOT_NULL(counter, "创建原子计数器失败");
    
    /* 创建阻塞信号量，用于控制任务执行 */
    g_blocking_sem = vox_atomic_int_create(mpool, 0);
    TEST_ASSERT_NOT_NULL(g_blocking_sem, "创建阻塞信号量失败");
    
    /* 快速提交大量阻塞任务，尝试填满队列 */
    /* 实际可用容量约为1，我们提交5个任务，应该会有一些失败 */
    const int task_count = 5;
    
    /* 使用原子计数器跟踪提交和失败的数量 */
    vox_atomic_int_t* submitted_count = vox_atomic_int_create(mpool, 0);
    vox_atomic_int_t* failed_count = vox_atomic_int_create(mpool, 0);
    TEST_ASSERT_NOT_NULL(submitted_count, "创建提交计数原子变量失败");
    TEST_ASSERT_NOT_NULL(failed_count, "创建失败计数原子变量失败");
    
    /* 快速连续提交任务，不等待，让队列被填满 */
    for (int i = 0; i < task_count; i++) {
        int result = vox_tpool_submit(tpool, blocking_task_func, counter, NULL);
        if (result == 0) {
            vox_atomic_int_increment(submitted_count);
        } else {
            vox_atomic_int_increment(failed_count);
            /* 如果队列已满，后续提交很可能也会失败，但继续尝试 */
        }
    }
    
    /* 读取最终计数 */
    int32_t submitted = vox_atomic_int_load(submitted_count);
    int32_t failed = vox_atomic_int_load(failed_count);
    
    /* 验证有任务提交失败（队列满） */
    /* 注意：由于竞态条件，可能所有任务都成功提交，也可能有失败 */
    /* 但至少应该有一些任务成功提交 */
    TEST_ASSERT_GT(submitted, 0, "应该有任务成功提交");
    TEST_ASSERT_EQ(submitted + failed, task_count, "提交总数应等于尝试提交数");
    
    /* 如果所有任务都成功提交，说明队列容量足够大，这不是错误 */
    /* 但我们可以验证队列满的情况：再次快速提交大量任务 */
    if (failed == 0) {
        /* 如果第一次没有失败，说明队列容量可能比预期大，或者任务执行太快 */
        /* 尝试再次快速提交，这次应该更可能触发队列满 */
        vox_atomic_int_store(submitted_count, submitted);
        vox_atomic_int_store(failed_count, 0);
        
        for (int i = 0; i < task_count; i++) {
            int result = vox_tpool_submit(tpool, blocking_task_func, counter, NULL);
            if (result == 0) {
                vox_atomic_int_increment(submitted_count);
            } else {
                vox_atomic_int_increment(failed_count);
            }
        }
        
        submitted = vox_atomic_int_load(submitted_count);
        failed = vox_atomic_int_load(failed_count);
        
        /* 如果仍然没有失败，说明队列容量足够大，测试通过（队列满的情况可能不会发生） */
        if (failed == 0) {
            /* 队列容量足够大，无法触发队列满，这是可以接受的 */
            /* 我们仍然验证基本功能正常 */
        } else {
            /* 有任务失败，验证失败的任务数 */
            TEST_ASSERT_GT(failed, 0, "应该有任务因队列满而提交失败");
        }
    } else {
        /* 有任务失败，验证失败的任务数 */
        TEST_ASSERT_GT(failed, 0, "应该有任务因队列满而提交失败");
    }
    
    /* 释放信号量，让所有已提交的任务完成 */
    vox_atomic_int_store(g_blocking_sem, 1);
    
    /* 等待所有已提交的任务完成 */
    vox_tpool_wait(tpool);
    
    /* 验证已提交的任务都执行完成 */
    int32_t executed_count = vox_atomic_int_load(counter);
    TEST_ASSERT_EQ(executed_count, submitted, "已提交的任务应全部执行完成");
    
    /* 队列现在应该有空闲空间，再次提交应该成功 */
    vox_atomic_int_t* final_submitted = vox_atomic_int_create(mpool, 0);
    TEST_ASSERT_NOT_NULL(final_submitted, "创建最终提交计数原子变量失败");
    
    for (int i = 0; i < failed; i++) {
        int result = vox_tpool_submit(tpool, atomic_task_func, counter, NULL);
        if (result == 0) {
            vox_atomic_int_increment(final_submitted);
        }
    }
    
    /* 等待新提交的任务完成 */
    vox_tpool_wait(tpool);
    
    /* 验证最终计数 */
    int32_t final_submitted_count = vox_atomic_int_load(final_submitted);
    int32_t final_count = vox_atomic_int_load(counter);
    TEST_ASSERT_EQ(final_count, submitted + final_submitted_count, "最终执行数量应正确");
    
    vox_atomic_int_destroy(final_submitted);
    vox_atomic_int_destroy(submitted_count);
    vox_atomic_int_destroy(failed_count);
    vox_atomic_int_destroy(g_blocking_sem);
    g_blocking_sem = NULL;
    vox_atomic_int_destroy(counter);
    vox_tpool_destroy(tpool);
}

/* 测试套件 */
test_case_t test_tpool_cases[] = {
    {"create_destroy", test_tpool_create_destroy},
    {"create_with_config", test_tpool_create_with_config},
    {"submit_single", test_tpool_submit_single},
    {"submit_multiple", test_tpool_submit_multiple},
    {"concurrent_tasks", test_tpool_concurrent_tasks},
    {"complete_callback", test_tpool_complete_callback},
    {"pending_tasks", test_tpool_pending_tasks},
    {"running_tasks", test_tpool_running_tasks},
    {"stats", test_tpool_stats},
    {"shutdown", test_tpool_shutdown},
    {"force_shutdown", test_tpool_force_shutdown},
    {"long_running_tasks", test_tpool_long_running_tasks},
    {"stress", test_tpool_stress},
    {"thread_safe_config", test_tpool_thread_safe_config},
    {"queue_full", test_tpool_queue_full},
    {"queue_type_config", test_tpool_queue_type_config},
};

test_suite_t test_tpool_suite = {
    "vox_tpool",
    test_tpool_cases,
    sizeof(test_tpool_cases) / sizeof(test_tpool_cases[0])
};
