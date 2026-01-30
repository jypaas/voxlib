/*
 * mutex_example.c - 线程同步原语示例程序
 * 演示 vox_mutex 的各种同步机制
 */

#include "../vox_mutex.h"
#include "../vox_thread.h"
#include "../vox_mpool.h"
#include <stdio.h>

#define NUM_THREADS 3

/* ===== 测试数据 ===== */
static int shared_counter = 0;
static int shared_data = 0;

/* ===== 互斥锁测试 ===== */

typedef struct {
    int thread_id;
    vox_mutex_t* mutex;
    int iterations;
} mutex_test_data_t;

static int mutex_worker(void* user_data) {
    mutex_test_data_t* data = (mutex_test_data_t*)user_data;
    
    for (int i = 0; i < data->iterations; i++) {
        vox_mutex_lock(data->mutex);
        shared_counter++;
        printf("  线程 %d: 计数器 = %d\n", data->thread_id, shared_counter);
        vox_mutex_unlock(data->mutex);
        vox_thread_sleep(10);
    }
    
    return 0;
}

static void test_mutex(void) {
    printf("\n=== 测试互斥锁 ===\n");
    
    /* 创建线程安全的内存池（用于多线程环境） */
    vox_mpool_config_t mpool_config = {0};
    mpool_config.thread_safe = 1;  /* 多线程环境需要启用线程安全 */
    vox_mpool_t* mpool = vox_mpool_create_with_config(&mpool_config);
    if (!mpool) {
        fprintf(stderr, "创建内存池失败\n");
        return;
    }
    
    shared_counter = 0;
    
    /* 使用栈空间分配互斥锁结构体 */
    vox_mutex_t mutex;
    if (vox_mutex_create(&mutex) != 0) {
        fprintf(stderr, "创建互斥锁失败\n");
        vox_mpool_destroy(mpool);
        return;
    }
    
    vox_thread_t* threads[NUM_THREADS];
    mutex_test_data_t data[NUM_THREADS];
    
    printf("创建 %d 个线程竞争共享资源...\n", NUM_THREADS);
    for (int i = 0; i < NUM_THREADS; i++) {
        data[i].thread_id = i + 1;
        data[i].mutex = &mutex;
        data[i].iterations = 5;
        threads[i] = vox_thread_create(mpool, mutex_worker, &data[i]);
    }
    
    for (int i = 0; i < NUM_THREADS; i++) {
        if (threads[i]) {
            vox_thread_join(threads[i], NULL);
        }
    }
    
    printf("最终计数器值: %d (期望: %d)\n", shared_counter, NUM_THREADS * 5);
    vox_mutex_destroy(&mutex);
    vox_mpool_destroy(mpool);
}

/* ===== 读写锁测试 ===== */

typedef struct {
    int thread_id;
    vox_rwlock_t* rwlock;
    bool is_reader;
} rwlock_test_data_t;

static int rwlock_worker(void* user_data) {
    rwlock_test_data_t* data = (rwlock_test_data_t*)user_data;
    
    if (data->is_reader) {
        /* 读操作 */
        vox_rwlock_rdlock(data->rwlock);
        printf("  读线程 %d: 读取数据 = %d\n", data->thread_id, shared_data);
        vox_thread_sleep(50);
        vox_rwlock_unlock(data->rwlock);
    } else {
        /* 写操作 */
        vox_rwlock_wrlock(data->rwlock);
        shared_data++;
        printf("  写线程 %d: 写入数据 = %d\n", data->thread_id, shared_data);
        vox_thread_sleep(50);
        vox_rwlock_unlock(data->rwlock);
    }
    
    return 0;
}

