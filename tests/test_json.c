/* ============================================================
 * test_json.c - vox_json 模块测试
 * ============================================================ */

#include "test_runner.h"
#include "../vox_json.h"
#include <string.h>

/* 测试解析简单值 */
static void test_json_parse_simple(vox_mpool_t* mpool) {
    /* 测试null */
    char* json1 = (char*)vox_mpool_alloc(mpool, 100);
    strcpy(json1, "null");
    size_t size1 = strlen(json1);
    vox_json_elem_t* elem1 = vox_json_parse(mpool, json1, &size1, NULL);
    TEST_ASSERT_NOT_NULL(elem1, "解析null失败");
    TEST_ASSERT_EQ(vox_json_get_type(elem1), VOX_JSON_NULL, "类型应为NULL");
    
    /* 测试boolean */
    char* json2 = (char*)vox_mpool_alloc(mpool, 100);
    strcpy(json2, "true");
    size_t size2 = strlen(json2);
    vox_json_elem_t* elem2 = vox_json_parse(mpool, json2, &size2, NULL);
    TEST_ASSERT_NOT_NULL(elem2, "解析boolean失败");
    TEST_ASSERT_EQ(vox_json_get_type(elem2), VOX_JSON_BOOLEAN, "类型应为BOOLEAN");
    TEST_ASSERT_EQ(vox_json_get_bool(elem2), 1, "布尔值应为true");
    
    /* 测试number */
    char* json3 = (char*)vox_mpool_alloc(mpool, 100);
    strcpy(json3, "42.5");
    size_t size3 = strlen(json3);
    vox_json_elem_t* elem3 = vox_json_parse(mpool, json3, &size3, NULL);
    TEST_ASSERT_NOT_NULL(elem3, "解析number失败");
    TEST_ASSERT_EQ(vox_json_get_type(elem3), VOX_JSON_NUMBER, "类型应为NUMBER");
    TEST_ASSERT_EQ(vox_json_get_number(elem3), 42.5, "数字值不正确");
    
    /* 测试string */
    char* json4 = (char*)vox_mpool_alloc(mpool, 100);
    strcpy(json4, "\"hello\"");
    size_t size4 = strlen(json4);
    vox_json_elem_t* elem4 = vox_json_parse(mpool, json4, &size4, NULL);
    TEST_ASSERT_NOT_NULL(elem4, "解析string失败");
    TEST_ASSERT_EQ(vox_json_get_type(elem4), VOX_JSON_STRING, "类型应为STRING");
    vox_strview_t str = vox_json_get_string(elem4);
    TEST_ASSERT_EQ(str.len, 5, "字符串长度不正确");
    TEST_ASSERT_EQ(memcmp(str.ptr, "hello", 5), 0, "字符串内容不正确");
}

/* 测试解析数组 */
static void test_json_parse_array(vox_mpool_t* mpool) {
    char* json = (char*)vox_mpool_alloc(mpool, 200);
    strcpy(json, "[1, 2, 3, \"hello\", true]");
    size_t size = strlen(json);
    
    vox_json_elem_t* elem = vox_json_parse(mpool, json, &size, NULL);
    TEST_ASSERT_NOT_NULL(elem, "解析数组失败");
    TEST_ASSERT_EQ(vox_json_get_type(elem), VOX_JSON_ARRAY, "类型应为ARRAY");
    
    size_t count = vox_json_get_array_count(elem);
    TEST_ASSERT_EQ(count, 5, "数组元素数量不正确");
    
    vox_json_elem_t* elem0 = vox_json_get_array_elem(elem, 0);
    TEST_ASSERT_NOT_NULL(elem0, "获取数组元素失败");
    TEST_ASSERT_EQ(vox_json_get_number(elem0), 1.0, "数组元素值不正确");
    
    vox_json_elem_t* elem3 = vox_json_get_array_elem(elem, 3);
    TEST_ASSERT_NOT_NULL(elem3, "获取数组元素失败");
    vox_strview_t str = vox_json_get_string(elem3);
    TEST_ASSERT_EQ(memcmp(str.ptr, "hello", 5), 0, "数组字符串元素不正确");
}

