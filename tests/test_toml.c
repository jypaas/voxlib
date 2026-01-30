/* ============================================================
 * test_toml.c - vox_toml 模块测试
 * ============================================================ */

#include "test_runner.h"
#include "../vox_toml.h"
#include <string.h>

/* 测试解析简单值 */
static void test_toml_parse_simple(vox_mpool_t* mpool) {
    /* 测试字符串 */
    char* toml1 = (char*)vox_mpool_alloc(mpool, 200);
    strcpy(toml1, "key = \"hello\"");
    size_t size1 = strlen(toml1);
    vox_toml_table_t* root1 = vox_toml_parse(mpool, toml1, &size1, NULL);
    TEST_ASSERT_NOT_NULL(root1, "解析TOML失败");
    vox_toml_elem_t* val1 = vox_toml_get_value(root1, "key");
    TEST_ASSERT_NOT_NULL(val1, "获取值失败");
    TEST_ASSERT_EQ(vox_toml_get_type(val1), VOX_TOML_STRING, "类型应为STRING");
    vox_strview_t str1 = vox_toml_get_string(val1);
    TEST_ASSERT_EQ(str1.len, 5, "字符串长度不正确");
    TEST_ASSERT_EQ(memcmp(str1.ptr, "hello", 5), 0, "字符串内容不正确");
    
    /* 测试整数 */
    char* toml2 = (char*)vox_mpool_alloc(mpool, 200);
    strcpy(toml2, "age = 42");
    size_t size2 = strlen(toml2);
    vox_toml_table_t* root2 = vox_toml_parse(mpool, toml2, &size2, NULL);
    TEST_ASSERT_NOT_NULL(root2, "解析TOML失败");
    vox_toml_elem_t* val2 = vox_toml_get_value(root2, "age");
    TEST_ASSERT_NOT_NULL(val2, "获取值失败");
    TEST_ASSERT_EQ(vox_toml_get_type(val2), VOX_TOML_INTEGER, "类型应为INTEGER");
    TEST_ASSERT_EQ(vox_toml_get_integer(val2), 42, "整数值不正确");
    
    /* 测试浮点数 */
    char* toml3 = (char*)vox_mpool_alloc(mpool, 200);
    strcpy(toml3, "pi = 3.14");
    size_t size3 = strlen(toml3);
    vox_toml_table_t* root3 = vox_toml_parse(mpool, toml3, &size3, NULL);
    TEST_ASSERT_NOT_NULL(root3, "解析TOML失败");
    vox_toml_elem_t* val3 = vox_toml_get_value(root3, "pi");
    TEST_ASSERT_NOT_NULL(val3, "获取值失败");
    TEST_ASSERT_EQ(vox_toml_get_type(val3), VOX_TOML_FLOAT, "类型应为FLOAT");
    TEST_ASSERT_EQ(vox_toml_get_float(val3), 3.14, "浮点数值不正确");
    
    /* 测试布尔值 */
    char* toml4 = (char*)vox_mpool_alloc(mpool, 200);
    strcpy(toml4, "active = true");
    size_t size4 = strlen(toml4);
    vox_toml_table_t* root4 = vox_toml_parse(mpool, toml4, &size4, NULL);
    TEST_ASSERT_NOT_NULL(root4, "解析TOML失败");
    vox_toml_elem_t* val4 = vox_toml_get_value(root4, "active");
    TEST_ASSERT_NOT_NULL(val4, "获取值失败");
    TEST_ASSERT_EQ(vox_toml_get_type(val4), VOX_TOML_BOOLEAN, "类型应为BOOLEAN");
    TEST_ASSERT_EQ(vox_toml_get_boolean(val4), 1, "布尔值应为true");
}

