/*
 * atomic_example.c - 原子操作示例程序
 * 演示 vox_atomic 的各种原子操作
 */

#include "../vox_atomic.h"
#include "../vox_thread.h"
#include "../vox_mpool.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NUM_THREADS 5
#define ITERATIONS 10000

/* ===== 原子整数测试 ===== */

typedef struct {
    int thread_id;
    vox_atomic_int_t* counter;
    int iterations;
} atomic_int_test_data_t;

static int atomic_int_worker(void* user_data) {
    atomic_int_test_data_t* data = (atomic_int_test_data_t*)user_data;
    
    for (int i = 0; i < data->iterations; i++) {
        vox_atomic_int_increment(data->counter);
    }
    
    return 0;
}

static void test_atomic_int(void) {
    printf("\n=== 测试原子整数 ===\n");
    
    /* 创建内存池 */
    vox_mpool_t* mpool = vox_mpool_create();
    if (!mpool) {
        fprintf(stderr, "创建内存池失败\n");
        return;
    }
    
    vox_atomic_int_t* counter = vox_atomic_int_create(mpool, 0);
    if (!counter) {
        fprintf(stderr, "创建原子整数失败\n");
        vox_mpool_destroy(mpool);
        return;
    }
    
    printf("初始值: %d\n", vox_atomic_int_load(counter));
    
    vox_thread_t* threads[NUM_THREADS];
    atomic_int_test_data_t data[NUM_THREADS];
    
    printf("创建 %d 个线程，每个线程递增 %d 次...\n", NUM_THREADS, ITERATIONS);
    for (int i = 0; i < NUM_THREADS; i++) {
        data[i].thread_id = i + 1;
        data[i].counter = counter;
        data[i].iterations = ITERATIONS;
        threads[i] = vox_thread_create(mpool, atomic_int_worker, &data[i]);
    }
    
    for (int i = 0; i < NUM_THREADS; i++) {
        if (threads[i]) {
            vox_thread_join(threads[i], NULL);
        }
    }
    
    printf("最终值: %d (期望: %d)\n", 
           vox_atomic_int_load(counter), 
           NUM_THREADS * ITERATIONS);
    
    /* 测试其他操作 */
    printf("\n测试其他原子操作:\n");
    printf("  当前值: %d\n", vox_atomic_int_load(counter));
    
    int32_t old = vox_atomic_int_add(counter, 100);
    printf("  add(100) 前值: %d, 后值: %d\n", old, vox_atomic_int_load(counter));
    
    old = vox_atomic_int_sub(counter, 50);
    printf("  sub(50) 前值: %d, 后值: %d\n", old, vox_atomic_int_load(counter));
    
    old = vox_atomic_int_exchange(counter, 999);
    printf("  exchange(999) 旧值: %d, 新值: %d\n", old, vox_atomic_int_load(counter));
    
    int32_t expected = 999;
    bool success = vox_atomic_int_compare_exchange(counter, &expected, 1000);
    printf("  compare_exchange(999->1000): %s, 当前值: %d\n", 
           success ? "成功" : "失败", vox_atomic_int_load(counter));
    
    vox_atomic_int_destroy(counter);
    vox_mpool_destroy(mpool);
}

/* ===== 原子长整数测试 ===== */

static void test_atomic_long(void) {
    printf("\n=== 测试原子长整数 ===\n");
    
    /* 创建内存池 */
    vox_mpool_t* mpool = vox_mpool_create();
    if (!mpool) {
        fprintf(stderr, "创建内存池失败\n");
        return;
    }
    
    vox_atomic_long_t* counter = vox_atomic_long_create(mpool, 0);
    if (!counter) {
        fprintf(stderr, "创建原子长整数失败\n");
        vox_mpool_destroy(mpool);
        return;
    }
    
    printf("初始值: %lld\n", (long long)vox_atomic_long_load(counter));
    
    /* 测试基本操作 */
    vox_atomic_long_store(counter, 1000);
    printf("store(1000) 后值: %lld\n", (long long)vox_atomic_long_load(counter));
    
    int64_t old = vox_atomic_long_add(counter, 500);
    printf("add(500) 前值: %lld, 后值: %lld\n", 
           (long long)old, (long long)vox_atomic_long_load(counter));
    
    old = vox_atomic_long_increment(counter);
    printf("increment() 前值: %lld, 后值: %lld\n", 
           (long long)old, (long long)vox_atomic_long_load(counter));
    
    old = vox_atomic_long_decrement(counter);
    printf("decrement() 前值: %lld, 后值: %lld\n", 
           (long long)old, (long long)vox_atomic_long_load(counter));
    
    int64_t expected = 1500;
    bool success = vox_atomic_long_compare_exchange(counter, &expected, 2000);
    printf("compare_exchange(1500->2000): %s, 当前值: %lld\n", 
           success ? "成功" : "失败", (long long)vox_atomic_long_load(counter));
    
    vox_atomic_long_destroy(counter);
    vox_mpool_destroy(mpool);
}

