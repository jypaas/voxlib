/* ============================================================
 * test_htable.c - vox_htable 模块测试
 * ============================================================ */

#include "test_runner.h"
#include "../vox_htable.h"
#include <string.h>
#include <stdio.h>

/* 测试创建和销毁 */
static void test_htable_create_destroy(vox_mpool_t* mpool) {
    vox_htable_t* htable = vox_htable_create(mpool);
    TEST_ASSERT_NOT_NULL(htable, "创建htable失败");
    TEST_ASSERT_EQ(vox_htable_size(htable), 0, "新htable大小应为0");
    TEST_ASSERT_EQ(vox_htable_empty(htable), 1, "新htable应为空");
    vox_htable_destroy(htable);
}

/* 测试set和get */
static void test_htable_set_get(vox_mpool_t* mpool) {
    vox_htable_t* htable = vox_htable_create(mpool);
    TEST_ASSERT_NOT_NULL(htable, "创建htable失败");
    
    const char* key1 = "key1";
    int value1 = 100;
    
    TEST_ASSERT_EQ(vox_htable_set(htable, key1, strlen(key1), &value1), 0, "set失败");
    TEST_ASSERT_EQ(vox_htable_size(htable), 1, "set后大小不正确");
    
    int* val = (int*)vox_htable_get(htable, key1, strlen(key1));
    TEST_ASSERT_NOT_NULL(val, "get失败");
    TEST_ASSERT_EQ(*val, 100, "get的值不正确");
    
    vox_htable_destroy(htable);
}

/* 测试contains */
static void test_htable_contains(vox_mpool_t* mpool) {
    vox_htable_t* htable = vox_htable_create(mpool);
    TEST_ASSERT_NOT_NULL(htable, "创建htable失败");
    
    const char* key = "test_key";
    int value = 42;
    
    TEST_ASSERT_EQ(vox_htable_contains(htable, key, strlen(key)), 0, "空表不应包含key");
    
    vox_htable_set(htable, key, strlen(key), &value);
    TEST_ASSERT_EQ(vox_htable_contains(htable, key, strlen(key)), 1, "应包含key");
    
    vox_htable_destroy(htable);
}

/* 测试delete */
static void test_htable_delete(vox_mpool_t* mpool) {
    vox_htable_t* htable = vox_htable_create(mpool);
    TEST_ASSERT_NOT_NULL(htable, "创建htable失败");
    
    const char* key = "delete_key";
    int value = 99;
    
    vox_htable_set(htable, key, strlen(key), &value);
    TEST_ASSERT_EQ(vox_htable_size(htable), 1, "set后大小应为1");
    
    TEST_ASSERT_EQ(vox_htable_delete(htable, key, strlen(key)), 0, "delete失败");
    TEST_ASSERT_EQ(vox_htable_size(htable), 0, "delete后大小应为0");
    TEST_ASSERT_EQ(vox_htable_contains(htable, key, strlen(key)), 0, "delete后不应包含key");
    
    /* 删除不存在的key应该返回-1 */
    TEST_ASSERT_EQ(vox_htable_delete(htable, "nonexistent", 11), -1, "删除不存在的key应返回-1");
    
    vox_htable_destroy(htable);
}

/* 测试更新值 */
static void test_htable_update(vox_mpool_t* mpool) {
    vox_htable_t* htable = vox_htable_create(mpool);
    TEST_ASSERT_NOT_NULL(htable, "创建htable失败");
    
    const char* key = "update_key";
    int value1 = 10, value2 = 20;
    
    vox_htable_set(htable, key, strlen(key), &value1);
    vox_htable_set(htable, key, strlen(key), &value2);  /* 更新值 */
    
    TEST_ASSERT_EQ(vox_htable_size(htable), 1, "更新后大小仍应为1");
    
    int* val = (int*)vox_htable_get(htable, key, strlen(key));
    TEST_ASSERT_NOT_NULL(val, "get失败");
    TEST_ASSERT_EQ(*val, 20, "更新后的值不正确");
    
    vox_htable_destroy(htable);
}

/* 测试clear */
static void test_htable_clear(vox_mpool_t* mpool) {
    vox_htable_t* htable = vox_htable_create(mpool);
    TEST_ASSERT_NOT_NULL(htable, "创建htable失败");
    
    const char* keys[] = {"key1", "key2", "key3"};
    int values[] = {1, 2, 3};
    
    for (int i = 0; i < 3; i++) {
        vox_htable_set(htable, keys[i], strlen(keys[i]), &values[i]);
    }
    
    vox_htable_clear(htable);
    TEST_ASSERT_EQ(vox_htable_size(htable), 0, "clear后大小应为0");
    TEST_ASSERT_EQ(vox_htable_empty(htable), 1, "clear后应为空");
    
    vox_htable_destroy(htable);
}