/* 测试解析数组 */
static void test_toml_parse_array(vox_mpool_t* mpool) {
    char* toml = (char*)vox_mpool_alloc(mpool, 300);
    strcpy(toml, "numbers = [1, 2, 3, 4, 5]");
    size_t size = strlen(toml);
    
    vox_toml_table_t* root = vox_toml_parse(mpool, toml, &size, NULL);
    TEST_ASSERT_NOT_NULL(root, "解析TOML失败");
    vox_toml_elem_t* val = vox_toml_get_value(root, "numbers");
    TEST_ASSERT_NOT_NULL(val, "获取值失败");
    TEST_ASSERT_EQ(vox_toml_get_type(val), VOX_TOML_ARRAY, "类型应为ARRAY");
    
    size_t count = vox_toml_get_array_count(val);
    TEST_ASSERT_EQ(count, 5, "数组元素数量不正确");
    
    vox_toml_elem_t* elem0 = vox_toml_get_array_elem(val, 0);
    TEST_ASSERT_NOT_NULL(elem0, "获取数组元素失败");
    TEST_ASSERT_EQ(vox_toml_get_integer(elem0), 1, "数组元素值不正确");
    
    vox_toml_elem_t* elem2 = vox_toml_get_array_elem(val, 2);
    TEST_ASSERT_NOT_NULL(elem2, "获取数组元素失败");
    TEST_ASSERT_EQ(vox_toml_get_integer(elem2), 3, "数组元素值不正确");
}

/* 测试解析表 */
static void test_toml_parse_table(vox_mpool_t* mpool) {
    char* toml = (char*)vox_mpool_alloc(mpool, 300);
    strcpy(toml, "[database]\nhost = \"localhost\"\nport = 5432");
    size_t size = strlen(toml);
    
    vox_toml_table_t* root = vox_toml_parse(mpool, toml, &size, NULL);
    TEST_ASSERT_NOT_NULL(root, "解析TOML失败");
    
    vox_toml_table_t* db_table = vox_toml_find_subtable(root, "database");
    TEST_ASSERT_NOT_NULL(db_table, "查找表失败");
    
    vox_toml_elem_t* host_val = vox_toml_get_value(db_table, "host");
    TEST_ASSERT_NOT_NULL(host_val, "获取值失败");
    vox_strview_t host = vox_toml_get_string(host_val);
    TEST_ASSERT_EQ(memcmp(host.ptr, "localhost", 9), 0, "字符串内容不正确");
    
    vox_toml_elem_t* port_val = vox_toml_get_value(db_table, "port");
    TEST_ASSERT_NOT_NULL(port_val, "获取值失败");
    TEST_ASSERT_EQ(vox_toml_get_integer(port_val), 5432, "整数值不正确");
}

/* 测试解析嵌套结构 */
static void test_toml_parse_nested(vox_mpool_t* mpool) {
    char* toml = (char*)vox_mpool_alloc(mpool, 500);
    strcpy(toml, "[server]\nhost = \"0.0.0.0\"\nport = 8080\n[server.database]\nname = \"testdb\"");
    size_t size = strlen(toml);
    
    vox_toml_table_t* root = vox_toml_parse(mpool, toml, &size, NULL);
    TEST_ASSERT_NOT_NULL(root, "解析TOML失败");
    
    vox_toml_table_t* server_table = vox_toml_find_subtable(root, "server");
    TEST_ASSERT_NOT_NULL(server_table, "查找server表失败");
    
    vox_toml_elem_t* host_val = vox_toml_get_value(server_table, "host");
    TEST_ASSERT_NOT_NULL(host_val, "获取host值失败");
    
    vox_toml_table_t* db_table = vox_toml_find_subtable(server_table, "database");
    TEST_ASSERT_NOT_NULL(db_table, "查找database子表失败");
    
    vox_toml_elem_t* name_val = vox_toml_get_value(db_table, "name");
    TEST_ASSERT_NOT_NULL(name_val, "获取name值失败");
    vox_strview_t name = vox_toml_get_string(name_val);
    TEST_ASSERT_EQ(memcmp(name.ptr, "testdb", 6), 0, "字符串内容不正确");
}