static void test_rwlock(void) {
    printf("\n=== 测试读写锁 ===\n");
    
    /* 创建线程安全的内存池（用于多线程环境） */
    vox_mpool_config_t mpool_config = {0};
    mpool_config.thread_safe = 1;  /* 多线程环境需要启用线程安全 */
    vox_mpool_t* mpool = vox_mpool_create_with_config(&mpool_config);
    if (!mpool) {
        fprintf(stderr, "创建内存池失败\n");
        return;
    }
    
    shared_data = 0;
    
    /* 使用栈空间分配读写锁结构体 */
    vox_rwlock_t rwlock;
    if (vox_rwlock_create(&rwlock) != 0) {
        fprintf(stderr, "创建读写锁失败\n");
        vox_mpool_destroy(mpool);
        return;
    }
    
    vox_thread_t* threads[5];
    rwlock_test_data_t data[5];
    
    /* 创建2个写线程和3个读线程 */
    printf("创建2个写线程和3个读线程...\n");
    for (int i = 0; i < 5; i++) {
        data[i].thread_id = i + 1;
        data[i].rwlock = &rwlock;
        data[i].is_reader = (i >= 2);  /* 前2个是写线程，后3个是读线程 */
        threads[i] = vox_thread_create(mpool, rwlock_worker, &data[i]);
    }
    
    for (int i = 0; i < 5; i++) {
        if (threads[i]) {
            vox_thread_join(threads[i], NULL);
        }
    }
    
    printf("最终数据值: %d\n", shared_data);
    vox_rwlock_destroy(&rwlock);
    vox_mpool_destroy(mpool);
}

/* ===== 递归锁测试 ===== */

static int recursive_function(vox_rmutex_t* rmutex, int depth) {
    if (depth <= 0) return 0;
    
    vox_rmutex_lock(rmutex);
    printf("  递归深度 %d: 已加锁\n", depth);
    
    if (depth > 1) {
        recursive_function(rmutex, depth - 1);
    }
    
    printf("  递归深度 %d: 准备解锁\n", depth);
    vox_rmutex_unlock(rmutex);
    return 0;
}

typedef struct {
    int thread_id;
    vox_rmutex_t* rmutex;
} rmutex_test_data_t;

static int rmutex_worker(void* user_data) {
    rmutex_test_data_t* data = (rmutex_test_data_t*)user_data;
    
    printf("  线程 %d 开始递归加锁...\n", data->thread_id);
    recursive_function(data->rmutex, 3);
    printf("  线程 %d 完成递归加锁\n", data->thread_id);
    
    return 0;
}

static void test_rmutex(void) {
    printf("\n=== 测试递归锁 ===\n");
    
    /* 创建线程安全的内存池（用于多线程环境） */
    vox_mpool_config_t mpool_config = {0};
    mpool_config.thread_safe = 1;  /* 多线程环境需要启用线程安全 */
    vox_mpool_t* mpool = vox_mpool_create_with_config(&mpool_config);
    if (!mpool) {
        fprintf(stderr, "创建内存池失败\n");
        return;
    }
    
    /* 使用栈空间分配递归锁结构体 */
    vox_rmutex_t rmutex;
    if (vox_rmutex_create(&rmutex) != 0) {
        fprintf(stderr, "创建递归锁失败\n");
        vox_mpool_destroy(mpool);
        return;
    }
    
    vox_thread_t* threads[NUM_THREADS];
    rmutex_test_data_t data[NUM_THREADS];
    
    printf("创建 %d 个线程测试递归加锁...\n", NUM_THREADS);
    for (int i = 0; i < NUM_THREADS; i++) {
        data[i].thread_id = i + 1;
        data[i].rmutex = &rmutex;
        threads[i] = vox_thread_create(mpool, rmutex_worker, &data[i]);
    }
    
    for (int i = 0; i < NUM_THREADS; i++) {
        if (threads[i]) {
            vox_thread_join(threads[i], NULL);
        }
    }
    
    vox_rmutex_destroy(&rmutex);
    vox_mpool_destroy(mpool);
}

/* ===== 自旋锁测试 ===== */

typedef struct {
    int thread_id;
    vox_spinlock_t* spinlock;
    int iterations;
} spinlock_test_data_t;

static int spinlock_worker(void* user_data) {
    spinlock_test_data_t* data = (spinlock_test_data_t*)user_data;
    
    for (int i = 0; i < data->iterations; i++) {
        vox_spinlock_lock(data->spinlock);
        shared_counter++;
        printf("  线程 %d: 自旋锁保护，计数器 = %d\n", data->thread_id, shared_counter);
        vox_spinlock_unlock(data->spinlock);
        vox_thread_yield();  /* 让出CPU */
    }
    
    return 0;
}

