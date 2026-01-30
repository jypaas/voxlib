/* ============================================================
 * test_mheap.c - vox_mheap 模块测试
 * ============================================================ */

#include "test_runner.h"
#include "../vox_mheap.h"

/* 整数比较函数 */
static int int_cmp(const void* a, const void* b) {
    int ia = *(const int*)a;
    int ib = *(const int*)b;
    if (ia < ib) return -1;
    if (ia > ib) return 1;
    return 0;
}

/* 测试创建和销毁 */
static void test_mheap_create_destroy(vox_mpool_t* mpool) {
    vox_mheap_t* heap = vox_mheap_create(mpool);
    TEST_ASSERT_NOT_NULL(heap, "创建mheap失败");
    TEST_ASSERT_EQ(vox_mheap_size(heap), 0, "新mheap大小应为0");
    TEST_ASSERT_EQ(vox_mheap_empty(heap), 1, "新mheap应为空");
    vox_mheap_destroy(heap);
}

/* 测试push和pop */
static void test_mheap_push_pop(vox_mpool_t* mpool) {
    vox_mheap_config_t config = {0};
    config.cmp_func = int_cmp;
    
    vox_mheap_t* heap = vox_mheap_create_with_config(mpool, &config);
    TEST_ASSERT_NOT_NULL(heap, "创建mheap失败");
    
    int values[] = {5, 2, 8, 1, 9, 3};
    
    /* 测试push */
    for (int i = 0; i < 6; i++) {
        TEST_ASSERT_EQ(vox_mheap_push(heap, &values[i]), 0, "push失败");
        TEST_ASSERT_EQ(vox_mheap_size(heap), (size_t)(i + 1), "push后大小不正确");
    }
    
    /* 测试peek - 应该是最小值 */
    int* min_val = (int*)vox_mheap_peek(heap);
    TEST_ASSERT_NOT_NULL(min_val, "peek失败");
    TEST_ASSERT_EQ(*min_val, 1, "peek应该返回最小值");
    
    /* 测试pop - 应该按从小到大顺序 */
    int expected[] = {1, 2, 3, 5, 8, 9};
    for (int i = 0; i < 6; i++) {
        int* val = (int*)vox_mheap_pop(heap);
        TEST_ASSERT_NOT_NULL(val, "pop失败");
        TEST_ASSERT_EQ(*val, expected[i], "pop的值不正确");
        TEST_ASSERT_EQ(vox_mheap_size(heap), (size_t)(5 - i), "pop后大小不正确");
    }
    
    TEST_ASSERT_EQ(vox_mheap_empty(heap), 1, "heap应为空");
    
    vox_mheap_destroy(heap);
}

/* 测试空堆操作 */
static void test_mheap_empty_ops(vox_mpool_t* mpool) {
    vox_mheap_t* heap = vox_mheap_create(mpool);
    TEST_ASSERT_NOT_NULL(heap, "创建mheap失败");
    
    /* 从空堆pop应该返回NULL */
    void* val = vox_mheap_pop(heap);
    TEST_ASSERT_NULL(val, "从空堆pop应返回NULL");
    
    /* 从空堆peek应该返回NULL */
    val = vox_mheap_peek(heap);
    TEST_ASSERT_NULL(val, "从空堆peek应返回NULL");
    
    vox_mheap_destroy(heap);
}

/* 测试clear */
static void test_mheap_clear(vox_mpool_t* mpool) {
    vox_mheap_config_t config = {0};
    config.cmp_func = int_cmp;
    
    vox_mheap_t* heap = vox_mheap_create_with_config(mpool, &config);
    TEST_ASSERT_NOT_NULL(heap, "创建mheap失败");
    
    int values[] = {5, 2, 8};
    for (int i = 0; i < 3; i++) {
        vox_mheap_push(heap, &values[i]);
    }
    
    vox_mheap_clear(heap);
    TEST_ASSERT_EQ(vox_mheap_size(heap), 0, "clear后大小应为0");
    TEST_ASSERT_EQ(vox_mheap_empty(heap), 1, "clear后应为空");
    
    vox_mheap_destroy(heap);
}

/* 测试最小堆特性 */
static void test_mheap_min_property(vox_mpool_t* mpool) {
    vox_mheap_config_t config = {0};
    config.cmp_func = int_cmp;
    
    vox_mheap_t* heap = vox_mheap_create_with_config(mpool, &config);
    TEST_ASSERT_NOT_NULL(heap, "创建mheap失败");
    
    /* 插入乱序数据 */
    int values[] = {10, 5, 15, 3, 7, 12, 1};
    for (int i = 0; i < 7; i++) {
        vox_mheap_push(heap, &values[i]);
    }
    
    /* 每次pop都应该是最小值 */
    int prev = -1;
    while (!vox_mheap_empty(heap)) {
        int* val = (int*)vox_mheap_pop(heap);
        TEST_ASSERT_NOT_NULL(val, "pop失败");
        TEST_ASSERT_EQ(*val >= prev, 1, "最小堆性质不满足");
        prev = *val;
    }
    
    vox_mheap_destroy(heap);
}

/* 测试套件 */
test_case_t test_mheap_cases[] = {
    {"create_destroy", test_mheap_create_destroy},
    {"push_pop", test_mheap_push_pop},
    {"empty_ops", test_mheap_empty_ops},
    {"clear", test_mheap_clear},
    {"min_property", test_mheap_min_property},
};

test_suite_t test_mheap_suite = {
    "vox_mheap",
    test_mheap_cases,
    sizeof(test_mheap_cases) / sizeof(test_mheap_cases[0])
};