/* 测试解析对象 */
static void test_json_parse_object(vox_mpool_t* mpool) {
    char* json = (char*)vox_mpool_alloc(mpool, 200);
    strcpy(json, "{\"name\":\"test\",\"age\":30,\"active\":true}");
    size_t size = strlen(json);
    
    vox_json_elem_t* elem = vox_json_parse(mpool, json, &size, NULL);
    TEST_ASSERT_NOT_NULL(elem, "解析对象失败");
    TEST_ASSERT_EQ(vox_json_get_type(elem), VOX_JSON_OBJECT, "类型应为OBJECT");
    
    size_t count = vox_json_get_object_count(elem);
    TEST_ASSERT_EQ(count, 3, "对象成员数量不正确");
    
    vox_json_elem_t* name_val = vox_json_get_object_value(elem, "name");
    TEST_ASSERT_NOT_NULL(name_val, "获取对象值失败");
    vox_strview_t name = vox_json_get_string(name_val);
    TEST_ASSERT_EQ(memcmp(name.ptr, "test", 4), 0, "对象值不正确");
    
    vox_json_elem_t* age_val = vox_json_get_object_value(elem, "age");
    TEST_ASSERT_NOT_NULL(age_val, "获取对象值失败");
    TEST_ASSERT_EQ(vox_json_get_number(age_val), 30.0, "对象值不正确");
    
    vox_json_elem_t* active_val = vox_json_get_object_value(elem, "active");
    TEST_ASSERT_NOT_NULL(active_val, "获取对象值失败");
    TEST_ASSERT_EQ(vox_json_get_bool(active_val), 1, "对象值不正确");
}

/* 测试解析嵌套结构 */
static void test_json_parse_nested(vox_mpool_t* mpool) {
    char* json = (char*)vox_mpool_alloc(mpool, 300);
    strcpy(json, "{\"user\":{\"name\":\"Alice\",\"tags\":[\"admin\",\"user\"]}}");
    size_t size = strlen(json);
    
    vox_json_elem_t* elem = vox_json_parse(mpool, json, &size, NULL);
    TEST_ASSERT_NOT_NULL(elem, "解析嵌套对象失败");
    
    vox_json_elem_t* user = vox_json_get_object_value(elem, "user");
    TEST_ASSERT_NOT_NULL(user, "获取嵌套对象失败");
    TEST_ASSERT_EQ(vox_json_get_type(user), VOX_JSON_OBJECT, "嵌套对象类型不正确");
    
    vox_json_elem_t* name = vox_json_get_object_value(user, "name");
    TEST_ASSERT_NOT_NULL(name, "获取嵌套对象值失败");
    
    vox_json_elem_t* tags = vox_json_get_object_value(user, "tags");
    TEST_ASSERT_NOT_NULL(tags, "获取嵌套数组失败");
    TEST_ASSERT_EQ(vox_json_get_type(tags), VOX_JSON_ARRAY, "嵌套数组类型不正确");
    TEST_ASSERT_EQ(vox_json_get_array_count(tags), 2, "嵌套数组元素数量不正确");
}

/* 测试遍历数组 */
static void test_json_array_traverse(vox_mpool_t* mpool) {
    char* json = (char*)vox_mpool_alloc(mpool, 200);
    strcpy(json, "[10, 20, 30]");
    size_t size = strlen(json);
    
    vox_json_elem_t* elem = vox_json_parse(mpool, json, &size, NULL);
    TEST_ASSERT_NOT_NULL(elem, "解析数组失败");
    
    vox_json_elem_t* first = vox_json_array_first(elem);
    TEST_ASSERT_NOT_NULL(first, "获取第一个元素失败");
    TEST_ASSERT_EQ(vox_json_get_number(first), 10.0, "第一个元素值不正确");
    
    vox_json_elem_t* second = vox_json_array_next(first);
    TEST_ASSERT_NOT_NULL(second, "获取下一个元素失败");
    TEST_ASSERT_EQ(vox_json_get_number(second), 20.0, "第二个元素值不正确");
    
    vox_json_elem_t* third = vox_json_array_next(second);
    TEST_ASSERT_NOT_NULL(third, "获取下一个元素失败");
    TEST_ASSERT_EQ(vox_json_get_number(third), 30.0, "第三个元素值不正确");
    
    vox_json_elem_t* fourth = vox_json_array_next(third);
    TEST_ASSERT_NULL(fourth, "应没有更多元素");
}

