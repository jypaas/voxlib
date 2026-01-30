/*
 * mpool_benchmark.c - 内存池性能基准测试
 * 对比 vox_mpool 和标准 malloc/free 的性能
 */

#include "../vox_mpool.h"
#include "../vox_time.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ITERATIONS 1000000
#define MAX_BLOCKS 1000

/* 测试标准 malloc/free */
void benchmark_malloc_free(size_t block_size, int iterations) {
    void* ptrs[MAX_BLOCKS];
    memset(ptrs, 0, sizeof(ptrs));
    
    vox_time_t start = vox_time_monotonic();
    
    for (int i = 0; i < iterations; i++) {
        int idx = i % MAX_BLOCKS;
        if (ptrs[idx]) {
            free(ptrs[idx]);
            ptrs[idx] = NULL;
        }
        ptrs[idx] = malloc(block_size);
        if (ptrs[idx]) {
            memset(ptrs[idx], 0xAA, block_size);
        }
    }
    
    /* 清理 */
    for (int i = 0; i < MAX_BLOCKS; i++) {
        if (ptrs[i]) {
            free(ptrs[i]);
            ptrs[i] = NULL;
        }
    }
    
    vox_time_t end = vox_time_monotonic();
    int64_t elapsed_us = vox_time_diff_us(end, start);
    double ops_per_sec = (elapsed_us > 0) ? (double)iterations * 1000000.0 / elapsed_us : 0.0;
    printf("  malloc/free: %lld 微秒 (%.2f 次/秒)\n", 
           (long long)elapsed_us, ops_per_sec);
}

/* 测试内存池 */
void benchmark_mpool(vox_mpool_t* pool, size_t block_size, int iterations) {
    void* ptrs[MAX_BLOCKS];
    memset(ptrs, 0, sizeof(ptrs));
    
    vox_time_t start = vox_time_monotonic();
    
    for (int i = 0; i < iterations; i++) {
        int idx = i % MAX_BLOCKS;
        if (ptrs[idx]) {
            vox_mpool_free(pool, ptrs[idx]);
            ptrs[idx] = NULL;
        }
        ptrs[idx] = vox_mpool_alloc(pool, block_size);
        if (ptrs[idx]) {
            memset(ptrs[idx], 0xAA, block_size);
        }
    }
    
    /* 清理 */
    for (int i = 0; i < MAX_BLOCKS; i++) {
        if (ptrs[i]) {
            vox_mpool_free(pool, ptrs[i]);
            ptrs[i] = NULL;
        }
    }
    
    vox_time_t end = vox_time_monotonic();
    int64_t elapsed_us = vox_time_diff_us(end, start);
    double ops_per_sec = (elapsed_us > 0) ? (double)iterations * 1000000.0 / elapsed_us : 0.0;
    printf("  mpool:       %lld 微秒 (%.2f 次/秒)\n", 
           (long long)elapsed_us, ops_per_sec);
}

/* 测试分配性能 */
void test_alloc_performance(void) {
    printf("\n=== 分配性能测试 ===\n");
    printf("测试 %d 次分配/释放操作\n\n", ITERATIONS);
    
    size_t sizes[] = {16, 32, 64, 128, 256, 512, 1024, 2048, 4096, 8192};
    int num_sizes = sizeof(sizes) / sizeof(sizes[0]);
    
    vox_mpool_t* pool = vox_mpool_create();
    if (!pool) {
        fprintf(stderr, "Failed to create memory pool\n");
        return;
    }
    
    for (int i = 0; i < num_sizes; i++) {
        printf("块大小 %zu 字节:\n", sizes[i]);
        benchmark_mpool(pool, sizes[i], ITERATIONS);
        benchmark_malloc_free(sizes[i], ITERATIONS);
        
        /* 计算性能提升 */
        printf("\n");
    }
    
    vox_mpool_destroy(pool);
}

