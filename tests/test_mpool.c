/* ============================================================
 * test_mpool.c - vox_mpool 模块测试
 * ============================================================ */

#include "test_runner.h"
#include "../vox_mpool.h"
#include "../vox_thread.h"
#include "../vox_os.h"
#include <string.h>

/* 测试基本创建和销毁 */
static void test_mpool_create_destroy(vox_mpool_t* mpool) {
    VOX_UNUSED(mpool);
    vox_mpool_t* pool = vox_mpool_create();
    TEST_ASSERT_NOT_NULL(pool, "创建内存池失败");
    vox_mpool_destroy(pool);
}

/* 测试使用配置创建内存池 */
static void test_mpool_create_with_config(vox_mpool_t* mpool) {
    VOX_UNUSED(mpool);
    vox_mpool_config_t config = {0};
    config.thread_safe = 1;
    config.initial_block_count = 32;
    
    vox_mpool_t* pool = vox_mpool_create_with_config(&config);
    TEST_ASSERT_NOT_NULL(pool, "使用配置创建内存池失败");
    vox_mpool_destroy(pool);
    
    /* 测试NULL配置 */
    pool = vox_mpool_create_with_config(NULL);
    TEST_ASSERT_NOT_NULL(pool, "使用NULL配置创建内存池失败");
    vox_mpool_destroy(pool);
}

/* 测试基本分配和释放 */
static void test_mpool_alloc_free(vox_mpool_t* mpool) {
    VOX_UNUSED(mpool);
    vox_mpool_t* pool = vox_mpool_create();
    TEST_ASSERT_NOT_NULL(pool, "创建内存池失败");
    
    /* 测试不同大小的分配 */
    void* ptr1 = vox_mpool_alloc(pool, 16);
    TEST_ASSERT_NOT_NULL(ptr1, "分配16字节失败");
    
    void* ptr2 = vox_mpool_alloc(pool, 32);
    TEST_ASSERT_NOT_NULL(ptr2, "分配32字节失败");
    
    void* ptr3 = vox_mpool_alloc(pool, 64);
    TEST_ASSERT_NOT_NULL(ptr3, "分配64字节失败");
    
    /* 测试释放 */
    vox_mpool_free(pool, ptr1);
    vox_mpool_free(pool, ptr2);
    vox_mpool_free(pool, ptr3);
    
    vox_mpool_destroy(pool);
}

/* 测试大块分配 */
static void test_mpool_large_alloc(vox_mpool_t* mpool) {
    VOX_UNUSED(mpool);
    vox_mpool_t* pool = vox_mpool_create();
    TEST_ASSERT_NOT_NULL(pool, "创建内存池失败");
    
    /* 测试超过8192字节的大块分配 */
    void* ptr = vox_mpool_alloc(pool, 16384);
    TEST_ASSERT_NOT_NULL(ptr, "分配大块内存失败");
    
    /* 测试写入和读取 */
    memset(ptr, 0xAA, 16384);
    uint8_t* bytes = (uint8_t*)ptr;
    TEST_ASSERT_EQ(bytes[0], 0xAA, "大块内存写入失败");
    TEST_ASSERT_EQ(bytes[16383], 0xAA, "大块内存写入失败");
    
    vox_mpool_free(pool, ptr);
    vox_mpool_destroy(pool);
}

/* 测试realloc */
static void test_mpool_realloc(vox_mpool_t* mpool) {
    VOX_UNUSED(mpool);
    vox_mpool_t* pool = vox_mpool_create();
    TEST_ASSERT_NOT_NULL(pool, "创建内存池失败");
    
    /* 测试从NULL realloc */
    void* ptr = vox_mpool_realloc(pool, NULL, 64);
    TEST_ASSERT_NOT_NULL(ptr, "realloc(NULL, 64)失败");
    
    /* 测试扩大 */
    void* ptr2 = vox_mpool_realloc(pool, ptr, 128);
    TEST_ASSERT_NOT_NULL(ptr2, "realloc扩大失败");
    
    /* 测试缩小 */
    void* ptr3 = vox_mpool_realloc(pool, ptr2, 32);
    TEST_ASSERT_NOT_NULL(ptr3, "realloc缩小失败");
    
    /* 测试释放 */
    vox_mpool_realloc(pool, ptr3, 0);
    
    vox_mpool_destroy(pool);
}