/* 测试遍历对象 */
static void test_json_object_traverse(vox_mpool_t* mpool) {
    char* json = (char*)vox_mpool_alloc(mpool, 200);
    strcpy(json, "{\"a\":1,\"b\":2,\"c\":3}");
    size_t size = strlen(json);
    
    vox_json_elem_t* elem = vox_json_parse(mpool, json, &size, NULL);
    TEST_ASSERT_NOT_NULL(elem, "解析对象失败");
    
    vox_json_member_t* first = vox_json_object_first(elem);
    TEST_ASSERT_NOT_NULL(first, "获取第一个成员失败");
    
    int count = 0;
    vox_json_member_t* member = first;
    while (member) {
        count++;
        member = vox_json_object_next(member);
    }
    TEST_ASSERT_EQ(count, 3, "遍历成员数量不正确");
}

/* 测试类型检查 */
static void test_json_type_check(vox_mpool_t* mpool) {
    char* json = (char*)vox_mpool_alloc(mpool, 100);
    strcpy(json, "42");
    size_t size = strlen(json);
    
    vox_json_elem_t* elem = vox_json_parse(mpool, json, &size, NULL);
    TEST_ASSERT_NOT_NULL(elem, "解析失败");
    
    TEST_ASSERT_EQ(vox_json_is_type(elem, VOX_JSON_NUMBER), 1, "类型检查失败");
    TEST_ASSERT_EQ(vox_json_is_type(elem, VOX_JSON_STRING), 0, "类型检查失败");
}

/* 测试错误处理 */
static void test_json_error_handling(vox_mpool_t* mpool) {
    char* json = (char*)vox_mpool_alloc(mpool, 100);
    strcpy(json, "{invalid json}");
    size_t size = strlen(json);
    
    vox_json_err_info_t err_info;
    vox_json_elem_t* elem = vox_json_parse(mpool, json, &size, &err_info);
    TEST_ASSERT_NULL(elem, "解析无效JSON应失败");
    TEST_ASSERT_NE(err_info.message, NULL, "错误信息应为非空");
}

/* 测试科学计数法和特殊数字 */
static void test_json_scientific_notation(vox_mpool_t* mpool) {
    /* 测试科学计数法 */
    char* json1 = (char*)vox_mpool_alloc(mpool, 100);
    strcpy(json1, "1.23e-4");
    size_t size1 = strlen(json1);
    vox_json_elem_t* elem1 = vox_json_parse(mpool, json1, &size1, NULL);
    TEST_ASSERT_NOT_NULL(elem1, "解析科学计数法失败");
    TEST_ASSERT_EQ(vox_json_get_type(elem1), VOX_JSON_NUMBER, "类型应为NUMBER");
    
    /* 测试大数字（在 double 整数精度内，避免平台 ERANGE 差异） */
    char* json2 = (char*)vox_mpool_alloc(mpool, 100);
    strcpy(json2, "1234567890123456");
    size_t size2 = strlen(json2);
    vox_json_elem_t* elem2 = vox_json_parse(mpool, json2, &size2, NULL);
    TEST_ASSERT_NOT_NULL(elem2, "解析大数字失败");
    TEST_ASSERT_EQ(vox_json_get_type(elem2), VOX_JSON_NUMBER, "类型应为NUMBER");
    
    /* 测试负数 */
    char* json3 = (char*)vox_mpool_alloc(mpool, 100);
    strcpy(json3, "-42.5");
    size_t size3 = strlen(json3);
    vox_json_elem_t* elem3 = vox_json_parse(mpool, json3, &size3, NULL);
    TEST_ASSERT_NOT_NULL(elem3, "解析负数失败");
    TEST_ASSERT_EQ(vox_json_get_type(elem3), VOX_JSON_NUMBER, "类型应为NUMBER");
    TEST_ASSERT_EQ(vox_json_get_number(elem3), -42.5, "负数值不正确");
}

/* 测试转义字符 */
static void test_json_escape_chars(vox_mpool_t* mpool) {
    char* json = (char*)vox_mpool_alloc(mpool, 200);
    strcpy(json, "\"Hello\\nWorld\\tTest\\\"Quote\\\"\"");
    size_t size = strlen(json);
    
    vox_json_elem_t* elem = vox_json_parse(mpool, json, &size, NULL);
    TEST_ASSERT_NOT_NULL(elem, "解析转义字符失败");
    TEST_ASSERT_EQ(vox_json_get_type(elem), VOX_JSON_STRING, "类型应为STRING");
    
    vox_strview_t str = vox_json_get_string(elem);
    TEST_ASSERT_GT(str.len, 0, "转义字符串长度应为正数");
}

