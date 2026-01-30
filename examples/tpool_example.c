/*
 * tpool_example.c - 线程池示例程序
 * 演示 vox_tpool 的各种使用场景
 */

#include "../vox_tpool.h"
#include "../vox_mpool.h"
#include "../vox_atomic.h"
#include "../vox_mutex.h"
#include "../vox_thread.h"
#include <stdio.h>
#include <string.h>

/* ===== 任务函数 ===== */

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

/* 长时间运行的任务函数 */
static void long_task_func(void* user_data) {
    vox_atomic_int_t* counter = (vox_atomic_int_t*)user_data;
    if (counter) {
        vox_thread_sleep(10);  /* 休眠10毫秒 */
        vox_atomic_int_increment(counter);
    }
}

/* 任务完成回调数据 */
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

/* 带互斥锁的任务函数 */
typedef struct {
    int* counter;
    vox_mutex_t* mutex;
} mutex_task_data_t;

static void mutex_task_func(void* user_data) {
    mutex_task_data_t* data = (mutex_task_data_t*)user_data;
    if (data && data->mutex && data->counter) {
        vox_mutex_lock(data->mutex);
        (*data->counter)++;
        vox_mutex_unlock(data->mutex);
    }
}

/* 压力测试任务数据结构 */
typedef struct {
    vox_atomic_int_t* counter;
    int iterations;
} stress_task_data_t;

/* 压力测试任务函数 */
static void stress_task_func(void* user_data) {
    stress_task_data_t* data = (stress_task_data_t*)user_data;
    if (data && data->counter) {
        for (int i = 0; i < data->iterations; i++) {
            vox_atomic_int_increment(data->counter);
        }
    }
}

/* 阻塞任务函数 */
static vox_atomic_int_t* g_blocking_sem = NULL;

static void blocking_task_func(void* user_data) {
    vox_atomic_int_t* sem = (vox_atomic_int_t*)user_data;
    if (sem) {
        while (vox_atomic_int_load(sem) == 0) {
            vox_thread_yield();
        }
    }
}

/* ===== 测试函数 ===== */

/* 测试1: 基本创建和销毁 */
static void test_basic_create_destroy(void) {
    printf("\n=== 测试1: 基本创建和销毁 ===\n");
    
    /* 创建内存池 */
    vox_mpool_t* mpool = vox_mpool_create();
    if (!mpool) {
        fprintf(stderr, "创建内存池失败\n");
        return;
    }
    
    /* 创建线程池 */
    vox_tpool_t* tpool = vox_tpool_create();
    if (!tpool) {
        fprintf(stderr, "创建线程池失败\n");
        vox_mpool_destroy(mpool);
        return;
    }
    
    printf("线程池创建成功\n");
    
    /* 销毁线程池 */
    vox_tpool_destroy(tpool);
    printf("线程池销毁成功\n");
    
    vox_mpool_destroy(mpool);
}

/* 测试2: 提交单个任务 */
static void test_submit_single(void) {
    printf("\n=== 测试2: 提交单个任务 ===\n");
    
    /* 创建线程安全的内存池 */
    vox_mpool_config_t mpool_config = {0};
    mpool_config.thread_safe = 1;
    vox_mpool_t* mpool = vox_mpool_create_with_config(&mpool_config);
    if (!mpool) {
        fprintf(stderr, "创建内存池失败\n");
        return;
    }
    
    vox_tpool_t* tpool = vox_tpool_create();
    if (!tpool) {
        fprintf(stderr, "创建线程池失败\n");
        vox_mpool_destroy(mpool);
        return;
    }
    
    vox_atomic_int_t* counter = vox_atomic_int_create(mpool, 0);
    if (!counter) {
        fprintf(stderr, "创建原子计数器失败\n");
        vox_tpool_destroy(tpool);
        vox_mpool_destroy(mpool);
        return;
    }
    
    printf("提交单个任务...\n");
    if (vox_tpool_submit(tpool, atomic_task_func, counter, NULL) == 0) {
        printf("任务提交成功\n");
        
        /* 等待任务完成 */
        vox_tpool_wait(tpool);
        
        int32_t value = vox_atomic_int_load(counter);
        printf("任务执行完成，计数器值: %d (期望: 1)\n", value);
    } else {
        printf("任务提交失败\n");
    }
    
    vox_atomic_int_destroy(counter);
    vox_tpool_destroy(tpool);
    vox_mpool_destroy(mpool);
}

