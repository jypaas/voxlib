/* ============================================================
 * test_vector.c - vox_vector 模块测试
 * ============================================================ */

#include "test_runner.h"
#include "../vox_vector.h"

/* 测试创建和销毁 */
static void test_vector_create_destroy(vox_mpool_t* mpool) {
    vox_vector_t* vec = vox_vector_create(mpool);
    TEST_ASSERT_NOT_NULL(vec, "创建vector失败");
    TEST_ASSERT_EQ(vox_vector_size(vec), 0, "新vector大小应为0");
    TEST_ASSERT_EQ(vox_vector_empty(vec), 1, "新vector应为空");
    vox_vector_destroy(vec);
}

/* 测试push和pop */
static void test_vector_push_pop(vox_mpool_t* mpool) {
    vox_vector_t* vec = vox_vector_create(mpool);
    TEST_ASSERT_NOT_NULL(vec, "创建vector失败");
    
    int values[] = {1, 2, 3, 4, 5};
    
    /* 测试push */
    for (int i = 0; i < 5; i++) {
        TEST_ASSERT_EQ(vox_vector_push(vec, &values[i]), 0, "push失败");
        TEST_ASSERT_EQ(vox_vector_size(vec), (size_t)(i + 1), "vector大小不正确");
    }
    
    /* 测试get */
    for (int i = 0; i < 5; i++) {
        int* val = (int*)vox_vector_get(vec, i);
        TEST_ASSERT_NOT_NULL(val, "get失败");
        TEST_ASSERT_EQ(*val, values[i], "get的值不正确");
    }
    
    /* 测试pop */
    for (int i = 4; i >= 0; i--) {
        int* val = (int*)vox_vector_pop(vec);
        TEST_ASSERT_NOT_NULL(val, "pop失败");
        TEST_ASSERT_EQ(*val, values[i], "pop的值不正确");
        TEST_ASSERT_EQ(vox_vector_size(vec), (size_t)i, "pop后大小不正确");
    }
    
    TEST_ASSERT_EQ(vox_vector_empty(vec), 1, "vector应为空");
    
    vox_vector_destroy(vec);
}

/* 测试insert和remove */
static void test_vector_insert_remove(vox_mpool_t* mpool) {
    vox_vector_t* vec = vox_vector_create(mpool);
    TEST_ASSERT_NOT_NULL(vec, "创建vector失败");
    
    int val1 = 1, val2 = 2, val3 = 3;
    
    vox_vector_push(vec, &val1);
    vox_vector_push(vec, &val3);
    
    /* 在位置1插入val2 */
    TEST_ASSERT_EQ(vox_vector_insert(vec, 1, &val2), 0, "insert失败");
    TEST_ASSERT_EQ(vox_vector_size(vec), 3, "insert后大小不正确");
    
    int* v1 = (int*)vox_vector_get(vec, 0);
    int* v2 = (int*)vox_vector_get(vec, 1);
    int* v3 = (int*)vox_vector_get(vec, 2);
    TEST_ASSERT_EQ(*v1, 1, "位置0的值不正确");
    TEST_ASSERT_EQ(*v2, 2, "位置1的值不正确");
    TEST_ASSERT_EQ(*v3, 3, "位置2的值不正确");
    
    /* 移除位置1的元素 */
    int* removed = (int*)vox_vector_remove(vec, 1);
    TEST_ASSERT_NOT_NULL(removed, "remove失败");
    TEST_ASSERT_EQ(*removed, 2, "remove的值不正确");
    TEST_ASSERT_EQ(vox_vector_size(vec), 2, "remove后大小不正确");
    
    vox_vector_destroy(vec);
}

/* 测试set和get */
static void test_vector_set_get(vox_mpool_t* mpool) {
    vox_vector_t* vec = vox_vector_create(mpool);
    TEST_ASSERT_NOT_NULL(vec, "创建vector失败");
    
    int val1 = 10, val2 = 20;
    vox_vector_push(vec, &val1);
    
    TEST_ASSERT_EQ(vox_vector_set(vec, 0, &val2), 0, "set失败");
    int* v = (int*)vox_vector_get(vec, 0);
    TEST_ASSERT_NOT_NULL(v, "get失败");
    TEST_ASSERT_EQ(*v, 20, "set的值不正确");
    
    vox_vector_destroy(vec);
}