/* 测试Unicode字符 */
static void test_json_unicode(vox_mpool_t* mpool) {
    char* json = (char*)vox_mpool_alloc(mpool, 200);
    strcpy(json, "\"\\u4e2d\\u6587\"");
    size_t size = strlen(json);
    
    vox_json_elem_t* elem = vox_json_parse(mpool, json, &size, NULL);
    TEST_ASSERT_NOT_NULL(elem, "解析Unicode字符失败");
    TEST_ASSERT_EQ(vox_json_get_type(elem), VOX_JSON_STRING, "类型应为STRING");
    
    vox_strview_t str = vox_json_get_string(elem);
    TEST_ASSERT_GT(str.len, 0, "Unicode字符串长度应为正数");
}

/* 测试空数组和空对象 */
static void test_json_empty_structures(vox_mpool_t* mpool) {
    /* 测试空数组 */
    char* json1 = (char*)vox_mpool_alloc(mpool, 100);
    strcpy(json1, "[]");
    size_t size1 = strlen(json1);
    vox_json_elem_t* elem1 = vox_json_parse(mpool, json1, &size1, NULL);
    TEST_ASSERT_NOT_NULL(elem1, "解析空数组失败");
    TEST_ASSERT_EQ(vox_json_get_type(elem1), VOX_JSON_ARRAY, "类型应为ARRAY");
    TEST_ASSERT_EQ(vox_json_get_array_count(elem1), 0, "空数组元素数量应为0");
    
    /* 测试空对象 */
    char* json2 = (char*)vox_mpool_alloc(mpool, 100);
    strcpy(json2, "{}");
    size_t size2 = strlen(json2);
    vox_json_elem_t* elem2 = vox_json_parse(mpool, json2, &size2, NULL);
    TEST_ASSERT_NOT_NULL(elem2, "解析空对象失败");
    TEST_ASSERT_EQ(vox_json_get_type(elem2), VOX_JSON_OBJECT, "类型应为OBJECT");
    TEST_ASSERT_EQ(vox_json_get_object_count(elem2), 0, "空对象成员数量应为0");
}

/* 测试复杂嵌套结构 */
static void test_json_complex_nested(vox_mpool_t* mpool) {
    char* json = (char*)vox_mpool_alloc(mpool, 500);
    strcpy(json, "{\"users\":[{\"id\":1,\"name\":\"Alice\",\"tags\":[\"admin\",\"user\"]},{\"id\":2,\"name\":\"Bob\",\"tags\":[]}],\"meta\":{\"count\":2,\"page\":1}}");
    size_t size = strlen(json);
    
    vox_json_elem_t* elem = vox_json_parse(mpool, json, &size, NULL);
    TEST_ASSERT_NOT_NULL(elem, "解析复杂嵌套结构失败");
    TEST_ASSERT_EQ(vox_json_get_type(elem), VOX_JSON_OBJECT, "类型应为OBJECT");
    
    vox_json_elem_t* users = vox_json_get_object_value(elem, "users");
    TEST_ASSERT_NOT_NULL(users, "获取users数组失败");
    TEST_ASSERT_EQ(vox_json_get_type(users), VOX_JSON_ARRAY, "users应为数组");
    TEST_ASSERT_EQ(vox_json_get_array_count(users), 2, "users数组应包含2个元素");
    
    vox_json_elem_t* user1 = vox_json_get_array_elem(users, 0);
    TEST_ASSERT_NOT_NULL(user1, "获取第一个用户失败");
    vox_json_elem_t* name1 = vox_json_get_object_value(user1, "name");
    TEST_ASSERT_NOT_NULL(name1, "获取用户名失败");
}

