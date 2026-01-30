/* ============================================================
 * test_mutex.c - vox_mutex 模块测试
 * ============================================================ */

#include "test_runner.h"
#include "../vox_mutex.h"
#include "../vox_thread.h"
#include "../vox_mpool.h"

/* 测试互斥锁基本操作 */
static void test_mutex_basic(vox_mpool_t* mpool) {
    VOX_UNUSED(mpool);
    vox_mutex_t mutex;
    
    TEST_ASSERT_EQ(vox_mutex_create(&mutex), 0, "创建互斥锁失败");
    
    TEST_ASSERT_EQ(vox_mutex_lock(&mutex), 0, "加锁失败");
    TEST_ASSERT_EQ(vox_mutex_unlock(&mutex), 0, "解锁失败");
    
    /* 测试trylock - 应该成功 */
    int result = vox_mutex_trylock(&mutex);
    TEST_ASSERT_EQ(result, 0, "尝试加锁失败");
    TEST_ASSERT_EQ(vox_mutex_unlock(&mutex), 0, "解锁失败");
    
    /* 注意：在Windows上，CRITICAL_SECTION可能允许同一线程重复进入（递归）
     * 在不同平台上行为可能不同，这里只测试基本功能，不测试重复加锁 */
    
    vox_mutex_destroy(&mutex);
}

/* 测试读写锁基本操作 */
static void test_rwlock_basic(vox_mpool_t* mpool) {
    VOX_UNUSED(mpool);
    vox_rwlock_t rwlock;
    
    TEST_ASSERT_EQ(vox_rwlock_create(&rwlock), 0, "创建读写锁失败");
    
    /* 测试读锁 */
    TEST_ASSERT_EQ(vox_rwlock_rdlock(&rwlock), 0, "获取读锁失败");
    TEST_ASSERT_EQ(vox_rwlock_unlock(&rwlock), 0, "解锁读锁失败");
    
    /* 测试多个读锁（如果支持） */
    TEST_ASSERT_EQ(vox_rwlock_rdlock(&rwlock), 0, "获取读锁失败");
    int try_result = vox_rwlock_tryrdlock(&rwlock);
    /* tryrdlock可能成功或失败，取决于实现 */
    if (try_result == 0) {
        /* 成功获取第二个读锁，需要解锁两次 */
        TEST_ASSERT_EQ(vox_rwlock_unlock(&rwlock), 0, "解锁第二个读锁失败");
        TEST_ASSERT_EQ(vox_rwlock_unlock(&rwlock), 0, "解锁第一个读锁失败");
    } else {
        /* 失败，只获取了一个读锁，只需要解锁一次 */
        TEST_ASSERT_EQ(vox_rwlock_unlock(&rwlock), 0, "解锁读锁失败");
    }
    
    /* 测试写锁 */
    TEST_ASSERT_EQ(vox_rwlock_wrlock(&rwlock), 0, "获取写锁失败");
    TEST_ASSERT_EQ(vox_rwlock_unlock(&rwlock), 0, "解锁写锁失败");
    
    vox_rwlock_destroy(&rwlock);
}

/* 测试递归锁基本操作 */
static void test_rmutex_basic(vox_mpool_t* mpool) {
    VOX_UNUSED(mpool);
    vox_rmutex_t rmutex;
    
    TEST_ASSERT_EQ(vox_rmutex_create(&rmutex), 0, "创建递归锁失败");
    
    /* 测试递归加锁 */
    TEST_ASSERT_EQ(vox_rmutex_lock(&rmutex), 0, "第一次加锁失败");
    TEST_ASSERT_EQ(vox_rmutex_lock(&rmutex), 0, "第二次加锁失败（递归锁应支持）");
    TEST_ASSERT_EQ(vox_rmutex_unlock(&rmutex), 0, "第一次解锁失败");
    TEST_ASSERT_EQ(vox_rmutex_unlock(&rmutex), 0, "第二次解锁失败");
    
    vox_rmutex_destroy(&rmutex);
}