/* ===== 原子指针测试 ===== */

static void test_atomic_ptr(void) {
    printf("\n=== 测试原子指针 ===\n");
    
    /* 创建内存池 */
    vox_mpool_t* mpool = vox_mpool_create();
    if (!mpool) {
        fprintf(stderr, "创建内存池失败\n");
        return;
    }
    
    vox_atomic_ptr_t* atomic_ptr = vox_atomic_ptr_create(mpool, NULL);
    if (!atomic_ptr) {
        fprintf(stderr, "创建原子指针失败\n");
        vox_mpool_destroy(mpool);
        return;
    }
    
    printf("初始值: %p (NULL)\n", vox_atomic_ptr_load(atomic_ptr));
    
    /* 测试基本操作 */
    int value1 = 100;
    int value2 = 200;
    int value3 = 300;
    
    vox_atomic_ptr_store(atomic_ptr, &value1);
    printf("store(&value1) 后值: %p (value1=%d)\n", 
           vox_atomic_ptr_load(atomic_ptr), 
           *(int*)vox_atomic_ptr_load(atomic_ptr));
    
    void* old = vox_atomic_ptr_exchange(atomic_ptr, &value2);
    printf("exchange(&value2) 旧值: %p, 新值: %p (value2=%d)\n", 
           old, vox_atomic_ptr_load(atomic_ptr),
           *(int*)vox_atomic_ptr_load(atomic_ptr));
    
    void* expected = &value2;
    bool success = vox_atomic_ptr_compare_exchange(atomic_ptr, &expected, &value3);
    printf("compare_exchange(&value2->&value3): %s, 当前值: %p (value3=%d)\n", 
           success ? "成功" : "失败", 
           vox_atomic_ptr_load(atomic_ptr),
           *(int*)vox_atomic_ptr_load(atomic_ptr));
    
    vox_atomic_ptr_destroy(atomic_ptr);
    vox_mpool_destroy(mpool);
}

/* ===== 原子整数位操作测试 ===== */

static void test_atomic_int_bitops(void) {
    printf("\n=== 测试原子整数位操作 ===\n");
    
    /* 创建内存池 */
    vox_mpool_t* mpool = vox_mpool_create();
    if (!mpool) {
        fprintf(stderr, "创建内存池失败\n");
        return;
    }
    
    vox_atomic_int_t* atomic = vox_atomic_int_create(mpool, 0xFF00);
    if (!atomic) {
        fprintf(stderr, "创建原子整数失败\n");
        vox_mpool_destroy(mpool);
        return;
    }
    
    printf("初始值: 0x%08X\n", vox_atomic_int_load(atomic));
    
    int32_t old = vox_atomic_int_and(atomic, 0x0F0F);
    printf("and(0x0F0F) 前值: 0x%08X, 后值: 0x%08X\n", 
           old, vox_atomic_int_load(atomic));
    
    old = vox_atomic_int_or(atomic, 0xF0F0);
    printf("or(0xF0F0) 前值: 0x%08X, 后值: 0x%08X\n", 
           old, vox_atomic_int_load(atomic));
    
    old = vox_atomic_int_xor(atomic, 0xFFFF);
    printf("xor(0xFFFF) 前值: 0x%08X, 后值: 0x%08X\n", 
           old, vox_atomic_int_load(atomic));
    
    vox_atomic_int_destroy(atomic);
    vox_mpool_destroy(mpool);
}

int main(void) {
    printf("=== vox_atomic 原子操作示例程序 ===\n");
    
    /* 运行各种测试 */
    test_atomic_int();
    test_atomic_long();
    test_atomic_ptr();
    test_atomic_int_bitops();
    
    printf("\n=== 所有测试完成 ===\n");
    return 0;
}