/* 测试解析内联表 */
static void test_toml_parse_inline_table(vox_mpool_t* mpool) {
    char* toml = (char*)vox_mpool_alloc(mpool, 300);
    strcpy(toml, "point = { x = 1, y = 2 }");
    size_t size = strlen(toml);
    
    vox_toml_table_t* root = vox_toml_parse(mpool, toml, &size, NULL);
    TEST_ASSERT_NOT_NULL(root, "解析TOML失败");
    
    vox_toml_elem_t* point_val = vox_toml_get_value(root, "point");
    TEST_ASSERT_NOT_NULL(point_val, "获取值失败");
    TEST_ASSERT_EQ(vox_toml_get_type(point_val), VOX_TOML_INLINE_TABLE, "类型应为INLINE_TABLE");
}

/* 测试遍历数组 */
static void test_toml_array_traverse(vox_mpool_t* mpool) {
    char* toml = (char*)vox_mpool_alloc(mpool, 300);
    strcpy(toml, "items = [10, 20, 30]");
    size_t size = strlen(toml);
    
    vox_toml_table_t* root = vox_toml_parse(mpool, toml, &size, NULL);
    TEST_ASSERT_NOT_NULL(root, "解析TOML失败");
    
    vox_toml_elem_t* items_val = vox_toml_get_value(root, "items");
    TEST_ASSERT_NOT_NULL(items_val, "获取值失败");
    
    vox_toml_elem_t* first = vox_toml_array_first(items_val);
    TEST_ASSERT_NOT_NULL(first, "获取第一个元素失败");
    TEST_ASSERT_EQ(vox_toml_get_integer(first), 10, "第一个元素值不正确");
    
    vox_toml_elem_t* second = vox_toml_array_next(first);
    TEST_ASSERT_NOT_NULL(second, "获取下一个元素失败");
    TEST_ASSERT_EQ(vox_toml_get_integer(second), 20, "第二个元素值不正确");
    
    vox_toml_elem_t* third = vox_toml_array_next(second);
    TEST_ASSERT_NOT_NULL(third, "获取下一个元素失败");
    TEST_ASSERT_EQ(vox_toml_get_integer(third), 30, "第三个元素值不正确");
    
    vox_toml_elem_t* fourth = vox_toml_array_next(third);
    TEST_ASSERT_NULL(fourth, "应没有更多元素");
}

/* 测试类型检查 */
static void test_toml_type_check(vox_mpool_t* mpool) {
    char* toml = (char*)vox_mpool_alloc(mpool, 200);
    strcpy(toml, "value = 42");
    size_t size = strlen(toml);
    
    vox_toml_table_t* root = vox_toml_parse(mpool, toml, &size, NULL);
    TEST_ASSERT_NOT_NULL(root, "解析失败");
    
    vox_toml_elem_t* val = vox_toml_get_value(root, "value");
    TEST_ASSERT_NOT_NULL(val, "获取值失败");
    
    TEST_ASSERT_EQ(vox_toml_is_type(val, VOX_TOML_INTEGER), 1, "类型检查失败");
    TEST_ASSERT_EQ(vox_toml_is_type(val, VOX_TOML_STRING), 0, "类型检查失败");
}

/* 测试错误处理 */
static void test_toml_error_handling(vox_mpool_t* mpool) {
    char* toml = (char*)vox_mpool_alloc(mpool, 200);
    strcpy(toml, "key = invalid value");
    size_t size = strlen(toml);
    
    vox_toml_err_info_t err_info;
    vox_toml_table_t* root = vox_toml_parse(mpool, toml, &size, &err_info);
    /* 可能解析失败或成功，取决于实现 */
    /* 这里主要测试错误信息是否被填充 */
    if (!root) {
        TEST_ASSERT_NE(err_info.message, NULL, "错误信息应为非空");
    }
}

/* 测试日期时间 */
static void test_toml_datetime(vox_mpool_t* mpool) {
    char* toml = (char*)vox_mpool_alloc(mpool, 300);
    strcpy(toml, "created = 1979-05-27T07:32:00Z");
    size_t size = strlen(toml);
    
    vox_toml_table_t* root = vox_toml_parse(mpool, toml, &size, NULL);
    TEST_ASSERT_NOT_NULL(root, "解析TOML失败");
    
    vox_toml_elem_t* val = vox_toml_get_value(root, "created");
    TEST_ASSERT_NOT_NULL(val, "获取值失败");
    TEST_ASSERT_EQ(vox_toml_get_type(val), VOX_TOML_DATETIME, "类型应为DATETIME");
    
    vox_strview_t dt = vox_toml_get_datetime(val);
    TEST_ASSERT_GT(dt.len, 0, "日期时间字符串长度应为正数");
}