/* 测试边界值 */
static void test_json_boundary_values(vox_mpool_t* mpool) {
    /* 测试零 */
    char* json1 = (char*)vox_mpool_alloc(mpool, 100);
    strcpy(json1, "0");
    size_t size1 = strlen(json1);
    vox_json_elem_t* elem1 = vox_json_parse(mpool, json1, &size1, NULL);
    TEST_ASSERT_NOT_NULL(elem1, "解析0失败");
    TEST_ASSERT_EQ(vox_json_get_number(elem1), 0.0, "0值不正确");
    
    /* 测试false */
    char* json2 = (char*)vox_mpool_alloc(mpool, 100);
    strcpy(json2, "false");
    size_t size2 = strlen(json2);
    vox_json_elem_t* elem2 = vox_json_parse(mpool, json2, &size2, NULL);
    TEST_ASSERT_NOT_NULL(elem2, "解析false失败");
    TEST_ASSERT_EQ(vox_json_get_type(elem2), VOX_JSON_BOOLEAN, "类型应为BOOLEAN");
    TEST_ASSERT_EQ(vox_json_get_bool(elem2), 0, "布尔值应为false");
    
    /* 测试空字符串 */
    char* json3 = (char*)vox_mpool_alloc(mpool, 100);
    strcpy(json3, "\"\"");
    size_t size3 = strlen(json3);
    vox_json_elem_t* elem3 = vox_json_parse(mpool, json3, &size3, NULL);
    TEST_ASSERT_NOT_NULL(elem3, "解析空字符串失败");
    TEST_ASSERT_EQ(vox_json_get_type(elem3), VOX_JSON_STRING, "类型应为STRING");
    vox_strview_t str = vox_json_get_string(elem3);
    TEST_ASSERT_EQ(str.len, 0, "空字符串长度应为0");
}

/* 测试序列化 */
static void test_json_serialize(vox_mpool_t* mpool) {
    char* json = (char*)vox_mpool_alloc(mpool, 256);
    strcpy(json, "{\"a\":1,\"b\":[2,3],\"c\":\"hi\"}");
    size_t size = strlen(json);
    vox_json_elem_t* elem = vox_json_parse(mpool, json, &size, NULL);
    TEST_ASSERT_NOT_NULL(elem, "解析失败");
    
    /* 使用 vox_json_to_string（推荐） */
    vox_string_t* str = vox_json_to_string(mpool, elem, false);
    TEST_ASSERT_NOT_NULL(str, "vox_json_to_string 应成功");
    const char* cstr = vox_string_cstr(str);
    TEST_ASSERT_NOT_NULL(cstr, "cstr 非空");
    TEST_ASSERT_TRUE(strchr(cstr, 'a') != NULL, "输出应含键a");
    TEST_ASSERT_TRUE(strchr(cstr, '1') != NULL, "输出应含值1");
    
    vox_string_t* str_pretty = vox_json_to_string(mpool, elem, true);
    TEST_ASSERT_NOT_NULL(str_pretty, "vox_json_to_string pretty 应成功");
    TEST_ASSERT_TRUE(strchr(vox_string_cstr(str_pretty), '\n') != NULL, "pretty 应含换行");
    
    /* 兼容：固定缓冲区序列化 */
    size_t written = 0;
    int ret = vox_json_serialize(elem, NULL, 0, &written, false);
    TEST_ASSERT_EQ(ret, 0, "计算长度应成功");
    TEST_ASSERT_TRUE(written > 0, "长度应大于0");
    char* buf = (char*)vox_mpool_alloc(mpool, written + 1);
    ret = vox_json_serialize(elem, buf, written + 1, &written, false);
    TEST_ASSERT_EQ(ret, 0, "序列化应成功");
}

/* 测试构建 API */
static void test_json_builder(vox_mpool_t* mpool) {
    vox_json_elem_t* root = vox_json_new_object(mpool);
    TEST_ASSERT_NOT_NULL(root, "new_object 失败");
    TEST_ASSERT_EQ(vox_json_get_type(root), VOX_JSON_OBJECT, "类型应为 OBJECT");
    
    vox_json_elem_t* n = vox_json_new_number(mpool, 42);
    TEST_ASSERT_EQ(vox_json_object_set(mpool, root, "num", n), 0, "object_set 失败");
    vox_json_elem_t* s = vox_json_new_string_cstr(mpool, "hello");
    TEST_ASSERT_EQ(vox_json_object_set(mpool, root, "str", s), 0, "object_set str 失败");
    vox_json_elem_t* arr = vox_json_new_array(mpool);
    vox_json_array_append(arr, vox_json_new_number(mpool, 1));
    vox_json_array_append(arr, vox_json_new_number(mpool, 2));
    TEST_ASSERT_EQ(vox_json_object_set(mpool, root, "arr", arr), 0, "object_set arr 失败");
    
    TEST_ASSERT_EQ(vox_json_get_object_count(root), 3, "应有3个成员");
    vox_json_elem_t* num_val = vox_json_get_object_value(root, "num");
    TEST_ASSERT_NOT_NULL(num_val, "应能取到 num");
    TEST_ASSERT_EQ(vox_json_get_int(num_val), 42, "num 应为 42");
    vox_json_elem_t* str_val = vox_json_get_object_value(root, "str");
    vox_strview_t str_sv = vox_json_get_string(str_val);
    TEST_ASSERT_TRUE(vox_strview_compare_cstr(&str_sv, "hello") == 0, "str 应为 hello");
    vox_json_elem_t* arr_val = vox_json_get_object_value(root, "arr");
    TEST_ASSERT_EQ(vox_json_get_array_count(arr_val), 2, "arr 应有2个元素");
    
    vox_string_t* str = vox_json_to_string(mpool, root, false);
    TEST_ASSERT_NOT_NULL(str, "vox_json_to_string 应成功");
    const char* buf = vox_string_cstr(str);
    TEST_ASSERT_TRUE(strstr(buf, "\"num\":42") != NULL || strstr(buf, "\"num\": 42") != NULL, "序列化应含 num");
    TEST_ASSERT_TRUE(strstr(buf, "hello") != NULL, "序列化应含 hello");
    
    TEST_ASSERT_EQ(vox_json_object_remove(mpool, root, "str"), 0, "remove 应成功");
    TEST_ASSERT_EQ(vox_json_get_object_count(root), 2, "移除后应有2个成员");
    TEST_ASSERT_NULL(vox_json_get_object_value(root, "str"), "str 应已被移除");
}