/* 测试3: 提交多个任务 */
static void test_submit_multiple(void) {
    printf("\n=== 测试3: 提交多个任务 ===\n");
    
    /* 创建线程安全的内存池 */
    vox_mpool_config_t mpool_config = {0};
    mpool_config.thread_safe = 1;
    vox_mpool_t* mpool = vox_mpool_create_with_config(&mpool_config);
    if (!mpool) {
        fprintf(stderr, "创建内存池失败\n");
        return;
    }
    
    vox_tpool_t* tpool = vox_tpool_create();
    if (!tpool) {
        fprintf(stderr, "创建线程池失败\n");
        vox_mpool_destroy(mpool);
        return;
    }
    
    vox_atomic_int_t* counter = vox_atomic_int_create(mpool, 0);
    if (!counter) {
        fprintf(stderr, "创建原子计数器失败\n");
        vox_tpool_destroy(tpool);
        vox_mpool_destroy(mpool);
        return;
    }
    
    const int task_count = 100;
    printf("提交 %d 个任务...\n", task_count);
    
    int submitted = 0;
    for (int i = 0; i < task_count; i++) {
        if (vox_tpool_submit(tpool, atomic_task_func, counter, NULL) == 0) {
            submitted++;
        }
    }
    
    printf("成功提交 %d 个任务\n", submitted);
    
    /* 等待所有任务完成 */
    vox_tpool_wait(tpool);
    
    int32_t value = vox_atomic_int_load(counter);
    printf("所有任务执行完成，计数器值: %d (期望: %d)\n", value, task_count);
    
    vox_atomic_int_destroy(counter);
    vox_tpool_destroy(tpool);
    vox_mpool_destroy(mpool);
}

/* 测试4: 并发任务执行 */
static void test_concurrent_tasks(void) {
    printf("\n=== 测试4: 并发任务执行 ===\n");
    
    /* 创建线程安全的内存池 */
    vox_mpool_config_t mpool_config = {0};
    mpool_config.thread_safe = 1;
    vox_mpool_t* mpool = vox_mpool_create_with_config(&mpool_config);
    if (!mpool) {
        fprintf(stderr, "创建内存池失败\n");
        return;
    }
    
    vox_tpool_t* tpool = vox_tpool_create();
    if (!tpool) {
        fprintf(stderr, "创建线程池失败\n");
        vox_mpool_destroy(mpool);
        return;
    }
    
    /* 使用栈空间分配互斥锁结构体 */
    vox_mutex_t mutex;
    if (vox_mutex_create(&mutex) != 0) {
        fprintf(stderr, "创建互斥锁失败\n");
        vox_tpool_destroy(tpool);
        vox_mpool_destroy(mpool);
        return;
    }
    
    int shared_counter = 0;
    const int task_count = 50;
    
    printf("提交 %d 个并发任务（使用互斥锁保护）...\n", task_count);
    
    mutex_task_data_t task_data = {
        .counter = &shared_counter,
        .mutex = &mutex
    };
    
    for (int i = 0; i < task_count; i++) {
        vox_tpool_submit(tpool, mutex_task_func, &task_data, NULL);
    }
    
    /* 等待所有任务完成 */
    vox_tpool_wait(tpool);
    
    printf("所有任务执行完成，计数器值: %d (期望: %d)\n", shared_counter, task_count);
    
    vox_mutex_destroy(&mutex);
    vox_tpool_destroy(tpool);
    vox_mpool_destroy(mpool);
}