/* 测试clear和resize */
static void test_vector_clear_resize(vox_mpool_t* mpool) {
    vox_vector_t* vec = vox_vector_create(mpool);
    TEST_ASSERT_NOT_NULL(vec, "创建vector失败");
    
    int values[] = {1, 2, 3};
    for (int i = 0; i < 3; i++) {
        vox_vector_push(vec, &values[i]);
    }
    
    vox_vector_clear(vec);
    TEST_ASSERT_EQ(vox_vector_size(vec), 0, "clear后大小应为0");
    TEST_ASSERT_EQ(vox_vector_empty(vec), 1, "clear后应为空");
    
    /* 测试resize */
    TEST_ASSERT_EQ(vox_vector_resize(vec, 5), 0, "resize失败");
    TEST_ASSERT_EQ(vox_vector_size(vec), 5, "resize后大小不正确");
    
    vox_vector_destroy(vec);
}

/* 测试边界情况 */
static void test_vector_edge_cases(vox_mpool_t* mpool) {
    vox_vector_t* vec = vox_vector_create(mpool);
    TEST_ASSERT_NOT_NULL(vec, "创建vector失败");
    
    /* 测试在空vector上pop */
    void* val = vox_vector_pop(vec);
    TEST_ASSERT_NULL(val, "从空vector pop应返回NULL");
    
    /* 测试在空vector上get */
    val = vox_vector_get(vec, 0);
    TEST_ASSERT_NULL(val, "从空vector get应返回NULL");
    
    /* 测试在空vector上remove */
    val = vox_vector_remove(vec, 0);
    TEST_ASSERT_NULL(val, "从空vector remove应返回NULL");
    
    /* 测试单个元素 */
    int single = 42;
    vox_vector_push(vec, &single);
    TEST_ASSERT_EQ(vox_vector_size(vec), 1, "单元素vector大小应为1");
    
    int* v = (int*)vox_vector_get(vec, 0);
    TEST_ASSERT_NOT_NULL(v, "获取单元素失败");
    TEST_ASSERT_EQ(*v, 42, "单元素值不正确");
    
    vox_vector_destroy(vec);
}

/* 测试大量数据 */
static void test_vector_large_data(vox_mpool_t* mpool) {
    vox_vector_t* vec = vox_vector_create(mpool);
    TEST_ASSERT_NOT_NULL(vec, "创建vector失败");
    
    /* 添加大量元素 */
    int count = 1000;
    int* values = (int*)vox_mpool_alloc(mpool, count * sizeof(int));
    for (int i = 0; i < count; i++) {
        values[i] = i;
        TEST_ASSERT_EQ(vox_vector_push(vec, &values[i]), 0, "push大量数据失败");
    }
    
    TEST_ASSERT_EQ(vox_vector_size(vec), (size_t)count, "大量数据后大小不正确");
    
    /* 验证所有数据 */
    for (int i = 0; i < count; i++) {
        int* v = (int*)vox_vector_get(vec, i);
        TEST_ASSERT_NOT_NULL(v, "获取大量数据失败");
        TEST_ASSERT_EQ(*v, i, "大量数据值不正确");
    }
    
    vox_vector_destroy(vec);
}

/* 测试insert边界 */
static void test_vector_insert_boundary(vox_mpool_t* mpool) {
    vox_vector_t* vec = vox_vector_create(mpool);
    TEST_ASSERT_NOT_NULL(vec, "创建vector失败");
    
    int val1 = 1, val2 = 2, val3 = 3;
    
    /* 在空vector开头插入 */
    TEST_ASSERT_EQ(vox_vector_insert(vec, 0, &val1), 0, "在开头插入失败");
    
    /* 在末尾插入 */
    TEST_ASSERT_EQ(vox_vector_insert(vec, 1, &val3), 0, "在末尾插入失败");
    
    /* 在中间插入 */
    TEST_ASSERT_EQ(vox_vector_insert(vec, 1, &val2), 0, "在中间插入失败");
    
    /* 验证顺序 */
    TEST_ASSERT_EQ(*(int*)vox_vector_get(vec, 0), 1, "位置0值不正确");
    TEST_ASSERT_EQ(*(int*)vox_vector_get(vec, 1), 2, "位置1值不正确");
    TEST_ASSERT_EQ(*(int*)vox_vector_get(vec, 2), 3, "位置2值不正确");
    
    vox_vector_destroy(vec);
}

/* 测试套件 */
test_case_t test_vector_cases[] = {
    {"create_destroy", test_vector_create_destroy},
    {"push_pop", test_vector_push_pop},
    {"insert_remove", test_vector_insert_remove},
    {"set_get", test_vector_set_get},
    {"clear_resize", test_vector_clear_resize},
    {"edge_cases", test_vector_edge_cases},
    {"large_data", test_vector_large_data},
    {"insert_boundary", test_vector_insert_boundary},
};

test_suite_t test_vector_suite = {
    "vox_vector",
    test_vector_cases,
    sizeof(test_vector_cases) / sizeof(test_vector_cases[0])
};