/* 测试获取块大小 */
static void test_mpool_get_size(vox_mpool_t* mpool) {
    VOX_UNUSED(mpool);
    vox_mpool_t* pool = vox_mpool_create();
    TEST_ASSERT_NOT_NULL(pool, "创建内存池失败");
    
    void* ptr1 = vox_mpool_alloc(pool, 16);
    size_t size1 = vox_mpool_get_size(pool, ptr1);
    TEST_ASSERT_EQ(size1, 16, "获取16字节块大小失败");
    
    void* ptr2 = vox_mpool_alloc(pool, 128);
    size_t size2 = vox_mpool_get_size(pool, ptr2);
    TEST_ASSERT_EQ(size2, 128, "获取128字节块大小失败");
    
    void* ptr3 = vox_mpool_alloc(pool, 16384);
    size_t size3 = vox_mpool_get_size(pool, ptr3);
    TEST_ASSERT_EQ(size3, 16384, "获取大块大小失败");
    
    vox_mpool_free(pool, ptr1);
    vox_mpool_free(pool, ptr2);
    vox_mpool_free(pool, ptr3);
    
    vox_mpool_destroy(pool);
}

/* 测试重置内存池 */
static void test_mpool_reset(vox_mpool_t* mpool) {
    VOX_UNUSED(mpool);
    vox_mpool_t* pool = vox_mpool_create();
    TEST_ASSERT_NOT_NULL(pool, "创建内存池失败");
    
    /* 分配一些内存 */
    void* ptr1 = vox_mpool_alloc(pool, 64);
    void* ptr2 = vox_mpool_alloc(pool, 128);
    TEST_ASSERT_NOT_NULL(ptr1, "分配失败");
    TEST_ASSERT_NOT_NULL(ptr2, "分配失败");
    
    /* 重置内存池 */
    vox_mpool_reset(pool);
    
    /* 重置后应该可以继续分配 */
    void* ptr3 = vox_mpool_alloc(pool, 64);
    TEST_ASSERT_NOT_NULL(ptr3, "重置后分配失败");
    
    vox_mpool_free(pool, ptr3);
    vox_mpool_destroy(pool);
}

/* 测试边界情况 */
static void test_mpool_edge_cases(vox_mpool_t* mpool) {
    VOX_UNUSED(mpool);
    vox_mpool_t* pool = vox_mpool_create();
    TEST_ASSERT_NOT_NULL(pool, "创建内存池失败");
    
    /* 测试分配0字节 */
    void* ptr = vox_mpool_alloc(pool, 0);
    TEST_ASSERT_NULL(ptr, "分配0字节应该返回NULL");
    
    /* 测试释放NULL */
    vox_mpool_free(pool, NULL);  /* 应该不会崩溃 */
    
    /* 测试所有支持的块大小 */
    size_t sizes[] = {16, 32, 64, 128, 256, 512, 1024, 2048, 4096, 8192};
    void* ptrs[10];
    
    for (int i = 0; i < 10; i++) {
        ptrs[i] = vox_mpool_alloc(pool, sizes[i]);
        TEST_ASSERT_NOT_NULL(ptrs[i], "分配失败");
    }
    
    for (int i = 0; i < 10; i++) {
        vox_mpool_free(pool, ptrs[i]);
    }
    
    vox_mpool_destroy(pool);
}

/* 线程安全测试数据结构 */
typedef struct {
    vox_mpool_t* pool;
    int* success_count;
    int* fail_count;
    int iterations;
    size_t alloc_size;
} mpool_thread_data_t;