/* 测试5: 任务完成回调 */
static void test_complete_callback(void) {
    printf("\n=== 测试5: 任务完成回调 ===\n");
    
    /* 创建线程安全的内存池 */
    vox_mpool_config_t mpool_config = {0};
    mpool_config.thread_safe = 1;
    vox_mpool_t* mpool = vox_mpool_create_with_config(&mpool_config);
    if (!mpool) {
        fprintf(stderr, "创建内存池失败\n");
        return;
    }
    
    vox_tpool_t* tpool = vox_tpool_create();
    if (!tpool) {
        fprintf(stderr, "创建线程池失败\n");
        vox_mpool_destroy(mpool);
        return;
    }
    
    /* 使用栈空间分配互斥锁结构体 */
    vox_mutex_t mutex;
    if (vox_mutex_create(&mutex) != 0) {
        fprintf(stderr, "创建互斥锁失败\n");
        vox_tpool_destroy(tpool);
        vox_mpool_destroy(mpool);
        return;
    }
    
    g_callback_mutex = &mutex;
    g_callback_count = 0;
    
    vox_atomic_int_t* counter = vox_atomic_int_create(mpool, 0);
    if (!counter) {
        fprintf(stderr, "创建原子计数器失败\n");
        vox_mutex_destroy(&mutex);
        vox_tpool_destroy(tpool);
        vox_mpool_destroy(mpool);
        return;
    }
    
    const int task_count = 30;
    printf("提交 %d 个带回调的任务...\n", task_count);
    
    for (int i = 0; i < task_count; i++) {
        vox_tpool_submit(tpool, atomic_task_func, counter, task_complete_callback);
    }
    
    /* 等待所有任务完成 */
    vox_tpool_wait(tpool);
    
    int32_t value = vox_atomic_int_load(counter);
    printf("任务执行完成，计数器值: %d (期望: %d)\n", value, task_count);
    printf("回调函数调用次数: %d (期望: %d)\n", g_callback_count, task_count);
    
    g_callback_mutex = NULL;
    vox_mutex_destroy(&mutex);
    vox_atomic_int_destroy(counter);
    vox_tpool_destroy(tpool);
    vox_mpool_destroy(mpool);
}

/* 测试6: 队列状态查询 */
static void test_queue_status(void) {
    printf("\n=== 测试6: 队列状态查询 ===\n");
    
    /* 创建线程安全的内存池 */
    vox_mpool_config_t mpool_config = {0};
    mpool_config.thread_safe = 1;
    vox_mpool_t* mpool = vox_mpool_create_with_config(&mpool_config);
    if (!mpool) {
        fprintf(stderr, "创建内存池失败\n");
        return;
    }
    
    vox_tpool_t* tpool = vox_tpool_create();
    if (!tpool) {
        fprintf(stderr, "创建线程池失败\n");
        vox_mpool_destroy(mpool);
        return;
    }
    
    printf("初始状态:\n");
    printf("  待处理任务数: %zu\n", vox_tpool_pending_tasks(tpool));
    printf("  正在执行任务数: %zu\n", vox_tpool_running_tasks(tpool));
    
    vox_atomic_int_t* counter = vox_atomic_int_create(mpool, 0);
    if (!counter) {
        fprintf(stderr, "创建原子计数器失败\n");
        vox_tpool_destroy(tpool);
        vox_mpool_destroy(mpool);
        return;
    }
    
    const int task_count = 20;
    printf("\n提交 %d 个任务...\n", task_count);
    
    for (int i = 0; i < task_count; i++) {
        vox_tpool_submit(tpool, atomic_task_func, counter, NULL);
    }
    
    printf("提交后状态:\n");
    printf("  待处理任务数: %zu\n", vox_tpool_pending_tasks(tpool));
    printf("  正在执行任务数: %zu\n", vox_tpool_running_tasks(tpool));
    
    /* 等待所有任务完成 */
    vox_tpool_wait(tpool);
    
    printf("\n完成后状态:\n");
    printf("  待处理任务数: %zu (期望: 0)\n", vox_tpool_pending_tasks(tpool));
    printf("  正在执行任务数: %zu (期望: 0)\n", vox_tpool_running_tasks(tpool));
    
    int32_t value = vox_atomic_int_load(counter);
    printf("  任务执行数量: %d (期望: %d)\n", value, task_count);
    
    vox_atomic_int_destroy(counter);
    vox_tpool_destroy(tpool);
    vox_mpool_destroy(mpool);
}