/* 测试自旋锁基本操作 */
static void test_spinlock_basic(vox_mpool_t* mpool) {
    VOX_UNUSED(mpool);
    vox_spinlock_t spinlock;
    
    TEST_ASSERT_EQ(vox_spinlock_create(&spinlock), 0, "创建自旋锁失败");
    
    vox_spinlock_lock(&spinlock);  /* void函数 */
    vox_spinlock_unlock(&spinlock);  /* void函数 */
    
    bool try_result = vox_spinlock_trylock(&spinlock);
    TEST_ASSERT_EQ(try_result, 1, "尝试加锁失败");
    vox_spinlock_unlock(&spinlock);  /* void函数 */
    
    vox_spinlock_destroy(&spinlock);
}

/* 测试信号量基本操作 */
static void test_semaphore_basic(vox_mpool_t* mpool) {
    VOX_UNUSED(mpool);
    vox_semaphore_t sem;
    
    TEST_ASSERT_EQ(vox_semaphore_create(&sem, 2), 0, "创建信号量失败");
    
    /* 测试获取和释放 */
    TEST_ASSERT_EQ(vox_semaphore_wait(&sem), 0, "等待信号量失败");
    TEST_ASSERT_EQ(vox_semaphore_wait(&sem), 0, "等待信号量失败");
    TEST_ASSERT_EQ(vox_semaphore_post(&sem), 0, "释放信号量失败");
    TEST_ASSERT_EQ(vox_semaphore_post(&sem), 0, "释放信号量失败");
    
    vox_semaphore_destroy(&sem);
}

/* 多线程竞争测试数据结构 */
typedef struct {
    vox_mutex_t mutex;
    int counter;
    int thread_count;
} mutex_test_data_t;

/* 互斥锁竞争测试线程函数 */
static int mutex_contention_func(void* user_data) {
    mutex_test_data_t* data = (mutex_test_data_t*)user_data;
    
    for (int i = 0; i < 1000; i++) {
        vox_mutex_lock(&data->mutex);
        data->counter++;
        vox_mutex_unlock(&data->mutex);
    }
    
    return 0;
}

/* 测试互斥锁多线程竞争 */
static void test_mutex_contention(vox_mpool_t* mpool) {
    mutex_test_data_t data = {
        .counter = 0,
        .thread_count = 5
    };
    TEST_ASSERT_EQ(vox_mutex_create(&data.mutex), 0, "创建互斥锁失败");
    
    /* 创建5个线程同时竞争 */
    vox_thread_t* threads[5];
    for (int i = 0; i < 5; i++) {
        threads[i] = vox_thread_create(mpool, mutex_contention_func, &data);
        TEST_ASSERT_NOT_NULL(threads[i], "创建线程失败");
    }
    
    /* 等待所有线程完成 */
    for (int i = 0; i < 5; i++) {
        vox_thread_join(threads[i], NULL);
    }
    
    /* 验证计数器值（每个线程增加1000次，共5个线程） */
    TEST_ASSERT_EQ(data.counter, 5000, "多线程竞争后计数器值不正确");
    
    vox_mutex_destroy(&data.mutex);
}

/* 读写锁竞争测试数据结构 */
typedef struct {
    vox_rwlock_t rwlock;
    vox_mutex_t mutex;
    int read_count;
    int write_count;
    int thread_count;
} rwlock_test_data_t;

/* 读锁线程函数 */
static int rwlock_read_func(void* user_data) {
    rwlock_test_data_t* data = (rwlock_test_data_t*)user_data;
    
    for (int i = 0; i < 100; i++) {
        vox_rwlock_rdlock(&data->rwlock);
        /* 使用互斥锁保护计数器 */
        vox_mutex_lock(&data->mutex);
        data->read_count++;
        vox_mutex_unlock(&data->mutex);
        vox_thread_sleep(1);  /* 模拟读取操作 */
        vox_rwlock_unlock(&data->rwlock);
    }
    
    return 0;
}

/* 写锁线程函数 */
static int rwlock_write_func(void* user_data) {
    rwlock_test_data_t* data = (rwlock_test_data_t*)user_data;
    
    for (int i = 0; i < 20; i++) {
        vox_rwlock_wrlock(&data->rwlock);
        /* 使用互斥锁保护计数器 */
        vox_mutex_lock(&data->mutex);
        data->write_count++;
        vox_mutex_unlock(&data->mutex);
        vox_thread_sleep(5);  /* 模拟写入操作 */
        vox_rwlock_unlock(&data->rwlock);
    }
    
    return 0;
}