static void test_spinlock(void) {
    printf("\n=== 测试自旋锁 ===\n");
    
    /* 创建线程安全的内存池（用于多线程环境） */
    vox_mpool_config_t mpool_config = {0};
    mpool_config.thread_safe = 1;  /* 多线程环境需要启用线程安全 */
    vox_mpool_t* mpool = vox_mpool_create_with_config(&mpool_config);
    if (!mpool) {
        fprintf(stderr, "创建内存池失败\n");
        return;
    }
    
    shared_counter = 0;
    
    /* 使用栈空间分配自旋锁结构体 */
    vox_spinlock_t spinlock;
    if (vox_spinlock_create(&spinlock) != 0) {
        fprintf(stderr, "创建自旋锁失败\n");
        vox_mpool_destroy(mpool);
        return;
    }
    
    vox_thread_t* threads[NUM_THREADS];
    spinlock_test_data_t data[NUM_THREADS];
    
    printf("创建 %d 个线程使用自旋锁...\n", NUM_THREADS);
    for (int i = 0; i < NUM_THREADS; i++) {
        data[i].thread_id = i + 1;
        data[i].spinlock = &spinlock;
        data[i].iterations = 3;
        threads[i] = vox_thread_create(mpool, spinlock_worker, &data[i]);
    }
    
    for (int i = 0; i < NUM_THREADS; i++) {
        if (threads[i]) {
            vox_thread_join(threads[i], NULL);
        }
    }
    
    printf("最终计数器值: %d\n", shared_counter);
    vox_spinlock_destroy(&spinlock);
    vox_mpool_destroy(mpool);
}

/* ===== 信号量测试 ===== */

typedef struct {
    int thread_id;
    vox_semaphore_t* sem;
} semaphore_test_data_t;

static int semaphore_producer(void* user_data) {
    semaphore_test_data_t* data = (semaphore_test_data_t*)user_data;
    
    for (int i = 0; i < 3; i++) {
        vox_thread_sleep(100);
        printf("  生产者线程 %d: 生产一个资源\n", data->thread_id);
        vox_semaphore_post(data->sem);
    }
    
    return 0;
}

static int semaphore_consumer(void* user_data) {
    semaphore_test_data_t* data = (semaphore_test_data_t*)user_data;
    
    for (int i = 0; i < 2; i++) {
        printf("  消费者线程 %d: 等待资源...\n", data->thread_id);
        vox_semaphore_wait(data->sem);
        printf("  消费者线程 %d: 消费一个资源\n", data->thread_id);
    }
    
    return 0;
}

static void test_semaphore(void) {
    printf("\n=== 测试信号量 ===\n");
    
    /* 创建线程安全的内存池（用于多线程环境） */
    vox_mpool_config_t mpool_config = {0};
    mpool_config.thread_safe = 1;  /* 多线程环境需要启用线程安全 */
    vox_mpool_t* mpool = vox_mpool_create_with_config(&mpool_config);
    if (!mpool) {
        fprintf(stderr, "创建内存池失败\n");
        return;
    }
    
    /* 使用栈空间分配信号量结构体 */
    vox_semaphore_t sem;
    if (vox_semaphore_create(&sem, 0) != 0) {  /* 初始值为0 */
        fprintf(stderr, "创建信号量失败\n");
        vox_mpool_destroy(mpool);
        return;
    }
    
    printf("创建2个生产者线程和2个消费者线程...\n");
    printf("信号量初始值: %d\n", vox_semaphore_get_value(&sem));
    
    vox_thread_t* threads[4];
    semaphore_test_data_t data[4];
    
    /* 创建2个消费者 */
    for (int i = 0; i < 2; i++) {
        data[i].thread_id = i + 1;
        data[i].sem = &sem;
        threads[i] = vox_thread_create(mpool, semaphore_consumer, &data[i]);
    }
    
    vox_thread_sleep(50);
    
    /* 创建2个生产者 */
    for (int i = 2; i < 4; i++) {
        data[i].thread_id = i - 1;
        data[i].sem = &sem;
        threads[i] = vox_thread_create(mpool, semaphore_producer, &data[i]);
    }
    
    for (int i = 0; i < 4; i++) {
        if (threads[i]) {
            vox_thread_join(threads[i], NULL);
        }
    }
    
    printf("信号量最终值: %d\n", vox_semaphore_get_value(&sem));
    vox_semaphore_destroy(&sem);
    vox_mpool_destroy(mpool);
}

/* ===== 信号量超时测试 ===== */