/* 测试7: 统计信息 */
static void test_stats(void) {
    printf("\n=== 测试7: 统计信息 ===\n");
    
    /* 创建线程安全的内存池 */
    vox_mpool_config_t mpool_config = {0};
    mpool_config.thread_safe = 1;
    vox_mpool_t* mpool = vox_mpool_create_with_config(&mpool_config);
    if (!mpool) {
        fprintf(stderr, "创建内存池失败\n");
        return;
    }
    
    vox_tpool_t* tpool = vox_tpool_create();
    if (!tpool) {
        fprintf(stderr, "创建线程池失败\n");
        vox_mpool_destroy(mpool);
        return;
    }
    
    size_t total_tasks = 0;
    size_t completed_tasks = 0;
    size_t failed_tasks = 0;
    
    printf("初始统计信息:\n");
    vox_tpool_stats(tpool, &total_tasks, &completed_tasks, &failed_tasks);
    printf("  总任务数: %zu\n", total_tasks);
    printf("  已完成任务数: %zu\n", completed_tasks);
    printf("  失败任务数: %zu\n", failed_tasks);
    
    vox_atomic_int_t* counter = vox_atomic_int_create(mpool, 0);
    if (!counter) {
        fprintf(stderr, "创建原子计数器失败\n");
        vox_tpool_destroy(tpool);
        vox_mpool_destroy(mpool);
        return;
    }
    
    const int task_count = 25;
    printf("\n提交 %d 个任务...\n", task_count);
    
    for (int i = 0; i < task_count; i++) {
        vox_tpool_submit(tpool, atomic_task_func, counter, NULL);
    }
    
    /* 等待所有任务完成 */
    vox_tpool_wait(tpool);
    
    printf("\n完成后统计信息:\n");
    vox_tpool_stats(tpool, &total_tasks, &completed_tasks, &failed_tasks);
    printf("  总任务数: %zu (期望: %d)\n", total_tasks, task_count);
    printf("  已完成任务数: %zu (期望: %d)\n", completed_tasks, task_count);
    printf("  失败任务数: %zu (期望: 0)\n", failed_tasks);
    
    int32_t value = vox_atomic_int_load(counter);
    printf("  任务执行数量: %d (期望: %d)\n", value, task_count);
    
    vox_atomic_int_destroy(counter);
    vox_tpool_destroy(tpool);
    vox_mpool_destroy(mpool);
}

/* 测试8: 优雅关闭 */
static void test_shutdown(void) {
    printf("\n=== 测试8: 优雅关闭 ===\n");
    
    /* 创建线程安全的内存池 */
    vox_mpool_config_t mpool_config = {0};
    mpool_config.thread_safe = 1;
    vox_mpool_t* mpool = vox_mpool_create_with_config(&mpool_config);
    if (!mpool) {
        fprintf(stderr, "创建内存池失败\n");
        return;
    }
    
    vox_tpool_t* tpool = vox_tpool_create();
    if (!tpool) {
        fprintf(stderr, "创建线程池失败\n");
        vox_mpool_destroy(mpool);
        return;
    }
    
    vox_atomic_int_t* counter = vox_atomic_int_create(mpool, 0);
    if (!counter) {
        fprintf(stderr, "创建原子计数器失败\n");
        vox_tpool_destroy(tpool);
        vox_mpool_destroy(mpool);
        return;
    }
    
    const int task_count = 40;
    printf("提交 %d 个任务...\n", task_count);
    
    for (int i = 0; i < task_count; i++) {
        vox_tpool_submit(tpool, atomic_task_func, counter, NULL);
    }
    
    printf("执行优雅关闭（等待所有任务完成）...\n");
    if (vox_tpool_shutdown(tpool) == 0) {
        printf("线程池已关闭\n");
        
        int32_t value = vox_atomic_int_load(counter);
        printf("任务执行数量: %d (期望: %d)\n", value, task_count);
        
        /* 关闭后不应接受新任务 */
        int test_counter = 0;
        if (vox_tpool_submit(tpool, simple_task_func, &test_counter, NULL) != 0) {
            printf("关闭后拒绝新任务（正确）\n");
        } else {
            printf("警告：关闭后仍接受新任务\n");
        }
    } else {
        printf("关闭线程池失败\n");
    }
    
    vox_atomic_int_destroy(counter);
    vox_tpool_destroy(tpool);
    vox_mpool_destroy(mpool);
}