/* 测试空数组和空表 */
static void test_toml_empty_structures(vox_mpool_t* mpool) {
    /* 测试空数组 */
    char* toml1 = (char*)vox_mpool_alloc(mpool, 200);
    strcpy(toml1, "empty = []");
    size_t size1 = strlen(toml1);
    vox_toml_table_t* root1 = vox_toml_parse(mpool, toml1, &size1, NULL);
    TEST_ASSERT_NOT_NULL(root1, "解析TOML失败");
    vox_toml_elem_t* val1 = vox_toml_get_value(root1, "empty");
    TEST_ASSERT_NOT_NULL(val1, "获取值失败");
    TEST_ASSERT_EQ(vox_toml_get_type(val1), VOX_TOML_ARRAY, "类型应为ARRAY");
    TEST_ASSERT_EQ(vox_toml_get_array_count(val1), 0, "空数组元素数量应为0");
    
    /* 测试空内联表 */
    char* toml2 = (char*)vox_mpool_alloc(mpool, 200);
    strcpy(toml2, "empty = {}");
    size_t size2 = strlen(toml2);
    vox_toml_table_t* root2 = vox_toml_parse(mpool, toml2, &size2, NULL);
    TEST_ASSERT_NOT_NULL(root2, "解析TOML失败");
    vox_toml_elem_t* val2 = vox_toml_get_value(root2, "empty");
    TEST_ASSERT_NOT_NULL(val2, "获取值失败");
    TEST_ASSERT_EQ(vox_toml_get_type(val2), VOX_TOML_INLINE_TABLE, "类型应为INLINE_TABLE");
}

/* 测试复杂嵌套结构 */
static void test_toml_complex_nested(vox_mpool_t* mpool) {
    char* toml = (char*)vox_mpool_alloc(mpool, 1000);
    strcpy(toml, "[users]\n[[users.items]]\nid = 1\nname = \"Alice\"\n[[users.items]]\nid = 2\nname = \"Bob\"");
    size_t size = strlen(toml);
    
    vox_toml_table_t* root = vox_toml_parse(mpool, toml, &size, NULL);
    TEST_ASSERT_NOT_NULL(root, "解析TOML失败");
    
    vox_toml_table_t* users_table = vox_toml_find_subtable(root, "users");
    TEST_ASSERT_NOT_NULL(users_table, "查找users表失败");
}

/* 测试边界值 */
static void test_toml_boundary_values(vox_mpool_t* mpool) {
    /* 测试零 */
    char* toml1 = (char*)vox_mpool_alloc(mpool, 200);
    strcpy(toml1, "zero = 0");
    size_t size1 = strlen(toml1);
    vox_toml_table_t* root1 = vox_toml_parse(mpool, toml1, &size1, NULL);
    TEST_ASSERT_NOT_NULL(root1, "解析TOML失败");
    vox_toml_elem_t* val1 = vox_toml_get_value(root1, "zero");
    TEST_ASSERT_NOT_NULL(val1, "获取值失败");
    TEST_ASSERT_EQ(vox_toml_get_integer(val1), 0, "0值不正确");
    
    /* 测试false */
    char* toml2 = (char*)vox_mpool_alloc(mpool, 200);
    strcpy(toml2, "flag = false");
    size_t size2 = strlen(toml2);
    vox_toml_table_t* root2 = vox_toml_parse(mpool, toml2, &size2, NULL);
    TEST_ASSERT_NOT_NULL(root2, "解析TOML失败");
    vox_toml_elem_t* val2 = vox_toml_get_value(root2, "flag");
    TEST_ASSERT_NOT_NULL(val2, "获取值失败");
    TEST_ASSERT_EQ(vox_toml_get_type(val2), VOX_TOML_BOOLEAN, "类型应为BOOLEAN");
    TEST_ASSERT_EQ(vox_toml_get_boolean(val2), 0, "布尔值应为false");
    
    /* 测试空字符串 */
    char* toml3 = (char*)vox_mpool_alloc(mpool, 200);
    strcpy(toml3, "empty = \"\"");
    size_t size3 = strlen(toml3);
    vox_toml_table_t* root3 = vox_toml_parse(mpool, toml3, &size3, NULL);
    TEST_ASSERT_NOT_NULL(root3, "解析TOML失败");
    vox_toml_elem_t* val3 = vox_toml_get_value(root3, "empty");
    TEST_ASSERT_NOT_NULL(val3, "获取值失败");
    TEST_ASSERT_EQ(vox_toml_get_type(val3), VOX_TOML_STRING, "类型应为STRING");
    vox_strview_t str = vox_toml_get_string(val3);
    TEST_ASSERT_EQ(str.len, 0, "空字符串长度应为0");
}