static void test_semaphore_timeout(void) {
    printf("\n=== 测试信号量超时 ===\n");
    
    /* 创建内存池（单线程环境，不需要线程安全） */
    vox_mpool_t* mpool = vox_mpool_create();
    if (!mpool) {
        fprintf(stderr, "创建内存池失败\n");
        return;
    }
    
    /* 使用栈空间分配信号量结构体 */
    vox_semaphore_t sem;
    if (vox_semaphore_create(&sem, 0) != 0) {
        fprintf(stderr, "创建信号量失败\n");
        vox_mpool_destroy(mpool);
        return;
    }
    
    printf("尝试等待信号量（超时100ms）...\n");
    int ret = vox_semaphore_timedwait(&sem, 100);
    if (ret == 0) {
        printf("等待成功\n");
    } else {
        printf("等待超时（预期行为）\n");
    }
    
    printf("尝试非阻塞等待...\n");
    ret = vox_semaphore_trywait(&sem);
    if (ret == 0) {
        printf("等待成功\n");
    } else {
        printf("信号量为0，无法获取（预期行为）\n");
    }
    
    vox_semaphore_destroy(&sem);
    vox_mpool_destroy(mpool);
}

/* ===== 屏障测试 ===== */

typedef struct {
    int thread_id;
    vox_barrier_t* barrier;
    int* shared_counter;
} barrier_test_data_t;

static int barrier_worker(void* user_data) {
    barrier_test_data_t* data = (barrier_test_data_t*)user_data;
    
    printf("  线程 %d: 开始工作，准备到达屏障...\n", data->thread_id);
    
    /* 模拟一些工作 */
    vox_thread_sleep(100 * data->thread_id);  /* 不同线程工作不同时间 */
    
    printf("  线程 %d: 到达屏障，等待其他线程...\n", data->thread_id);
    
    /* 等待所有线程到达屏障 */
    if (vox_barrier_wait(data->barrier) == 0) {
        printf("  线程 %d: 所有线程已到达，继续执行\n", data->thread_id);
        
        /* 屏障后的工作 */
        (*data->shared_counter)++;
        printf("  线程 %d: 完成屏障后的工作，计数器 = %d\n", 
               data->thread_id, *data->shared_counter);
    } else {
        printf("  线程 %d: 屏障等待失败\n", data->thread_id);
    }
    
    return 0;
}

static void test_barrier(void) {
    printf("\n=== 测试屏障 ===\n");
    
    /* 创建线程安全的内存池（用于多线程环境） */
    vox_mpool_config_t mpool_config = {0};
    mpool_config.thread_safe = 1;  /* 多线程环境需要启用线程安全 */
    vox_mpool_t* mpool = vox_mpool_create_with_config(&mpool_config);
    if (!mpool) {
        fprintf(stderr, "创建内存池失败\n");
        return;
    }
    
    int barrier_counter = 0;  /* 使用不同的名称避免与全局变量冲突 */
    
    /* 使用栈空间分配屏障结构体 */
    #define BARRIER_THREADS 4
    vox_barrier_t barrier;
    if (vox_barrier_create(&barrier, BARRIER_THREADS) != 0) {
        fprintf(stderr, "创建屏障失败\n");
        vox_mpool_destroy(mpool);
        return;
    }
    
    vox_thread_t* threads[BARRIER_THREADS];
    barrier_test_data_t data[BARRIER_THREADS];
    
    printf("创建 %d 个线程，它们将在屏障处同步...\n", BARRIER_THREADS);
    for (int i = 0; i < BARRIER_THREADS; i++) {
        data[i].thread_id = i + 1;
        data[i].barrier = &barrier;
        data[i].shared_counter = &barrier_counter;
        threads[i] = vox_thread_create(mpool, barrier_worker, &data[i]);
    }
    
    for (int i = 0; i < BARRIER_THREADS; i++) {
        if (threads[i]) {
            vox_thread_join(threads[i], NULL);
        }
    }
    
    printf("最终计数器值: %d (期望: %d)\n", barrier_counter, BARRIER_THREADS);
    vox_barrier_destroy(&barrier);
    vox_mpool_destroy(mpool);
}

/* ===== 事件测试 ===== */

typedef struct {
    int thread_id;
    vox_event_t* event;
    bool is_signal_thread;
} event_test_data_t;

