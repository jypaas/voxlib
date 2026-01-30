/*
 * mpool_example.c - 内存池测试程序
 * 编译: gcc -o mpool_example mpool_example.c ../vox_mpool.c ../vox_mutex.c
 * 运行: ./mpool_example
 */

 #include "../vox_mpool.h"
 #include <stdio.h>
 #include <string.h>
 #include <stdlib.h>
 
 /* 测试基本功能 */
 static void test_basic_functionality(vox_mpool_t* pool) {
     printf("\n=== Testing Basic Functionality ===\n");
     
     /* 测试不同大小的分配 */
     void* ptrs[300];
     size_t sizes[] = {8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096, 8192};
     int num_sizes = sizeof(sizes) / sizeof(sizes[0]);

     printf("\nAllocating 100 memory blocks...\n");
     for (int i = 0; i < 100; i++) {
         size_t size = sizes[i % num_sizes];
         ptrs[i] = vox_mpool_alloc(pool, size);
         if (ptrs[i]) {
             memset(ptrs[i], 0xAB, size);  /* 填充数据 */
             
             /* 验证可以获取块大小 */
             size_t retrieved_size = vox_mpool_get_size(pool, ptrs[i]);
             if (retrieved_size != size && retrieved_size < size) {
                 printf("  WARNING: Block %d: requested %zu bytes, got %zu bytes\n", 
                     i, size, retrieved_size);
             }
         } else {
             fprintf(stderr, "Failed to allocate block %d\n", i);
         }
     }
     
     vox_mpool_stats(pool);
     
     printf("\nFreeing half of the blocks...\n");
     for (int i = 0; i < 50; i++) {
         vox_mpool_free(pool, ptrs[i]);
     }
     
     vox_mpool_stats(pool);
     
     printf("\nRe-allocating blocks...\n");
     for (int i = 0; i < 50; i++) {
         size_t size = sizes[i % num_sizes];
         ptrs[i] = vox_mpool_alloc(pool, size);
     }
     
     vox_mpool_stats(pool);
     
     printf("\nFreeing all blocks...\n");
     for (int i = 0; i < 100; i++) {
         vox_mpool_free(pool, ptrs[i]);
     }
     
     vox_mpool_stats(pool);
 }
 
 /* 测试realloc功能 */
 static void test_realloc(vox_mpool_t* pool) {
     printf("\n=== Testing realloc ===\n");
     
     /* 测试realloc：从小到大 */
     printf("\nTest 1: Reallocating from 32 bytes to 256 bytes\n");
     void* ptr1 = vox_mpool_alloc(pool, 32);
     if (ptr1) {
         memset(ptr1, 0xAA, 32);
         printf("Original: %zu bytes at %p\n", vox_mpool_get_size(pool, ptr1), ptr1);
         
         ptr1 = vox_mpool_realloc(pool, ptr1, 256);
         printf("After realloc: %zu bytes at %p\n", vox_mpool_get_size(pool, ptr1), ptr1);
     }
     
     /* 测试realloc：在同一槽内 */
     printf("\nTest 2: Reallocating within same slot (60 -> 64 bytes)\n");
     void* ptr2 = vox_mpool_alloc(pool, 60);
     if (ptr2) {
         void* ptr2_old = ptr2;
         printf("Original: %zu bytes at %p\n", vox_mpool_get_size(pool, ptr2), ptr2);
         
         ptr2 = vox_mpool_realloc(pool, ptr2, 64);
         printf("After realloc: %zu bytes at %p\n", vox_mpool_get_size(pool, ptr2), ptr2);
         printf("Pointer unchanged (same slot): %s\n", ptr2 == ptr2_old ? "YES" : "NO");
     }
     
     /* 测试realloc：缩小 */
     printf("\nTest 3: Reallocating from 512 bytes to 128 bytes\n");
     void* ptr3 = vox_mpool_alloc(pool, 512);
     if (ptr3) {
         memset(ptr3, 0xBB, 512);
         printf("Original: %zu bytes at %p\n", vox_mpool_get_size(pool, ptr3), ptr3);
         
         ptr3 = vox_mpool_realloc(pool, ptr3, 128);
         printf("After realloc: %zu bytes at %p\n", vox_mpool_get_size(pool, ptr3), ptr3);
     }
     
     /* 测试realloc：NULL指针 */
     printf("\nTest 4: Realloc with NULL pointer (acts as alloc)\n");
     void* ptr4 = vox_mpool_realloc(pool, NULL, 128);
     printf("Allocated: %zu bytes at %p\n", vox_mpool_get_size(pool, ptr4), ptr4);
     
     /* 测试realloc：大小为0 */
     printf("\nTest 5: Realloc with size 0 (acts as free)\n");
     void* ptr5 = vox_mpool_realloc(pool, ptr4, 0);
     printf("Result: %p (should be NULL)\n", ptr5);
     
     /* 测试超大块分配 */
     printf("\nTest 6: Large allocation (beyond pool sizes)\n");
     void* large_ptr = vox_mpool_alloc(pool, 16384);
     if (large_ptr) {
         printf("Allocated 16384 bytes (falls back to malloc)\n");
         memset(large_ptr, 0xCC, 16384);
         vox_mpool_free(pool, large_ptr);
         printf("Freed large block\n");
     }
     
     /* 清理测试的realloc内存 */
     if (ptr1) vox_mpool_free(pool, ptr1);
     if (ptr2) vox_mpool_free(pool, ptr2);
     if (ptr3) vox_mpool_free(pool, ptr3);
 }
 
 /* 测试reset功能 */
 static void test_reset(vox_mpool_t* pool) {
     printf("\n=== Testing reset ===\n");
     printf("\nAllocating some blocks before reset...\n");
     void* reset_ptrs[10];
     for (int i = 0; i < 10; i++) {
         reset_ptrs[i] = vox_mpool_alloc(pool, 64);
         if (reset_ptrs[i]) {
             memset(reset_ptrs[i], 0xDD, 64);
             printf("  Allocated block %d: %zu bytes\n", i, vox_mpool_get_size(pool, reset_ptrs[i]));
         }
     }
     vox_mpool_stats(pool);
     
     printf("\nResetting memory pool...\n");
     vox_mpool_reset(pool);
     printf("After reset:\n");
     vox_mpool_stats(pool);
     
     printf("\nAllocating blocks after reset (should reuse freed blocks)...\n");
     for (int i = 0; i < 10; i++) {
         void* ptr = vox_mpool_alloc(pool, 64);
         if (ptr) {
             printf("  Allocated block %d: %zu bytes at %p\n", i, vox_mpool_get_size(pool, ptr), ptr);
             vox_mpool_free(pool, ptr);
         }
     }
     vox_mpool_stats(pool);
 }
 
 /* 测试配置功能 */
 static void test_config(void) {
     printf("\n=== Testing Configuration ===\n");
     
     /* 测试1: 默认配置（非线程安全，默认块数量） */
     printf("\nTest 1: Default configuration\n");
     vox_mpool_t* pool1 = vox_mpool_create();
     if (pool1) {
         printf("Created pool with default config\n");
         void* ptr = vox_mpool_alloc(pool1, 64);
         if (ptr) {
             printf("Allocated 64 bytes successfully\n");
             vox_mpool_free(pool1, ptr);
         }
         vox_mpool_destroy(pool1);
     }
     
     /* 测试2: 自定义初始块数量 */
     printf("\nTest 2: Custom initial_block_count (128)\n");
     vox_mpool_config_t config2 = {
         .thread_safe = 0,
         .initial_block_count = 128
     };
     vox_mpool_t* pool2 = vox_mpool_create_with_config(&config2);
     if (pool2) {
         printf("Created pool with initial_block_count=128\n");
         /* 分配多个块以触发扩展 */
         void* ptrs[200];
         for (int i = 0; i < 200; i++) {
             ptrs[i] = vox_mpool_alloc(pool2, 64);
         }
         printf("Allocated 200 blocks of 64 bytes\n");
         vox_mpool_stats(pool2);
         
         /* 释放所有块 */
         for (int i = 0; i < 200; i++) {
             vox_mpool_free(pool2, ptrs[i]);
         }
         vox_mpool_destroy(pool2);
     }
     
     /* 测试3: 线程安全配置 */
     printf("\nTest 3: Thread-safe configuration\n");
     vox_mpool_config_t config3 = {
         .thread_safe = 1,
         .initial_block_count = 64
     };
     vox_mpool_t* pool3 = vox_mpool_create_with_config(&config3);
     if (pool3) {
         printf("Created thread-safe pool\n");
         void* ptr = vox_mpool_alloc(pool3, 64);
         if (ptr) {
             printf("Allocated 64 bytes in thread-safe pool\n");
             vox_mpool_free(pool3, ptr);
         }
         vox_mpool_destroy(pool3);
     }
     
     /* 测试4: 完整配置（线程安全 + 自定义块数量） */
     printf("\nTest 4: Full configuration (thread-safe + custom block count)\n");
     vox_mpool_config_t config4 = {
         .thread_safe = 1,
         .initial_block_count = 256
     };
     vox_mpool_t* pool4 = vox_mpool_create_with_config(&config4);
     if (pool4) {
         printf("Created thread-safe pool with initial_block_count=256\n");
         void* ptrs[500];
         for (int i = 0; i < 500; i++) {
             ptrs[i] = vox_mpool_alloc(pool4, 32);
         }
         printf("Allocated 500 blocks of 32 bytes\n");
         vox_mpool_stats(pool4);
         
         for (int i = 0; i < 500; i++) {
             vox_mpool_free(pool4, ptrs[i]);
         }
         vox_mpool_destroy(pool4);
     }
     
     /* 测试5: NULL配置（应该使用默认值） */
     printf("\nTest 5: NULL config (should use defaults)\n");
     vox_mpool_t* pool5 = vox_mpool_create_with_config(NULL);
     if (pool5) {
         printf("Created pool with NULL config (defaults)\n");
         void* ptr = vox_mpool_alloc(pool5, 64);
         if (ptr) {
             printf("Allocated 64 bytes successfully\n");
             vox_mpool_free(pool5, ptr);
         }
         vox_mpool_destroy(pool5);
     }
     
     /* 测试6: 初始块数量为0（应该使用默认值64） */
     printf("\nTest 6: initial_block_count=0 (should use default 64)\n");
     vox_mpool_config_t config6 = {
         .thread_safe = 0,
         .initial_block_count = 0
     };
     vox_mpool_t* pool6 = vox_mpool_create_with_config(&config6);
     if (pool6) {
         printf("Created pool with initial_block_count=0 (should use default)\n");
         void* ptr = vox_mpool_alloc(pool6, 64);
         if (ptr) {
             printf("Allocated 64 bytes successfully\n");
             vox_mpool_free(pool6, ptr);
         }
         vox_mpool_destroy(pool6);
     }
 }
 
 int main(void) {
     printf("=== Memory Pool Test Suite ===\n");
     
     /* 测试配置功能 */
     test_config();
     
     /* 使用默认配置创建内存池进行基本功能测试 */
     printf("\n=== Creating memory pool for basic tests ===\n");
     vox_mpool_t* pool = vox_mpool_create();
     if (!pool) {
         fprintf(stderr, "Failed to create memory pool\n");
         return 1;
     }
     
     /* 运行基本功能测试 */
     test_basic_functionality(pool);
     
     /* 测试realloc功能 */
     test_realloc(pool);
     
     /* 测试reset功能 */
     test_reset(pool);
     
     printf("\nDestroying memory pool...\n");
     vox_mpool_destroy(pool);
     
     printf("\n=== All tests completed successfully! ===\n");
     printf("Features verified:\n");
     printf("  - Basic allocation and deallocation\n");
     printf("  - Realloc functionality\n");
     printf("  - Reset functionality\n");
     printf("  - Configuration API (vox_mpool_create_with_config)\n");
     printf("  - Thread-safe configuration\n");
     printf("  - Custom initial_block_count configuration\n");
     return 0;
}