/* 测试数字与类型严格检查：前导零拒绝、number_is_integer、get_int 越界 */
static void test_json_strict_number(vox_mpool_t* mpool) {
    /* 前导零应解析失败 */
    char* json_lead = (char*)vox_mpool_alloc(mpool, 32);
    strcpy(json_lead, "01");
    size_t sz = strlen(json_lead);
    vox_json_err_info_t err_info;
    vox_json_elem_t* e = vox_json_parse(mpool, json_lead, &sz, &err_info);
    TEST_ASSERT_NULL(e, "前导零 01 应解析失败");
    TEST_ASSERT_TRUE(err_info.message != NULL && strstr(err_info.message, "Leading") != NULL, "应报前导零错误");

    /* 合法整数：number_is_integer 为 true，get_int 正确 */
    char* json_ok = (char*)vox_mpool_alloc(mpool, 32);
    strcpy(json_ok, "42");
    sz = strlen(json_ok);
    e = vox_json_parse(mpool, json_ok, &sz, NULL);
    TEST_ASSERT_NOT_NULL(e, "解析 42 应成功");
    TEST_ASSERT_TRUE(vox_json_number_is_integer(e), "42 应为整数");
    TEST_ASSERT_EQ(vox_json_get_int(e), 42, "get_int 42");

    /* 小数：number_is_integer 为 false，get_int 截断 */
    strcpy(json_ok, "3.14");
    sz = strlen(json_ok);
    e = vox_json_parse(mpool, json_ok, &sz, NULL);
    TEST_ASSERT_NOT_NULL(e, "解析 3.14 应成功");
    TEST_ASSERT_FALSE(vox_json_number_is_integer(e), "3.14 不应为整数");
    TEST_ASSERT_EQ(vox_json_get_int(e), 3, "get_int 截断为 3");

    /* 非 NUMBER 类型调用 get_int 返回 0 */
    vox_json_elem_t* s = vox_json_new_string_cstr(mpool, "x");
    TEST_ASSERT_EQ(vox_json_get_int((const vox_json_elem_t*)s), 0, "string 上 get_int 应返回 0");
}

/* 测试套件 */
test_case_t test_json_cases[] = {
    {"parse_simple", test_json_parse_simple},
    {"parse_array", test_json_parse_array},
    {"parse_object", test_json_parse_object},
    {"parse_nested", test_json_parse_nested},
    {"array_traverse", test_json_array_traverse},
    {"object_traverse", test_json_object_traverse},
    {"type_check", test_json_type_check},
    {"error_handling", test_json_error_handling},
    {"scientific_notation", test_json_scientific_notation},
    {"escape_chars", test_json_escape_chars},
    {"unicode", test_json_unicode},
    {"empty_structures", test_json_empty_structures},
    {"complex_nested", test_json_complex_nested},
    {"boundary_values", test_json_boundary_values},
    {"serialize", test_json_serialize},
    {"builder", test_json_builder},
    {"strict_number", test_json_strict_number},
};

test_suite_t test_json_suite = {
    "vox_json",
    test_json_cases,
    sizeof(test_json_cases) / sizeof(test_json_cases[0])
};