/* 测试读写锁多线程竞争 */
static void test_rwlock_contention(vox_mpool_t* mpool) {
    rwlock_test_data_t data = {
        .read_count = 0,
        .write_count = 0,
        .thread_count = 0
    };
    TEST_ASSERT_EQ(vox_rwlock_create(&data.rwlock), 0, "创建读写锁失败");
    TEST_ASSERT_EQ(vox_mutex_create(&data.mutex), 0, "创建互斥锁失败");
    
    /* 创建3个读线程和2个写线程 */
    vox_thread_t* threads[5];
    threads[0] = vox_thread_create(mpool, rwlock_read_func, &data);
    threads[1] = vox_thread_create(mpool, rwlock_read_func, &data);
    threads[2] = vox_thread_create(mpool, rwlock_read_func, &data);
    threads[3] = vox_thread_create(mpool, rwlock_write_func, &data);
    threads[4] = vox_thread_create(mpool, rwlock_write_func, &data);
    
    for (int i = 0; i < 5; i++) {
        TEST_ASSERT_NOT_NULL(threads[i], "创建线程失败");
    }
    
    /* 等待所有线程完成 */
    for (int i = 0; i < 5; i++) {
        vox_thread_join(threads[i], NULL);
    }
    
    /* 验证读写计数（读线程每个100次，写线程每个20次） */
    TEST_ASSERT_EQ(data.read_count, 300, "读锁竞争后读计数不正确");
    TEST_ASSERT_EQ(data.write_count, 40, "写锁竞争后写计数不正确");
    
    vox_mutex_destroy(&data.mutex);
    vox_rwlock_destroy(&data.rwlock);
}

/* 信号量竞争测试数据结构 */
typedef struct {
    vox_semaphore_t sem;
    vox_mutex_t mutex;
    int counter;
    int max_count;
} semaphore_test_data_t;

/* 信号量竞争测试线程函数 */
static int semaphore_contention_func(void* user_data) {
    semaphore_test_data_t* data = (semaphore_test_data_t*)user_data;
    
    for (int i = 0; i < 50; i++) {
        vox_semaphore_wait(&data->sem);
        /* 使用互斥锁保护计数器 */
        vox_mutex_lock(&data->mutex);
        data->counter++;
        vox_mutex_unlock(&data->mutex);
        
        vox_thread_sleep(1);
        
        vox_mutex_lock(&data->mutex);
        data->counter--;
        vox_mutex_unlock(&data->mutex);
        
        vox_semaphore_post(&data->sem);
    }
    
    return 0;
}

/* 测试信号量多线程竞争 */
static void test_semaphore_contention(vox_mpool_t* mpool) {
    semaphore_test_data_t data = {
        .counter = 0,
        .max_count = 3
    };
    TEST_ASSERT_EQ(vox_semaphore_create(&data.sem, 3), 0, "创建信号量失败（允许3个并发）");
    TEST_ASSERT_EQ(vox_mutex_create(&data.mutex), 0, "创建互斥锁失败");
    
    /* 创建5个线程，但信号量只允许3个并发 */
    vox_thread_t* threads[5];
    for (int i = 0; i < 5; i++) {
        threads[i] = vox_thread_create(mpool, semaphore_contention_func, &data);
        TEST_ASSERT_NOT_NULL(threads[i], "创建线程失败");
    }
    
    /* 等待所有线程完成 */
    for (int i = 0; i < 5; i++) {
        vox_thread_join(threads[i], NULL);
    }
    
    /* 验证最终计数器应为0（所有操作都已完成） */
    TEST_ASSERT_EQ(data.counter, 0, "信号量竞争后计数器应为0");
    
    vox_mutex_destroy(&data.mutex);
    vox_semaphore_destroy(&data.sem);
}

/* 测试套件 */
test_case_t test_mutex_cases[] = {
    {"mutex_basic", test_mutex_basic},
    {"rwlock_basic", test_rwlock_basic},
    {"rmutex_basic", test_rmutex_basic},
    {"spinlock_basic", test_spinlock_basic},
    {"semaphore_basic", test_semaphore_basic},
    {"mutex_contention", test_mutex_contention},
    {"rwlock_contention", test_rwlock_contention},
    {"semaphore_contention", test_semaphore_contention},
};

test_suite_t test_mutex_suite = {
    "vox_mutex",
    test_mutex_cases,
    sizeof(test_mutex_cases) / sizeof(test_mutex_cases[0])
};
