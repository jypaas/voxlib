/* ============================================================
 * test_rbtree.c - vox_rbtree 模块测试
 * ============================================================ */

#include "test_runner.h"
#include "../vox_rbtree.h"
#include <string.h>

/* 测试创建和销毁 */
static void test_rbtree_create_destroy(vox_mpool_t* mpool) {
    vox_rbtree_t* tree = vox_rbtree_create(mpool);
    TEST_ASSERT_NOT_NULL(tree, "创建rbtree失败");
    TEST_ASSERT_EQ(vox_rbtree_size(tree), 0, "新rbtree大小应为0");
    TEST_ASSERT_EQ(vox_rbtree_empty(tree), 1, "新rbtree应为空");
    vox_rbtree_destroy(tree);
}

/* 测试insert和find */
static void test_rbtree_insert_find(vox_mpool_t* mpool) {
    vox_rbtree_t* tree = vox_rbtree_create(mpool);
    TEST_ASSERT_NOT_NULL(tree, "创建rbtree失败");
    
    const char* keys[] = {"key1", "key2", "key3"};
    int values[] = {10, 20, 30};
    
    for (int i = 0; i < 3; i++) {
        TEST_ASSERT_EQ(vox_rbtree_insert(tree, keys[i], strlen(keys[i]), &values[i]), 0, "insert失败");
    }
    
    TEST_ASSERT_EQ(vox_rbtree_size(tree), 3, "insert后大小不正确");
    
    for (int i = 0; i < 3; i++) {
        int* val = (int*)vox_rbtree_find(tree, keys[i], strlen(keys[i]));
        TEST_ASSERT_NOT_NULL(val, "find失败");
        TEST_ASSERT_EQ(*val, values[i], "find的值不正确");
    }
    
    vox_rbtree_destroy(tree);
}

/* 测试contains */
static void test_rbtree_contains(vox_mpool_t* mpool) {
    vox_rbtree_t* tree = vox_rbtree_create(mpool);
    TEST_ASSERT_NOT_NULL(tree, "创建rbtree失败");
    
    const char* key = "test_key";
    int value = 42;
    
    TEST_ASSERT_EQ(vox_rbtree_contains(tree, key, strlen(key)), 0, "空树不应包含key");
    
    vox_rbtree_insert(tree, key, strlen(key), &value);
    TEST_ASSERT_EQ(vox_rbtree_contains(tree, key, strlen(key)), 1, "应包含key");
    
    vox_rbtree_destroy(tree);
}

/* 测试delete */
static void test_rbtree_delete(vox_mpool_t* mpool) {
    vox_rbtree_t* tree = vox_rbtree_create(mpool);
    TEST_ASSERT_NOT_NULL(tree, "创建rbtree失败");
    
    const char* key = "delete_key";
    int value = 99;
    
    vox_rbtree_insert(tree, key, strlen(key), &value);
    TEST_ASSERT_EQ(vox_rbtree_size(tree), 1, "insert后大小应为1");
    
    TEST_ASSERT_EQ(vox_rbtree_delete(tree, key, strlen(key)), 0, "delete失败");
    TEST_ASSERT_EQ(vox_rbtree_size(tree), 0, "delete后大小应为0");
    TEST_ASSERT_EQ(vox_rbtree_contains(tree, key, strlen(key)), 0, "delete后不应包含key");
    
    /* 删除不存在的key应该返回-1 */
    TEST_ASSERT_EQ(vox_rbtree_delete(tree, "nonexistent", 11), -1, "删除不存在的key应返回-1");
    
    vox_rbtree_destroy(tree);
}

/* 测试排序特性 */
static void test_rbtree_ordering(vox_mpool_t* mpool) {
    vox_rbtree_t* tree = vox_rbtree_create(mpool);
    TEST_ASSERT_NOT_NULL(tree, "创建rbtree失败");
    
    const char* keys[] = {"c", "a", "b", "d"};
    int values[] = {3, 1, 2, 4};
    
    for (int i = 0; i < 4; i++) {
        vox_rbtree_insert(tree, keys[i], strlen(keys[i]), &values[i]);
    }
    
    /* 测试min和max */
    const void* min_key;
    size_t min_key_len;
    TEST_ASSERT_EQ(vox_rbtree_min(tree, &min_key, &min_key_len), 0, "获取min失败");
    TEST_ASSERT_EQ(memcmp(min_key, "a", 1), 0, "min key不正确");
    
    const void* max_key;
    size_t max_key_len;
    TEST_ASSERT_EQ(vox_rbtree_max(tree, &max_key, &max_key_len), 0, "获取max失败");
    TEST_ASSERT_EQ(memcmp(max_key, "d", 1), 0, "max key不正确");
    
    vox_rbtree_destroy(tree);
}

/* 测试clear */
static void test_rbtree_clear(vox_mpool_t* mpool) {
    vox_rbtree_t* tree = vox_rbtree_create(mpool);
    TEST_ASSERT_NOT_NULL(tree, "创建rbtree失败");
    
    const char* keys[] = {"key1", "key2", "key3"};
    int values[] = {1, 2, 3};
    
    for (int i = 0; i < 3; i++) {
        vox_rbtree_insert(tree, keys[i], strlen(keys[i]), &values[i]);
    }
    
    vox_rbtree_clear(tree);
    TEST_ASSERT_EQ(vox_rbtree_size(tree), 0, "clear后大小应为0");
    TEST_ASSERT_EQ(vox_rbtree_empty(tree), 1, "clear后应为空");
    
    vox_rbtree_destroy(tree);
}

/* 测试套件 */
test_case_t test_rbtree_cases[] = {
    {"create_destroy", test_rbtree_create_destroy},
    {"insert_find", test_rbtree_insert_find},
    {"contains", test_rbtree_contains},
    {"delete", test_rbtree_delete},
    {"ordering", test_rbtree_ordering},
    {"clear", test_rbtree_clear},
};

test_suite_t test_rbtree_suite = {
    "vox_rbtree",
    test_rbtree_cases,
    sizeof(test_rbtree_cases) / sizeof(test_rbtree_cases[0])
};