/* 线程安全测试线程函数 - 分配和释放 */
static int mpool_thread_alloc_free_func(void* user_data) {
    mpool_thread_data_t* data = (mpool_thread_data_t*)user_data;
    
    for (int i = 0; i < data->iterations; i++) {
        /* 分配内存 */
        void* ptr = vox_mpool_alloc(data->pool, data->alloc_size);
        if (ptr) {
            /* 写入数据验证 */
            memset(ptr, (uint8_t)(i & 0xFF), data->alloc_size);
            
            /* 验证数据 */
            uint8_t* bytes = (uint8_t*)ptr;
            int valid = 1;
            for (size_t j = 0; j < data->alloc_size; j++) {
                if (bytes[j] != (uint8_t)(i & 0xFF)) {
                    valid = 0;
                    break;
                }
            }
            
            if (valid) {
                /* 释放内存 */
                vox_mpool_free(data->pool, ptr);
                (*data->success_count)++;
            } else {
                vox_mpool_free(data->pool, ptr);
                (*data->fail_count)++;
            }
        } else {
            (*data->fail_count)++;
        }
    }
    
    return 0;
}

/* 线程安全测试线程函数 - 分配、使用、释放 */
static int mpool_thread_work_func(void* user_data) {
    mpool_thread_data_t* data = (mpool_thread_data_t*)user_data;
    
    void* ptrs[10];
    int ptr_count = 0;
    
    for (int i = 0; i < data->iterations; i++) {
        /* 随机分配或释放 */
        if (ptr_count < 10 && (i % 3 != 0)) {
            /* 分配 */
            void* ptr = vox_mpool_alloc(data->pool, data->alloc_size);
            if (ptr) {
                memset(ptr, (uint8_t)(i & 0xFF), data->alloc_size);
                ptrs[ptr_count++] = ptr;
                (*data->success_count)++;
            } else {
                (*data->fail_count)++;
            }
        } else if (ptr_count > 0) {
            /* 释放 */
            ptr_count--;
            vox_mpool_free(data->pool, ptrs[ptr_count]);
        }
    }
    
    /* 释放剩余的内存 */
    for (int i = 0; i < ptr_count; i++) {
        vox_mpool_free(data->pool, ptrs[i]);
    }
    
    return 0;
}

/* 测试线程安全的内存池 - 基本分配释放 */
static void test_mpool_thread_safe_basic(vox_mpool_t* mpool) {
    /* 创建线程安全的内存池 */
    vox_mpool_config_t config = {0};
    config.thread_safe = 1;
    vox_mpool_t* pool = vox_mpool_create_with_config(&config);
    TEST_ASSERT_NOT_NULL(pool, "创建线程安全内存池失败");
    
    int success_count = 0;
    int fail_count = 0;
    mpool_thread_data_t data = {
        pool,
        &success_count,
        &fail_count,
        500,  /* 每个线程500次操作 */
        64    /* 分配64字节 */
    };
    
    /* 创建5个线程同时进行分配和释放 */
    vox_thread_t* threads[5];
    for (int i = 0; i < 5; i++) {
        threads[i] = vox_thread_create(mpool, mpool_thread_alloc_free_func, &data);
        TEST_ASSERT_NOT_NULL(threads[i], "创建线程失败");
    }
    
    /* 等待所有线程完成 */
    for (int i = 0; i < 5; i++) {
        vox_thread_join(threads[i], NULL);
    }
    
    /* 验证：应该有大量成功操作，失败应该很少或为0 */
    TEST_ASSERT_GT(success_count, 2000, "线程安全测试：成功操作数过少");
    TEST_ASSERT_EQ(fail_count, 0, "线程安全测试：不应该有失败操作");
    
    vox_mpool_destroy(pool);
}