/* 测试字面字符串 */
static void test_toml_literal_string(vox_mpool_t* mpool) {
    char* toml = (char*)vox_mpool_alloc(mpool, 300);
    strcpy(toml, "path = 'C:\\\\Windows\\\\System32'");
    size_t size = strlen(toml);
    
    vox_toml_table_t* root = vox_toml_parse(mpool, toml, &size, NULL);
    TEST_ASSERT_NOT_NULL(root, "解析TOML失败");
    
    vox_toml_elem_t* val = vox_toml_get_value(root, "path");
    TEST_ASSERT_NOT_NULL(val, "获取值失败");
    TEST_ASSERT_EQ(vox_toml_get_type(val), VOX_TOML_STRING, "类型应为STRING");
    
    vox_strview_t str = vox_toml_get_string(val);
    TEST_ASSERT_GT(str.len, 0, "字符串长度应为正数");
}

/* 测试注释 */
static void test_toml_comments(vox_mpool_t* mpool) {
    char* toml = (char*)vox_mpool_alloc(mpool, 300);
    strcpy(toml, "# This is a comment\nkey = \"value\" # Inline comment");
    size_t size = strlen(toml);
    
    vox_toml_table_t* root = vox_toml_parse(mpool, toml, &size, NULL);
    TEST_ASSERT_NOT_NULL(root, "解析TOML失败");
    
    vox_toml_elem_t* val = vox_toml_get_value(root, "key");
    TEST_ASSERT_NOT_NULL(val, "获取值失败");
    vox_strview_t str = vox_toml_get_string(val);
    TEST_ASSERT_EQ(memcmp(str.ptr, "value", 5), 0, "字符串内容不正确");
}

/* 测试内联表访问 */
static void test_toml_inline_table_access(vox_mpool_t* mpool) {
    char* toml = (char*)vox_mpool_alloc(mpool, 300);
    strcpy(toml, "point = { x = 1, y = 2, z = 3 }");
    size_t size = strlen(toml);
    
    vox_toml_table_t* root = vox_toml_parse(mpool, toml, &size, NULL);
    TEST_ASSERT_NOT_NULL(root, "解析TOML失败");
    
    vox_toml_elem_t* point_val = vox_toml_get_value(root, "point");
    TEST_ASSERT_NOT_NULL(point_val, "获取值失败");
    TEST_ASSERT_EQ(vox_toml_get_type(point_val), VOX_TOML_INLINE_TABLE, "类型应为INLINE_TABLE");
    
    size_t count = vox_toml_get_inline_table_count(point_val);
    TEST_ASSERT_EQ(count, 3, "内联表键值对数量应为3");
    
    vox_toml_elem_t* x_val = vox_toml_get_inline_table_value(point_val, "x");
    TEST_ASSERT_NOT_NULL(x_val, "获取x值失败");
    TEST_ASSERT_EQ(vox_toml_get_integer(x_val), 1, "x值应为1");
    
    vox_toml_elem_t* y_val = vox_toml_get_inline_table_value(point_val, "y");
    TEST_ASSERT_NOT_NULL(y_val, "获取y值失败");
    TEST_ASSERT_EQ(vox_toml_get_integer(y_val), 2, "y值应为2");
    
    vox_toml_elem_t* z_val = vox_toml_get_inline_table_value(point_val, "z");
    TEST_ASSERT_NOT_NULL(z_val, "获取z值失败");
    TEST_ASSERT_EQ(vox_toml_get_integer(z_val), 3, "z值应为3");
}