/* 测试9: 强制关闭 */
static void test_force_shutdown(void) {
    printf("\n=== 测试9: 强制关闭 ===\n");
    
    /* 创建线程安全的内存池 */
    vox_mpool_config_t mpool_config = {0};
    mpool_config.thread_safe = 1;
    vox_mpool_t* mpool = vox_mpool_create_with_config(&mpool_config);
    if (!mpool) {
        fprintf(stderr, "创建内存池失败\n");
        return;
    }
    
    vox_tpool_t* tpool = vox_tpool_create();
    if (!tpool) {
        fprintf(stderr, "创建线程池失败\n");
        vox_mpool_destroy(mpool);
        return;
    }
    
    vox_atomic_int_t* counter = vox_atomic_int_create(mpool, 0);
    if (!counter) {
        fprintf(stderr, "创建原子计数器失败\n");
        vox_tpool_destroy(tpool);
        vox_mpool_destroy(mpool);
        return;
    }
    
    const int task_count = 15;
    printf("提交 %d 个长时间运行的任务...\n", task_count);
    
    for (int i = 0; i < task_count; i++) {
        vox_tpool_submit(tpool, long_task_func, counter, NULL);
    }
    
    printf("执行强制关闭（不等待任务完成）...\n");
    vox_tpool_force_shutdown(tpool);
    printf("线程池已强制关闭\n");
    
    /* 关闭后不应接受新任务 */
    int test_counter = 0;
    if (vox_tpool_submit(tpool, simple_task_func, &test_counter, NULL) != 0) {
        printf("关闭后拒绝新任务（正确）\n");
    } else {
        printf("警告：关闭后仍接受新任务\n");
    }
    
    vox_atomic_int_destroy(counter);
    vox_tpool_destroy(tpool);
    vox_mpool_destroy(mpool);
}

/* 测试10: 长时间运行的任务 */
static void test_long_running_tasks(void) {
    printf("\n=== 测试10: 长时间运行的任务 ===\n");
    
    /* 创建线程安全的内存池 */
    vox_mpool_config_t mpool_config = {0};
    mpool_config.thread_safe = 1;
    vox_mpool_t* mpool = vox_mpool_create_with_config(&mpool_config);
    if (!mpool) {
        fprintf(stderr, "创建内存池失败\n");
        return;
    }
    
    vox_tpool_t* tpool = vox_tpool_create();
    if (!tpool) {
        fprintf(stderr, "创建线程池失败\n");
        vox_mpool_destroy(mpool);
        return;
    }
    
    vox_atomic_int_t* counter = vox_atomic_int_create(mpool, 0);
    if (!counter) {
        fprintf(stderr, "创建原子计数器失败\n");
        vox_tpool_destroy(tpool);
        vox_mpool_destroy(mpool);
        return;
    }
    
    const int task_count = 10;
    printf("提交 %d 个长时间运行的任务（每个休眠10ms）...\n", task_count);
    
    for (int i = 0; i < task_count; i++) {
        vox_tpool_submit(tpool, long_task_func, counter, NULL);
    }
    
    /* 等待所有任务完成 */
    vox_tpool_wait(tpool);
    
    int32_t value = vox_atomic_int_load(counter);
    printf("所有任务执行完成，计数器值: %d (期望: %d)\n", value, task_count);
    
    vox_atomic_int_destroy(counter);
    vox_tpool_destroy(tpool);
    vox_mpool_destroy(mpool);
}