/* 测试大量键值对 */
static void test_htable_large_data(vox_mpool_t* mpool) {
    vox_htable_t* htable = vox_htable_create(mpool);
    TEST_ASSERT_NOT_NULL(htable, "创建htable失败");
    
    int count = 500;
    char** keys = (char**)vox_mpool_alloc(mpool, count * sizeof(char*));
    int* values = (int*)vox_mpool_alloc(mpool, count * sizeof(int));
    
    /* 生成大量键值对 */
    for (int i = 0; i < count; i++) {
        keys[i] = (char*)vox_mpool_alloc(mpool, 32);
        sprintf(keys[i], "key_%d", i);
        values[i] = i * 10;
        TEST_ASSERT_EQ(vox_htable_set(htable, keys[i], strlen(keys[i]), &values[i]), 0, "设置大量数据失败");
    }
    
    TEST_ASSERT_EQ(vox_htable_size(htable), (size_t)count, "大量数据后大小不正确");
    
    /* 验证所有数据 */
    for (int i = 0; i < count; i++) {
        int* val = (int*)vox_htable_get(htable, keys[i], strlen(keys[i]));
        TEST_ASSERT_NOT_NULL(val, "获取大量数据失败");
        TEST_ASSERT_EQ(*val, i * 10, "大量数据值不正确");
    }
    
    vox_htable_destroy(htable);
}

/* 测试哈希冲突 */
static void test_htable_collision(vox_mpool_t* mpool) {
    vox_htable_t* htable = vox_htable_create(mpool);
    TEST_ASSERT_NOT_NULL(htable, "创建htable失败");
    
    /* 使用相似的键来测试哈希冲突处理 */
    const char* keys[] = {"a", "b", "c", "d", "e", "f", "g", "h"};
    int values[] = {1, 2, 3, 4, 5, 6, 7, 8};
    
    for (int i = 0; i < 8; i++) {
        TEST_ASSERT_EQ(vox_htable_set(htable, keys[i], strlen(keys[i]), &values[i]), 0, "设置键值对失败");
    }
    
    /* 验证所有键值对都能正确获取 */
    for (int i = 0; i < 8; i++) {
        int* val = (int*)vox_htable_get(htable, keys[i], strlen(keys[i]));
        TEST_ASSERT_NOT_NULL(val, "获取键值对失败");
        TEST_ASSERT_EQ(*val, values[i], "键值对值不正确");
    }
    
    vox_htable_destroy(htable);
}

/* 测试边界情况 */
static void test_htable_edge_cases(vox_mpool_t* mpool) {
    vox_htable_t* htable = vox_htable_create(mpool);
    TEST_ASSERT_NOT_NULL(htable, "创建htable失败");
    
    int* val;
    
    /* 测试空键（如果支持） */
    int value = 42;
    int result = vox_htable_set(htable, "", 0, &value);
    if (result == 0) {
        /* 空键被支持，验证可以获取 */
        val = (int*)vox_htable_get(htable, "", 0);
        TEST_ASSERT_NOT_NULL(val, "获取空键失败");
        TEST_ASSERT_EQ(*val, 42, "空键值不正确");
    }
    /* 如果空键不被支持（返回-1），这是可以接受的，不进行验证 */
    
    /* 测试长键 */
    char long_key[256];
    memset(long_key, 'A', 255);
    long_key[255] = '\0';
    int long_value = 999;
    TEST_ASSERT_EQ(vox_htable_set(htable, long_key, 255, &long_value), 0, "设置长键失败");
    val = (int*)vox_htable_get(htable, long_key, 255);
    TEST_ASSERT_NOT_NULL(val, "获取长键失败");
    TEST_ASSERT_EQ(*val, 999, "长键值不正确");
    
    /* 测试不存在的键 */
    val = (int*)vox_htable_get(htable, "nonexistent", 11);
    TEST_ASSERT_NULL(val, "获取不存在的键应返回NULL");
    
    vox_htable_destroy(htable);
}

/* 测试删除和重新插入 */
static void test_htable_delete_reinsert(vox_mpool_t* mpool) {
    vox_htable_t* htable = vox_htable_create(mpool);
    TEST_ASSERT_NOT_NULL(htable, "创建htable失败");
    
    const char* key = "test_key";
    int value1 = 100, value2 = 200;
    
    /* 设置值 */
    vox_htable_set(htable, key, strlen(key), &value1);
    TEST_ASSERT_EQ(vox_htable_size(htable), 1, "设置后大小应为1");
    
    /* 删除 */
    TEST_ASSERT_EQ(vox_htable_delete(htable, key, strlen(key)), 0, "删除失败");
    TEST_ASSERT_EQ(vox_htable_size(htable), 0, "删除后大小应为0");
    
    /* 重新插入 */
    vox_htable_set(htable, key, strlen(key), &value2);
    TEST_ASSERT_EQ(vox_htable_size(htable), 1, "重新插入后大小应为1");
    
    int* val = (int*)vox_htable_get(htable, key, strlen(key));
    TEST_ASSERT_NOT_NULL(val, "获取重新插入的值失败");
    TEST_ASSERT_EQ(*val, 200, "重新插入的值不正确");
    
    vox_htable_destroy(htable);
}

/* 测试套件 */
test_case_t test_htable_cases[] = {
    {"create_destroy", test_htable_create_destroy},
    {"set_get", test_htable_set_get},
    {"contains", test_htable_contains},
    {"delete", test_htable_delete},
    {"update", test_htable_update},
    {"clear", test_htable_clear},
    {"large_data", test_htable_large_data},
    {"collision", test_htable_collision},
    {"edge_cases", test_htable_edge_cases},
    {"delete_reinsert", test_htable_delete_reinsert},
};

test_suite_t test_htable_suite = {
    "vox_htable",
    test_htable_cases,
    sizeof(test_htable_cases) / sizeof(test_htable_cases[0])
};