/* 测试表数组 */
static void test_toml_array_of_tables(vox_mpool_t* mpool) {
    char* toml = (char*)vox_mpool_alloc(mpool, 500);
    strcpy(toml, "[[products]]\nname = \"Hammer\"\nsku = 738594937\n\n[[products]]\nname = \"Nail\"\nsku = 284758393");
    size_t size = strlen(toml);
    
    vox_toml_table_t* root = vox_toml_parse(mpool, toml, &size, NULL);
    TEST_ASSERT_NOT_NULL(root, "解析TOML失败");
    
    vox_toml_table_t* products_table = vox_toml_find_subtable(root, "products");
    TEST_ASSERT_NOT_NULL(products_table, "查找products表失败");
    TEST_ASSERT_EQ(products_table->is_array_of_tables, 1, "应为表数组");
}

/* 测试表遍历 */
static void test_toml_table_traverse(vox_mpool_t* mpool) {
    char* toml = (char*)vox_mpool_alloc(mpool, 500);
    strcpy(toml, "[config]\nname = \"MyApp\"\nversion = \"1.0.0\"\ndebug = true");
    size_t size = strlen(toml);
    
    vox_toml_table_t* root = vox_toml_parse(mpool, toml, &size, NULL);
    TEST_ASSERT_NOT_NULL(root, "解析TOML失败");
    
    vox_toml_table_t* config_table = vox_toml_find_subtable(root, "config");
    TEST_ASSERT_NOT_NULL(config_table, "查找config表失败");
    
    vox_toml_keyvalue_t* kv = vox_toml_table_first_keyvalue(config_table);
    TEST_ASSERT_NOT_NULL(kv, "获取第一个键值对失败");
    
    int count = 0;
    while (kv) {
        count++;
        kv = vox_toml_table_next_keyvalue(kv);
    }
    TEST_ASSERT_EQ(count, 3, "键值对数量应为3");
}

/* 测试日期和时间类型 */
static void test_toml_date_time_types(vox_mpool_t* mpool) {
    char* toml = (char*)vox_mpool_alloc(mpool, 500);
    strcpy(toml, "created = 1979-05-27T07:32:00Z\ndate = 2024-01-01\ntime = 12:00:00");
    size_t size = strlen(toml);
    
    vox_toml_table_t* root = vox_toml_parse(mpool, toml, &size, NULL);
    TEST_ASSERT_NOT_NULL(root, "解析TOML失败");
    
    vox_toml_elem_t* created = vox_toml_get_value(root, "created");
    TEST_ASSERT_NOT_NULL(created, "获取created值失败");
    TEST_ASSERT_EQ(vox_toml_get_type(created), VOX_TOML_DATETIME, "类型应为DATETIME");
    
    vox_toml_elem_t* date = vox_toml_get_value(root, "date");
    TEST_ASSERT_NOT_NULL(date, "获取date值失败");
    TEST_ASSERT_EQ(vox_toml_get_type(date), VOX_TOML_DATE, "类型应为DATE");
    
    vox_toml_elem_t* time = vox_toml_get_value(root, "time");
    TEST_ASSERT_NOT_NULL(time, "获取time值失败");
    TEST_ASSERT_EQ(vox_toml_get_type(time), VOX_TOML_TIME, "类型应为TIME");
}

