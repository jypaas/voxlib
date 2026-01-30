/* ============================================================
 * test_string.c - vox_string 模块测试
 * ============================================================ */

#include "test_runner.h"
#include "../vox_string.h"
#include <string.h>

/* 测试创建和销毁 */
static void test_string_create_destroy(vox_mpool_t* mpool) {
    vox_string_t* str = vox_string_create(mpool);
    TEST_ASSERT_NOT_NULL(str, "创建string失败");
    TEST_ASSERT_EQ(vox_string_length(str), 0, "新string长度应为0");
    TEST_ASSERT_EQ(vox_string_empty(str), 1, "新string应为空");
    vox_string_destroy(str);
}

/* 测试从C字符串创建 */
static void test_string_from_cstr(vox_mpool_t* mpool) {
    vox_string_t* str = vox_string_from_cstr(mpool, "Hello");
    TEST_ASSERT_NOT_NULL(str, "从C字符串创建失败");
    TEST_ASSERT_EQ(vox_string_length(str), 5, "字符串长度不正确");
    
    const char* cstr = vox_string_cstr(str);
    TEST_ASSERT_NOT_NULL(cstr, "获取C字符串失败");
    TEST_ASSERT_STR_EQ(cstr, "Hello", "字符串内容不正确");
    
    vox_string_destroy(str);
}

/* 测试set和get */
static void test_string_set_get(vox_mpool_t* mpool) {
    vox_string_t* str = vox_string_create(mpool);
    TEST_ASSERT_NOT_NULL(str, "创建string失败");
    
    TEST_ASSERT_EQ(vox_string_set(str, "World"), 0, "set失败");
    TEST_ASSERT_EQ(vox_string_length(str), 5, "字符串长度不正确");
    
    const char* cstr = vox_string_cstr(str);
    TEST_ASSERT_STR_EQ(cstr, "World", "字符串内容不正确");
    
    vox_string_destroy(str);
}

/* 测试append */
static void test_string_append(vox_mpool_t* mpool) {
    vox_string_t* str = vox_string_from_cstr(mpool, "Hello");
    TEST_ASSERT_NOT_NULL(str, "创建string失败");
    
    TEST_ASSERT_EQ(vox_string_append(str, " World"), 0, "append失败");
    TEST_ASSERT_EQ(vox_string_length(str), 11, "字符串长度不正确");
    
    const char* cstr = vox_string_cstr(str);
    TEST_ASSERT_STR_EQ(cstr, "Hello World", "字符串内容不正确");
    
    vox_string_destroy(str);
}

/* 测试find */
static void test_string_find(vox_mpool_t* mpool) {
    vox_string_t* str = vox_string_from_cstr(mpool, "Hello World");
    TEST_ASSERT_NOT_NULL(str, "创建string失败");
    
    size_t pos = vox_string_find(str, "World", 0);
    TEST_ASSERT_EQ(pos, 6, "find位置不正确");
    
    pos = vox_string_find(str, "Not Found", 0);
    TEST_ASSERT_EQ(pos, SIZE_MAX, "未找到应返回SIZE_MAX");
    
    vox_string_destroy(str);
}

/* 测试replace */
static void test_string_replace(vox_mpool_t* mpool) {
    vox_string_t* str = vox_string_from_cstr(mpool, "Hello World");
    TEST_ASSERT_NOT_NULL(str, "创建string失败");
    
    int count = vox_string_replace(str, "World", "Vox");
    TEST_ASSERT_EQ(count, 1, "replace次数不正确");
    
    const char* cstr = vox_string_cstr(str);
    TEST_ASSERT_STR_EQ(cstr, "Hello Vox", "replace后内容不正确");
    
    vox_string_destroy(str);
}

/* 测试substr */
static void test_string_substr(vox_mpool_t* mpool) {
    vox_string_t* str = vox_string_from_cstr(mpool, "Hello World");
    TEST_ASSERT_NOT_NULL(str, "创建string失败");
    
    vox_string_t* substr = vox_string_substr(mpool, str, 0, 5);
    TEST_ASSERT_NOT_NULL(substr, "substr失败");
    
    const char* cstr = vox_string_cstr(substr);
    TEST_ASSERT_STR_EQ(cstr, "Hello", "substr内容不正确");
    
    vox_string_destroy(substr);
    vox_string_destroy(str);
}