/* 测试11: 压力测试 */
static void test_stress(void) {
    printf("\n=== 测试11: 压力测试 ===\n");
    
    /* 创建线程安全的内存池 */
    vox_mpool_config_t mpool_config = {0};
    mpool_config.thread_safe = 1;
    vox_mpool_t* mpool = vox_mpool_create_with_config(&mpool_config);
    if (!mpool) {
        fprintf(stderr, "创建内存池失败\n");
        return;
    }
    
    vox_tpool_t* tpool = vox_tpool_create();
    if (!tpool) {
        fprintf(stderr, "创建线程池失败\n");
        vox_mpool_destroy(mpool);
        return;
    }
    
    vox_atomic_int_t* counter = vox_atomic_int_create(mpool, 0);
    if (!counter) {
        fprintf(stderr, "创建原子计数器失败\n");
        vox_tpool_destroy(tpool);
        vox_mpool_destroy(mpool);
        return;
    }
    
    stress_task_data_t task_data = {
        .counter = counter,
        .iterations = 100
    };
    
    const int task_count = 500;
    printf("提交 %d 个压力测试任务（每个任务执行100次递增）...\n", task_count);
    
    for (int i = 0; i < task_count; i++) {
        vox_tpool_submit(tpool, stress_task_func, &task_data, NULL);
    }
    
    /* 等待所有任务完成 */
    vox_tpool_wait(tpool);
    
    int32_t value = vox_atomic_int_load(counter);
    int32_t expected = task_count * task_data.iterations;
    printf("所有任务执行完成，计数器值: %d (期望: %d)\n", value, expected);
    
    vox_atomic_int_destroy(counter);
    vox_tpool_destroy(tpool);
    vox_mpool_destroy(mpool);
}

/* 测试12: 自定义配置 */
static void test_custom_config(void) {
    printf("\n=== 测试12: 自定义配置 ===\n");
    
    /* 创建线程安全的内存池 */
    vox_mpool_config_t mpool_config = {0};
    mpool_config.thread_safe = 1;
    vox_mpool_t* mpool = vox_mpool_create_with_config(&mpool_config);
    if (!mpool) {
        fprintf(stderr, "创建内存池失败\n");
        return;
    }
    
    /* 测试非线程安全配置 */
    printf("测试非线程安全配置...\n");
    vox_tpool_config_t config1 = {
        .thread_count = 2,
        .queue_capacity = 64,
        .thread_priority = -1,
    };
    
    vox_tpool_t* tpool1 = vox_tpool_create_with_config(&config1);
    if (tpool1) {
        printf("非线程安全线程池创建成功\n");
        
        vox_atomic_int_t* counter1 = vox_atomic_int_create(mpool, 0);
        if (counter1) {
            vox_tpool_submit(tpool1, atomic_task_func, counter1, NULL);
            vox_tpool_wait(tpool1);
            
            int32_t value = vox_atomic_int_load(counter1);
            printf("任务执行完成，计数器值: %d (期望: 1)\n", value);
            
            vox_atomic_int_destroy(counter1);
        }
        
        vox_tpool_destroy(tpool1);
    }
    
    /* 测试线程安全配置 */
    printf("\n测试线程安全配置...\n");
    vox_tpool_config_t config2 = {
        .thread_count = 4,
        .queue_capacity = 128,
        .thread_priority = -1,
    };
    
    vox_tpool_t* tpool2 = vox_tpool_create_with_config(&config2);
    if (tpool2) {
        printf("线程安全线程池创建成功\n");
        
        vox_atomic_int_t* counter2 = vox_atomic_int_create(mpool, 0);
        if (counter2) {
            const int task_count = 20;
            for (int i = 0; i < task_count; i++) {
                vox_tpool_submit(tpool2, atomic_task_func, counter2, NULL);
            }
            
            vox_tpool_wait(tpool2);
            
            int32_t value = vox_atomic_int_load(counter2);
            printf("任务执行完成，计数器值: %d (期望: %d)\n", value, task_count);
            
            vox_atomic_int_destroy(counter2);
        }
        
        vox_tpool_destroy(tpool2);
    }
    
    vox_mpool_destroy(mpool);
}