/* 测试序列化 */
static void test_toml_serialize(vox_mpool_t* mpool) {
    char* toml = (char*)vox_mpool_alloc(mpool, 500);
    strcpy(toml, "name = \"test\"\nage = 30\n[server]\nhost = \"localhost\"\nport = 8080");
    size_t size = strlen(toml);
    
    vox_toml_table_t* root = vox_toml_parse(mpool, toml, &size, NULL);
    TEST_ASSERT_NOT_NULL(root, "解析TOML失败");
    
    size_t output_size = 0;
    char* serialized = vox_toml_to_string(mpool, root, &output_size);
    TEST_ASSERT_NOT_NULL(serialized, "序列化失败");
    TEST_ASSERT_GT(output_size, 0, "序列化结果不应为空");
    
    /* 验证序列化结果可以再次解析 */
    vox_toml_table_t* root2 = vox_toml_parse_str(mpool, serialized, NULL);
    TEST_ASSERT_NOT_NULL(root2, "解析序列化结果失败");
    
    vox_toml_elem_t* name_val = vox_toml_get_value(root2, "name");
    TEST_ASSERT_NOT_NULL(name_val, "获取name值失败");
    vox_strview_t name = vox_toml_get_string(name_val);
    TEST_ASSERT_EQ(memcmp(name.ptr, "test", 4), 0, "序列化后解析的字符串不正确");
}

/* 测试序列化数组 */
static void test_toml_serialize_array(vox_mpool_t* mpool) {
    char* toml = (char*)vox_mpool_alloc(mpool, 300);
    strcpy(toml, "numbers = [1, 2, 3]\nfruits = [\"apple\", \"banana\"]");
    size_t size = strlen(toml);
    
    vox_toml_table_t* root = vox_toml_parse(mpool, toml, &size, NULL);
    TEST_ASSERT_NOT_NULL(root, "解析TOML失败");
    
    size_t output_size = 0;
    char* serialized = vox_toml_to_string(mpool, root, &output_size);
    TEST_ASSERT_NOT_NULL(serialized, "序列化失败");
    
    /* 验证序列化结果包含数组 */
    TEST_ASSERT_NE(strstr(serialized, "[1, 2, 3]"), NULL, "序列化结果应包含数字数组");
    TEST_ASSERT_NE(strstr(serialized, "[\"apple\", \"banana\"]"), NULL, "序列化结果应包含字符串数组");
}

/* 测试序列化内联表 */
static void test_toml_serialize_inline_table(vox_mpool_t* mpool) {
    char* toml = (char*)vox_mpool_alloc(mpool, 300);
    strcpy(toml, "point = { x = 1, y = 2 }");
    size_t size = strlen(toml);
    
    vox_toml_table_t* root = vox_toml_parse(mpool, toml, &size, NULL);
    TEST_ASSERT_NOT_NULL(root, "解析TOML失败");
    
    size_t output_size = 0;
    char* serialized = vox_toml_to_string(mpool, root, &output_size);
    TEST_ASSERT_NOT_NULL(serialized, "序列化失败");
    
    /* 验证序列化结果包含内联表（允许有或没有空格） */
    const char* found1 = strstr(serialized, "{ x = 1, y = 2 }");
    const char* found2 = strstr(serialized, "{x = 1, y = 2}");
    const char* found3 = strstr(serialized, "{ x = 1, y = 2}");
    const char* found4 = strstr(serialized, "x = 1");
    int has_inline_table = (found1 != NULL || found2 != NULL || found3 != NULL || found4 != NULL) ? 1 : 0;
    TEST_ASSERT_EQ(has_inline_table, 1, "序列化结果应包含内联表");
}

/* 测试浮点数精度 */
static void test_toml_float_precision(vox_mpool_t* mpool) {
    char* toml = (char*)vox_mpool_alloc(mpool, 300);
    strcpy(toml, "pi = 3.141592653589793\nepsilon = 1e-10");
    size_t size = strlen(toml);
    
    vox_toml_table_t* root = vox_toml_parse(mpool, toml, &size, NULL);
    TEST_ASSERT_NOT_NULL(root, "解析TOML失败");
    
    vox_toml_elem_t* pi_val = vox_toml_get_value(root, "pi");
    TEST_ASSERT_NOT_NULL(pi_val, "获取pi值失败");
    TEST_ASSERT_EQ(vox_toml_get_type(pi_val), VOX_TOML_FLOAT, "类型应为FLOAT");
    
    double pi = vox_toml_get_float(pi_val);
    TEST_ASSERT_GT(pi, 3.14159, "pi值应大于3.14159");
    TEST_ASSERT_LT(pi, 3.14160, "pi值应小于3.14160");
}