/* 测试批量分配性能 */
void test_batch_alloc_performance(void) {
    printf("\n=== 批量分配性能测试 ===\n");
    printf("测试批量分配 %d 个块，然后全部释放\n\n", MAX_BLOCKS);
    
    size_t sizes[] = {64, 256, 1024};
    int num_sizes = sizeof(sizes) / sizeof(sizes[0]);
    
    vox_mpool_t* pool = vox_mpool_create();
    if (!pool) {
        fprintf(stderr, "Failed to create memory pool\n");
        return;
    }
    
    for (int s = 0; s < num_sizes; s++) {
        size_t block_size = sizes[s];
        printf("块大小 %zu 字节:\n", block_size);
        
        void* ptrs[MAX_BLOCKS];
        memset(ptrs, 0, sizeof(ptrs));
        
        /* 测试内存池 */
        vox_time_t start = vox_time_monotonic();
        
        for (int i = 0; i < MAX_BLOCKS; i++) {
            ptrs[i] = vox_mpool_alloc(pool, block_size);
            if (ptrs[i]) {
                memset(ptrs[i], 0xAA, block_size);
            }
        }
        
        for (int i = 0; i < MAX_BLOCKS; i++) {
            if (ptrs[i]) {
                vox_mpool_free(pool, ptrs[i]);
                ptrs[i] = NULL;
            }
        }
        
        vox_time_t end = vox_time_monotonic();
        int64_t elapsed_us = vox_time_diff_us(end, start);
        double ops_per_sec = (elapsed_us > 0) ? (double)MAX_BLOCKS * 2 * 1000000.0 / elapsed_us : 0.0;
        printf("  mpool: %lld 微秒 (%.2f 次/秒)\n", 
               (long long)elapsed_us, ops_per_sec);
        
        /* 测试标准 malloc */
        memset(ptrs, 0, sizeof(ptrs));
        start = vox_time_monotonic();
        
        for (int i = 0; i < MAX_BLOCKS; i++) {
            ptrs[i] = malloc(block_size);
            if (ptrs[i]) {
                memset(ptrs[i], 0xAA, block_size);
            }
        }
        
        for (int i = 0; i < MAX_BLOCKS; i++) {
            if (ptrs[i]) {
                free(ptrs[i]);
                ptrs[i] = NULL;
            }
        }
        
        end = vox_time_monotonic();
        elapsed_us = vox_time_diff_us(end, start);
        ops_per_sec = (elapsed_us > 0) ? (double)MAX_BLOCKS * 2 * 1000000.0 / elapsed_us : 0.0;
        printf("  malloc: %lld 微秒 (%.2f 次/秒)\n", 
               (long long)elapsed_us, ops_per_sec);
        
        printf("\n");
    }
    
    vox_mpool_destroy(pool);
}

/* 测试内存碎片 */
void test_fragmentation(void) {
    printf("\n=== 内存碎片测试 ===\n");
    printf("测试内存池的内存利用率\n\n");
    
    vox_mpool_t* pool = vox_mpool_create();
    if (!pool) {
        fprintf(stderr, "Failed to create memory pool\n");
        return;
    }
    
    void* ptrs[100];
    memset(ptrs, 0, sizeof(ptrs));
    
    size_t sizes[] = {8, 16, 32, 64, 128, 256};
    int num_sizes = sizeof(sizes) / sizeof(sizes[0]);
    
    printf("分配不同大小的块:\n");
    for (int i = 0; i < 100; i++) {
        size_t size = sizes[i % num_sizes];
        ptrs[i] = vox_mpool_alloc(pool, size);
        if (ptrs[i]) {
            size_t actual_size = vox_mpool_get_size(pool, ptrs[i]);
            if (i < 10) {
                printf("  请求 %zu 字节，实际分配 %zu 字节 (利用率 %.1f%%)\n",
                       size, actual_size, 100.0 * size / actual_size);
            }
        }
    }
    
    vox_mpool_stats(pool);
    
    printf("\n释放一半的块:\n");
    for (int i = 0; i < 50; i++) {
        if (ptrs[i]) {
            vox_mpool_free(pool, ptrs[i]);
            ptrs[i] = NULL;
        }
    }
    
    vox_mpool_stats(pool);
    
    printf("\n重新分配:\n");
    for (int i = 0; i < 50; i++) {
        size_t size = sizes[i % num_sizes];
        ptrs[i] = vox_mpool_alloc(pool, size);
    }
    
    vox_mpool_stats(pool);
    
    /* 清理 */
    for (int i = 0; i < 100; i++) {
        if (ptrs[i]) {
            vox_mpool_free(pool, ptrs[i]);
            ptrs[i] = NULL;
        }
    }
    
    vox_mpool_destroy(pool);
}