/* 测试线程安全的内存池 - 混合操作 */
static void test_mpool_thread_safe_mixed(vox_mpool_t* mpool) {
    /* 创建线程安全的内存池 */
    vox_mpool_config_t config = {0};
    config.thread_safe = 1;
    vox_mpool_t* pool = vox_mpool_create_with_config(&config);
    TEST_ASSERT_NOT_NULL(pool, "创建线程安全内存池失败");
    
    int success_count = 0;
    int fail_count = 0;
    mpool_thread_data_t data = {
        pool,
        &success_count,
        &fail_count,
        300,  /* 每个线程300次操作 */
        128   /* 分配128字节 */
    };
    
    /* 创建8个线程同时进行混合操作 */
    vox_thread_t* threads[8];
    for (int i = 0; i < 8; i++) {
        threads[i] = vox_thread_create(mpool, mpool_thread_work_func, &data);
        TEST_ASSERT_NOT_NULL(threads[i], "创建线程失败");
    }
    
    /* 等待所有线程完成 */
    for (int i = 0; i < 8; i++) {
        vox_thread_join(threads[i], NULL);
    }
    
    /* 验证：应该有大量成功操作 */
    TEST_ASSERT_GT(success_count, 1000, "线程安全测试：成功操作数过少");
    
    vox_mpool_destroy(pool);
}

/* 测试线程安全的内存池 - 不同大小分配 */
static void test_mpool_thread_safe_various_sizes(vox_mpool_t* mpool) {
    /* 创建线程安全的内存池 */
    vox_mpool_config_t config = {0};
    config.thread_safe = 1;
    vox_mpool_t* pool = vox_mpool_create_with_config(&config);
    TEST_ASSERT_NOT_NULL(pool, "创建线程安全内存池失败");
    
    /* 测试不同大小的分配 */
    size_t sizes[] = {16, 32, 64, 128, 256, 512, 1024, 2048};
    int thread_count = 4;
    int iterations = 200;
    
    int total_success = 0;
    int total_fail = 0;
    
    /* 对每个大小进行多线程测试 */
    for (size_t s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++) {
        int success_count = 0;
        int fail_count = 0;
        mpool_thread_data_t data = {
            pool,
            &success_count,
            &fail_count,
            iterations,
            sizes[s]
        };
        
        vox_thread_t* threads[4];
        for (int i = 0; i < thread_count; i++) {
            threads[i] = vox_thread_create(mpool, mpool_thread_alloc_free_func, &data);
            TEST_ASSERT_NOT_NULL(threads[i], "创建线程失败");
        }
        
        /* 等待所有线程完成 */
        for (int i = 0; i < thread_count; i++) {
            vox_thread_join(threads[i], NULL);
        }
        
        total_success += success_count;
        total_fail += fail_count;
    }
    
    /* 验证：应该有大量成功操作，失败应该很少或为0 */
    TEST_ASSERT_GT(total_success, 5000, "线程安全测试：总成功操作数过少");
    TEST_ASSERT_EQ(total_fail, 0, "线程安全测试：不应该有失败操作");
    
    vox_mpool_destroy(pool);
}

/* 测试套件 */
test_case_t test_mpool_cases[] = {
    {"create_destroy", test_mpool_create_destroy},
    {"create_with_config", test_mpool_create_with_config},
    {"alloc_free", test_mpool_alloc_free},
    {"large_alloc", test_mpool_large_alloc},
    {"realloc", test_mpool_realloc},
    {"get_size", test_mpool_get_size},
    {"reset", test_mpool_reset},
    {"edge_cases", test_mpool_edge_cases},
    {"thread_safe_basic", test_mpool_thread_safe_basic},
    {"thread_safe_mixed", test_mpool_thread_safe_mixed},
    {"thread_safe_various_sizes", test_mpool_thread_safe_various_sizes},
};

test_suite_t test_mpool_suite = {
    "vox_mpool",
    test_mpool_cases,
    sizeof(test_mpool_cases) / sizeof(test_mpool_cases[0])
};