static int event_worker(void* user_data) {
    event_test_data_t* data = (event_test_data_t*)user_data;
    
    if (data->is_signal_thread) {
        /* 信号线程：等待一段时间后触发事件 */
        vox_thread_sleep(100);
        printf("  信号线程 %d: 触发事件\n", data->thread_id);
        vox_event_set(data->event);
        return 0;
    } else {
        /* 等待线程：等待事件被触发 */
        printf("  等待线程 %d: 等待事件...\n", data->thread_id);
        if (vox_event_wait(data->event) == 0) {
            printf("  等待线程 %d: 事件已触发，继续执行\n", data->thread_id);
        } else {
            printf("  等待线程 %d: 等待事件失败\n", data->thread_id);
        }
        return 0;
    }
}

static int event_auto_reset_worker(void* user_data) {
    event_test_data_t* data = (event_test_data_t*)user_data;
    
    if (data->is_signal_thread) {
        /* 信号线程：多次触发事件以唤醒所有等待线程 */
        vox_thread_sleep(100);
        for (int i = 0; i < 3; i++) {
            printf("  信号线程 %d: 触发事件 (%d/3)\n", data->thread_id, i + 1);
            vox_event_set(data->event);
            vox_thread_sleep(50);  /* 给等待线程时间处理 */
        }
        return 0;
    } else {
        /* 等待线程：等待事件被触发 */
        printf("  等待线程 %d: 等待事件...\n", data->thread_id);
        if (vox_event_wait(data->event) == 0) {
            printf("  等待线程 %d: 事件已触发，继续执行\n", data->thread_id);
        } else {
            printf("  等待线程 %d: 等待事件失败\n", data->thread_id);
        }
        return 0;
    }
}

static void test_event_manual_reset(void) {
    printf("\n=== 测试事件（手动重置） ===\n");
    
    /* 创建线程安全的内存池（用于多线程环境） */
    vox_mpool_config_t mpool_config = {0};
    mpool_config.thread_safe = 1;  /* 多线程环境需要启用线程安全 */
    vox_mpool_t* mpool = vox_mpool_create_with_config(&mpool_config);
    if (!mpool) {
        fprintf(stderr, "创建内存池失败\n");
        return;
    }
    
    /* 使用栈空间分配事件结构体 */
    vox_event_t event;
    /* 创建手动重置事件，初始状态为未触发 */
    if (vox_event_create(&event, true, false) != 0) {
        fprintf(stderr, "创建事件失败\n");
        vox_mpool_destroy(mpool);
        return;
    }
    
    printf("创建 3 个等待线程和 1 个信号线程...\n");
    vox_thread_t* threads[4];
    event_test_data_t data[4];
    
    for (int i = 0; i < 3; i++) {
        data[i].thread_id = i + 1;
        data[i].event = &event;
        data[i].is_signal_thread = false;
        threads[i] = vox_thread_create(mpool, event_worker, &data[i]);
    }
    
    data[3].thread_id = 4;
    data[3].event = &event;
    data[3].is_signal_thread = true;
    threads[3] = vox_thread_create(mpool, event_worker, &data[3]);
    
    for (int i = 0; i < 4; i++) {
        if (threads[i]) {
            vox_thread_join(threads[i], NULL);
        }
    }
    
    /* 手动重置事件：所有等待的线程都应该被唤醒 */
    printf("手动重置事件：所有等待线程都应该被唤醒\n");
    
    vox_event_reset(&event);
    vox_event_destroy(&event);
    vox_mpool_destroy(mpool);
}

static void test_event_auto_reset(void) {
    printf("\n=== 测试事件（自动重置） ===\n");
    
    /* 创建线程安全的内存池（用于多线程环境） */
    vox_mpool_config_t mpool_config = {0};
    mpool_config.thread_safe = 1;  /* 多线程环境需要启用线程安全 */
    vox_mpool_t* mpool = vox_mpool_create_with_config(&mpool_config);
    if (!mpool) {
        fprintf(stderr, "创建内存池失败\n");
        return;
    }
    
    /* 使用栈空间分配事件结构体 */
    vox_event_t event;
    /* 创建自动重置事件，初始状态为未触发 */
    if (vox_event_create(&event, false, false) != 0) {
        fprintf(stderr, "创建事件失败\n");
        vox_mpool_destroy(mpool);
        return;
    }
    
    printf("创建 3 个等待线程和 1 个信号线程...\n");
    printf("注意：自动重置事件每次只唤醒一个等待线程，需要多次触发\n");
    vox_thread_t* threads[4];
    event_test_data_t data[4];
    
    for (int i = 0; i < 3; i++) {
        data[i].thread_id = i + 1;
        data[i].event = &event;
        data[i].is_signal_thread = false;
        threads[i] = vox_thread_create(mpool, event_auto_reset_worker, &data[i]);
    }
    
    data[3].thread_id = 4;
    data[3].event = &event;
    data[3].is_signal_thread = true;
    threads[3] = vox_thread_create(mpool, event_auto_reset_worker, &data[3]);
    
    for (int i = 0; i < 4; i++) {
        if (threads[i]) {
            vox_thread_join(threads[i], NULL);
        }
    }
    
    /* 自动重置事件：每次触发只唤醒一个等待线程 */
    printf("自动重置事件：每次触发只唤醒一个等待线程，需要多次触发来唤醒所有线程\n");
    
    vox_event_destroy(&event);
    vox_mpool_destroy(mpool);
}

