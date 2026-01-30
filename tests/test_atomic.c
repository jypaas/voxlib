/* ============================================================
 * test_atomic.c - vox_atomic 模块测试
 * ============================================================ */

#include "test_runner.h"
#include "../vox_atomic.h"

/* 测试原子整数创建和销毁 */
static void test_atomic_int_create_destroy(vox_mpool_t* mpool) {
    vox_atomic_int_t* atomic = vox_atomic_int_create(mpool, 42);
    TEST_ASSERT_NOT_NULL(atomic, "创建原子整数失败");
    
    int32_t val = vox_atomic_int_load(atomic);
    TEST_ASSERT_EQ(val, 42, "初始值不正确");
    
    vox_atomic_int_destroy(atomic);
}

/* 测试原子整数load和store */
static void test_atomic_int_load_store(vox_mpool_t* mpool) {
    vox_atomic_int_t* atomic = vox_atomic_int_create(mpool, 0);
    TEST_ASSERT_NOT_NULL(atomic, "创建原子整数失败");
    
    vox_atomic_int_store(atomic, 100);
    int32_t val = vox_atomic_int_load(atomic);
    TEST_ASSERT_EQ(val, 100, "store/load失败");
    
    vox_atomic_int_destroy(atomic);
}

/* 测试原子整数add和sub */
static void test_atomic_int_add_sub(vox_mpool_t* mpool) {
    vox_atomic_int_t* atomic = vox_atomic_int_create(mpool, 10);
    TEST_ASSERT_NOT_NULL(atomic, "创建原子整数失败");
    
    int32_t old = vox_atomic_int_add(atomic, 5);
    TEST_ASSERT_EQ(old, 10, "add返回的旧值不正确");
    
    int32_t val = vox_atomic_int_load(atomic);
    TEST_ASSERT_EQ(val, 15, "add后的值不正确");
    
    old = vox_atomic_int_sub(atomic, 3);
    TEST_ASSERT_EQ(old, 15, "sub返回的旧值不正确");
    
    val = vox_atomic_int_load(atomic);
    TEST_ASSERT_EQ(val, 12, "sub后的值不正确");
    
    vox_atomic_int_destroy(atomic);
}

/* 测试原子整数increment和decrement */
static void test_atomic_int_inc_dec(vox_mpool_t* mpool) {
    vox_atomic_int_t* atomic = vox_atomic_int_create(mpool, 5);
    TEST_ASSERT_NOT_NULL(atomic, "创建原子整数失败");
    
    int32_t val = vox_atomic_int_increment(atomic);
    TEST_ASSERT_EQ(val, 6, "increment后的值不正确");
    
    val = vox_atomic_int_decrement(atomic);
    TEST_ASSERT_EQ(val, 5, "decrement后的值不正确");
    
    vox_atomic_int_destroy(atomic);
}

/* 测试原子整数exchange */
static void test_atomic_int_exchange(vox_mpool_t* mpool) {
    vox_atomic_int_t* atomic = vox_atomic_int_create(mpool, 20);
    TEST_ASSERT_NOT_NULL(atomic, "创建原子整数失败");
    
    int32_t old = vox_atomic_int_exchange(atomic, 30);
    TEST_ASSERT_EQ(old, 20, "exchange返回的旧值不正确");
    
    int32_t val = vox_atomic_int_load(atomic);
    TEST_ASSERT_EQ(val, 30, "exchange后的值不正确");
    
    vox_atomic_int_destroy(atomic);
}

/* 测试原子整数compare_exchange */
static void test_atomic_int_compare_exchange(vox_mpool_t* mpool) {
    vox_atomic_int_t* atomic = vox_atomic_int_create(mpool, 50);
    TEST_ASSERT_NOT_NULL(atomic, "创建原子整数失败");
    
    int32_t expected = 50;
    bool success = vox_atomic_int_compare_exchange(atomic, &expected, 60);
    TEST_ASSERT_EQ(success, 1, "CAS应该成功");
    
    int32_t val = vox_atomic_int_load(atomic);
    TEST_ASSERT_EQ(val, 60, "CAS后的值不正确");
    
    /* 测试失败的CAS */
    expected = 50;  /* 当前值是60，不是50 */
    success = vox_atomic_int_compare_exchange(atomic, &expected, 70);
    TEST_ASSERT_EQ(success, 0, "CAS应该失败");
    TEST_ASSERT_EQ(expected, 60, "expected应该被更新为实际值");
    
    vox_atomic_int_destroy(atomic);
}

/* 测试原子指针 */
static void test_atomic_ptr(vox_mpool_t* mpool) {
    vox_atomic_ptr_t* atomic = vox_atomic_ptr_create(mpool, NULL);
    TEST_ASSERT_NOT_NULL(atomic, "创建原子指针失败");
    
    int value = 42;
    vox_atomic_ptr_store(atomic, &value);
    
    void* ptr = vox_atomic_ptr_load(atomic);
    TEST_ASSERT_EQ(ptr, (void*)&value, "原子指针load/store失败");
    
    int value2 = 100;
    void* old_ptr = vox_atomic_ptr_exchange(atomic, &value2);
    TEST_ASSERT_EQ(old_ptr, (void*)&value, "exchange返回的旧指针不正确");
    
    ptr = vox_atomic_ptr_load(atomic);
    TEST_ASSERT_EQ(ptr, (void*)&value2, "exchange后的指针不正确");
    
    vox_atomic_ptr_destroy(atomic);
}

/* 测试套件 */
test_case_t test_atomic_cases[] = {
    {"int_create_destroy", test_atomic_int_create_destroy},
    {"int_load_store", test_atomic_int_load_store},
    {"int_add_sub", test_atomic_int_add_sub},
    {"int_inc_dec", test_atomic_int_inc_dec},
    {"int_exchange", test_atomic_int_exchange},
    {"int_compare_exchange", test_atomic_int_compare_exchange},
    {"ptr", test_atomic_ptr},
};

test_suite_t test_atomic_suite = {
    "vox_atomic",
    test_atomic_cases,
    sizeof(test_atomic_cases) / sizeof(test_atomic_cases[0])
};