/* 测试13: 队列满的情况 */
static void test_queue_full(void) {
    printf("\n=== 测试13: 队列满的情况 ===\n");
    
    /* 创建线程安全的内存池 */
    vox_mpool_config_t mpool_config = {0};
    mpool_config.thread_safe = 1;
    vox_mpool_t* mpool = vox_mpool_create_with_config(&mpool_config);
    if (!mpool) {
        fprintf(stderr, "创建内存池失败\n");
        return;
    }
    
    /* 创建小容量队列的线程池 */
    vox_tpool_config_t config = {
        .thread_count = 1,
        .queue_capacity = 4,  /* 小容量，容易触发队列满 */
        .thread_priority = -1,
    };
    
    vox_tpool_t* tpool = vox_tpool_create_with_config(&config);
    if (!tpool) {
        fprintf(stderr, "创建线程池失败\n");
        vox_mpool_destroy(mpool);
        return;
    }
    
    vox_atomic_int_t* counter = vox_atomic_int_create(mpool, 0);
    if (!counter) {
        fprintf(stderr, "创建原子计数器失败\n");
        vox_tpool_destroy(tpool);
        vox_mpool_destroy(mpool);
        return;
    }
    
    /* 创建阻塞信号量 */
    g_blocking_sem = vox_atomic_int_create(mpool, 0);
    if (!g_blocking_sem) {
        fprintf(stderr, "创建阻塞信号量失败\n");
        vox_atomic_int_destroy(counter);
        vox_tpool_destroy(tpool);
        vox_mpool_destroy(mpool);
        return;
    }
    
    const int task_count = 10;
    int submitted = 0;
    int failed = 0;
    
    printf("尝试提交 %d 个任务到小容量队列（容量约3）...\n", task_count);
    
    /* 快速提交任务 */
    for (int i = 0; i < task_count; i++) {
        int result = vox_tpool_submit(tpool, blocking_task_func, g_blocking_sem, NULL);
        if (result == 0) {
            submitted++;
        } else {
            failed++;
        }
    }
    
    printf("提交结果: 成功 %d, 失败 %d\n", submitted, failed);
    
    if (failed > 0) {
        printf("队列满的情况已触发（正确）\n");
    } else {
        printf("注意：未触发队列满（可能队列容量足够大或任务执行太快）\n");
    }
    
    /* 释放信号量，让任务完成 */
    vox_atomic_int_store(g_blocking_sem, 1);
    vox_tpool_wait(tpool);
    
    printf("所有已提交的任务已完成\n");
    
    vox_atomic_int_destroy(g_blocking_sem);
    g_blocking_sem = NULL;
    vox_atomic_int_destroy(counter);
    vox_tpool_destroy(tpool);
    vox_mpool_destroy(mpool);
}

int main(void) {
    printf("=== vox_tpool 线程池示例程序 ===\n");
    
    /* 运行所有测试 */
    test_basic_create_destroy();
    test_submit_single();
    test_submit_multiple();
    test_concurrent_tasks();
    test_complete_callback();
    test_queue_status();
    test_stats();
    test_shutdown();
    test_force_shutdown();
    test_long_running_tasks();
    test_stress();
    test_custom_config();
    test_queue_full();
    
    printf("\n=== 所有测试完成 ===\n");
    return 0;
}