static void test_event_timeout(void) {
    printf("\n=== 测试事件（超时等待） ===\n");
    
    /* 创建内存池（单线程环境，不需要线程安全） */
    vox_mpool_t* mpool = vox_mpool_create();
    if (!mpool) {
        fprintf(stderr, "创建内存池失败\n");
        return;
    }
    
    /* 使用栈空间分配事件结构体 */
    vox_event_t event;
    /* 创建自动重置事件，初始状态为未触发 */
    if (vox_event_create(&event, false, false) != 0) {
        fprintf(stderr, "创建事件失败\n");
        vox_mpool_destroy(mpool);
        return;
    }
    
    printf("测试超时等待（500ms）...\n");
    int result = vox_event_timedwait(&event, 500);
    if (result == 0) {
        printf("  事件在超时前被触发\n");
    } else {
        printf("  等待超时（符合预期）\n");
    }
    
    printf("测试非阻塞等待...\n");
    result = vox_event_trywait(&event);
    if (result == 0) {
        printf("  事件已触发\n");
    } else {
        printf("  事件未触发（符合预期）\n");
    }
    
    printf("触发事件后再次等待...\n");
    vox_event_set(&event);
    result = vox_event_timedwait(&event, 100);
    if (result == 0) {
        printf("  事件已触发，等待成功\n");
    } else {
        printf("  等待失败\n");
    }
    
    vox_event_destroy(&event);
    vox_mpool_destroy(mpool);
}

static void test_event_pulse(void) {
    printf("\n=== 测试事件（脉冲） ===\n");
    
    /* 创建线程安全的内存池（用于多线程环境） */
    vox_mpool_config_t mpool_config = {0};
    mpool_config.thread_safe = 1;  /* 多线程环境需要启用线程安全 */
    vox_mpool_t* mpool = vox_mpool_create_with_config(&mpool_config);
    if (!mpool) {
        fprintf(stderr, "创建内存池失败\n");
        return;
    }
    
    /* 使用栈空间分配事件结构体 */
    vox_event_t event;
    /* 创建手动重置事件，初始状态为未触发 */
    if (vox_event_create(&event, true, false) != 0) {
        fprintf(stderr, "创建事件失败\n");
        vox_mpool_destroy(mpool);
        return;
    }
    
    printf("创建 2 个等待线程...\n");
    vox_thread_t* threads[2];
    event_test_data_t data[2];
    
    for (int i = 0; i < 2; i++) {
        data[i].thread_id = i + 1;
        data[i].event = &event;
        data[i].is_signal_thread = false;
        threads[i] = vox_thread_create(mpool, event_worker, &data[i]);
    }
    
    /* 等待线程启动 */
    vox_thread_sleep(50);
    
    printf("发送脉冲事件（触发并立即重置）...\n");
    vox_event_pulse(&event);
    
    for (int i = 0; i < 2; i++) {
        if (threads[i]) {
            vox_thread_join(threads[i], NULL);
        }
    }
    
    printf("脉冲事件：唤醒等待线程后立即重置\n");
    
    vox_event_destroy(&event);
    vox_mpool_destroy(mpool);
}

int main(void) {
    printf("=== vox_mutex 同步原语示例程序 ===\n");
    
    /* 运行各种测试 */
    test_mutex();
    test_rwlock();
    test_rmutex();
    test_spinlock();
    test_semaphore();
    test_semaphore_timeout();
    test_barrier();
    test_event_manual_reset();
    test_event_auto_reset();
    test_event_timeout();
    test_event_pulse();
    
    printf("\n=== 所有测试完成 ===\n");
    return 0;
}