/* 测试连续分配性能 */
void test_sequential_alloc_performance(void) {
    printf("\n=== 连续分配性能测试 ===\n");
    printf("测试连续分配和释放 %d 个块\n\n", MAX_BLOCKS);
    
    size_t sizes[] = {64, 256, 1024};
    int num_sizes = sizeof(sizes) / sizeof(sizes[0]);
    
    vox_mpool_t* pool = vox_mpool_create();
    if (!pool) {
        fprintf(stderr, "Failed to create memory pool\n");
        return;
    }
    
    for (int s = 0; s < num_sizes; s++) {
        size_t block_size = sizes[s];
        printf("块大小 %zu 字节:\n", block_size);
        
        void* ptrs[MAX_BLOCKS];
        memset(ptrs, 0, sizeof(ptrs));
        
        /* 测试内存池：连续分配 */
        vox_time_t start = vox_time_monotonic();
        for (int i = 0; i < MAX_BLOCKS; i++) {
            ptrs[i] = vox_mpool_alloc(pool, block_size);
            if (ptrs[i]) {
                memset(ptrs[i], 0xAA, block_size);
            }
        }
        vox_time_t end = vox_time_monotonic();
        int64_t alloc_us = vox_time_diff_us(end, start);
        
        /* 测试内存池：连续释放 */
        start = vox_time_monotonic();
        for (int i = 0; i < MAX_BLOCKS; i++) {
            if (ptrs[i]) {
                vox_mpool_free(pool, ptrs[i]);
                ptrs[i] = NULL;
            }
        }
        end = vox_time_monotonic();
        int64_t free_us = vox_time_diff_us(end, start);
        
        printf("  mpool - 分配: %lld 微秒, 释放: %lld 微秒, 总计: %lld 微秒\n",
               (long long)alloc_us, (long long)free_us, (long long)(alloc_us + free_us));
        
        /* 测试标准 malloc：连续分配 */
        memset(ptrs, 0, sizeof(ptrs));
        start = vox_time_monotonic();
        for (int i = 0; i < MAX_BLOCKS; i++) {
            ptrs[i] = malloc(block_size);
            if (ptrs[i]) {
                memset(ptrs[i], 0xAA, block_size);
            }
        }
        end = vox_time_monotonic();
        alloc_us = vox_time_diff_us(end, start);
        
        /* 测试标准 malloc：连续释放 */
        start = vox_time_monotonic();
        for (int i = 0; i < MAX_BLOCKS; i++) {
            if (ptrs[i]) {
                free(ptrs[i]);
                ptrs[i] = NULL;
            }
        }
        end = vox_time_monotonic();
        free_us = vox_time_diff_us(end, start);
        
        printf("  malloc - 分配: %lld 微秒, 释放: %lld 微秒, 总计: %lld 微秒\n",
               (long long)alloc_us, (long long)free_us, (long long)(alloc_us + free_us));
        
        printf("\n");
    }
    
    vox_mpool_destroy(pool);
}

int main(void) {
    printf("=== vox_mpool 性能基准测试 ===\n");
    
    test_alloc_performance();
    test_batch_alloc_performance();
    test_sequential_alloc_performance();
    test_fragmentation();
    
    printf("\n=== 测试完成 ===\n");
    return 0;
}