/* 测试Unicode和特殊字符 */
static void test_string_unicode_special(vox_mpool_t* mpool) {
    /* 测试UTF-8字符串 */
    vox_string_t* str = vox_string_from_cstr(mpool, "你好世界");
    TEST_ASSERT_NOT_NULL(str, "创建Unicode字符串失败");
    TEST_ASSERT_GT(vox_string_length(str), 0, "Unicode字符串长度应为正数");
    
    /* 测试特殊字符 */
    vox_string_t* str2 = vox_string_from_cstr(mpool, "Hello\nWorld\tTest\r\n");
    TEST_ASSERT_NOT_NULL(str2, "创建特殊字符字符串失败");
    TEST_ASSERT_GT(vox_string_length(str2), 0, "特殊字符字符串长度应为正数");
    
    vox_string_destroy(str);
    vox_string_destroy(str2);
}

/* 测试长字符串 */
static void test_string_long(vox_mpool_t* mpool) {
    /* 创建一个较长的字符串 */
    char long_str[1000];
    for (int i = 0; i < 999; i++) {
        long_str[i] = 'A' + (i % 26);
    }
    long_str[999] = '\0';
    
    vox_string_t* str = vox_string_from_cstr(mpool, long_str);
    TEST_ASSERT_NOT_NULL(str, "创建长字符串失败");
    TEST_ASSERT_EQ(vox_string_length(str), 999, "长字符串长度不正确");
    
    const char* cstr = vox_string_cstr(str);
    TEST_ASSERT_EQ(memcmp(cstr, long_str, 999), 0, "长字符串内容不正确");
    
    vox_string_destroy(str);
}

/* 测试多次replace */
static void test_string_replace_multiple(vox_mpool_t* mpool) {
    vox_string_t* str = vox_string_from_cstr(mpool, "foo bar foo baz foo");
    TEST_ASSERT_NOT_NULL(str, "创建string失败");
    
    int count = vox_string_replace(str, "foo", "test");
    TEST_ASSERT_EQ(count, 3, "replace次数不正确");
    
    const char* cstr = vox_string_cstr(str);
    TEST_ASSERT_STR_EQ(cstr, "test bar test baz test", "多次replace后内容不正确");
    
    vox_string_destroy(str);
}

/* 测试边界情况 */
static void test_string_edge_cases(vox_mpool_t* mpool) {
    /* 测试空字符串 */
    vox_string_t* str1 = vox_string_from_cstr(mpool, "");
    TEST_ASSERT_NOT_NULL(str1, "创建空字符串失败");
    TEST_ASSERT_EQ(vox_string_length(str1), 0, "空字符串长度应为0");
    TEST_ASSERT_EQ(vox_string_empty(str1), 1, "空字符串应为空");
    
    /* 测试单个字符 */
    vox_string_t* str2 = vox_string_from_cstr(mpool, "A");
    TEST_ASSERT_NOT_NULL(str2, "创建单字符字符串失败");
    TEST_ASSERT_EQ(vox_string_length(str2), 1, "单字符字符串长度应为1");
    
    /* 测试find边界 */
    size_t pos = vox_string_find(str2, "A", 0);
    TEST_ASSERT_EQ(pos, 0, "在开头查找应返回0");
    
    pos = vox_string_find(str2, "A", 1);
    TEST_ASSERT_EQ(pos, SIZE_MAX, "超出范围查找应返回SIZE_MAX");
    
    /* 测试substr边界 */
    vox_string_t* substr = vox_string_substr(mpool, str2, 0, 0);
    TEST_ASSERT_NOT_NULL(substr, "substr(0,0)不应为NULL");
    TEST_ASSERT_EQ(vox_string_length(substr), 0, "substr(0,0)长度应为0");
    
    vox_string_destroy(str1);
    vox_string_destroy(str2);
    vox_string_destroy(substr);
}

/* 测试套件 */
test_case_t test_string_cases[] = {
    {"create_destroy", test_string_create_destroy},
    {"from_cstr", test_string_from_cstr},
    {"set_get", test_string_set_get},
    {"append", test_string_append},
    {"find", test_string_find},
    {"replace", test_string_replace},
    {"substr", test_string_substr},
    {"unicode_special", test_string_unicode_special},
    {"long", test_string_long},
    {"replace_multiple", test_string_replace_multiple},
    {"edge_cases", test_string_edge_cases},
};

test_suite_t test_string_suite = {
    "vox_string",
    test_string_cases,
    sizeof(test_string_cases) / sizeof(test_string_cases[0])
};