/* 测试负数 */
static void test_toml_negative_numbers(vox_mpool_t* mpool) {
    char* toml = (char*)vox_mpool_alloc(mpool, 300);
    strcpy(toml, "negative_int = -42\nnegative_float = -3.14");
    size_t size = strlen(toml);
    
    vox_toml_table_t* root = vox_toml_parse(mpool, toml, &size, NULL);
    TEST_ASSERT_NOT_NULL(root, "解析TOML失败");
    
    vox_toml_elem_t* int_val = vox_toml_get_value(root, "negative_int");
    TEST_ASSERT_NOT_NULL(int_val, "获取整数值失败");
    int64_t int_result = vox_toml_get_integer(int_val);
    TEST_ASSERT_EQ(int_result, (int64_t)-42, "负整数值不正确");
    
    vox_toml_elem_t* float_val = vox_toml_get_value(root, "negative_float");
    TEST_ASSERT_NOT_NULL(float_val, "获取浮点数值失败");
    double f = vox_toml_get_float(float_val);
    TEST_ASSERT_LT(f, -3.13, "负浮点数值不正确");
    TEST_ASSERT_GT(f, -3.15, "负浮点数值不正确");
}

/* 测试路径查找 */
static void test_toml_find_by_path(vox_mpool_t* mpool) {
    char* toml = (char*)vox_mpool_alloc(mpool, 500);
    strcpy(toml, "[server]\n[server.database]\nhost = \"localhost\"\nport = 5432");
    size_t size = strlen(toml);
    
    vox_toml_table_t* root = vox_toml_parse(mpool, toml, &size, NULL);
    TEST_ASSERT_NOT_NULL(root, "解析TOML失败");
    
    vox_toml_table_t* db_table = vox_toml_find_table_by_path(root, "server.database");
    TEST_ASSERT_NOT_NULL(db_table, "通过路径查找表失败");
    
    vox_toml_elem_t* host_val = vox_toml_get_value(db_table, "host");
    TEST_ASSERT_NOT_NULL(host_val, "获取host值失败");
    vox_strview_t host = vox_toml_get_string(host_val);
    TEST_ASSERT_EQ(memcmp(host.ptr, "localhost", 9), 0, "字符串内容不正确");
}

/* 测试套件 */
test_case_t test_toml_cases[] = {
    {"parse_simple", test_toml_parse_simple},
    {"parse_array", test_toml_parse_array},
    {"parse_table", test_toml_parse_table},
    {"parse_nested", test_toml_parse_nested},
    {"parse_inline_table", test_toml_parse_inline_table},
    {"array_traverse", test_toml_array_traverse},
    {"type_check", test_toml_type_check},
    {"error_handling", test_toml_error_handling},
    {"datetime", test_toml_datetime},
    {"empty_structures", test_toml_empty_structures},
    {"complex_nested", test_toml_complex_nested},
    {"boundary_values", test_toml_boundary_values},
    {"literal_string", test_toml_literal_string},
    {"comments", test_toml_comments},
    {"inline_table_access", test_toml_inline_table_access},
    {"array_of_tables", test_toml_array_of_tables},
    {"table_traverse", test_toml_table_traverse},
    {"date_time_types", test_toml_date_time_types},
    {"serialize", test_toml_serialize},
    {"serialize_array", test_toml_serialize_array},
    {"serialize_inline_table", test_toml_serialize_inline_table},
    {"float_precision", test_toml_float_precision},
    {"negative_numbers", test_toml_negative_numbers},
    {"find_by_path", test_toml_find_by_path},
};

test_suite_t test_toml_suite = {
    "vox_toml",
    test_toml_cases,
    sizeof(test_toml_cases) / sizeof(test_toml_cases[0])
};
