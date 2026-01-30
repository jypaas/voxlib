/*
 * json_example.c - JSON 解析器示例程序
 * 演示 vox_json 的基本用法
 */

#include "../vox_json.h"
#include "../vox_mpool.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* 打印字符串视图 */
static void print_strview(const char* label, const vox_strview_t* sv) {
    if (label && label[0] != '\0') {
        printf("%s: ", label);
    }
    if (sv && sv->ptr && sv->len > 0) {
        printf("\"%.*s\"", (int)sv->len, sv->ptr);
    } else {
        printf("(空)");
    }
}

/* 示例1: 简单对象 */
static void example_simple_object(void) {
    printf("=== 示例 1: 简单对象 ===\n");
    
    const char* json_str = "{\"name\":\"张三\",\"age\":30,\"city\":\"北京\"}";
    printf("JSON: %s\n", json_str);
    
    vox_mpool_t* mpool = vox_mpool_create();
    if (!mpool) {
        fprintf(stderr, "创建内存池失败\n");
        return;
    }
    
    vox_json_err_info_t err_info;
    vox_json_elem_t* root = vox_json_parse_str(mpool, json_str, &err_info);
    
    if (!root) {
        printf("解析错误: %s\n", err_info.message);
        printf("位置: 行 %d, 列 %d, 偏移 %zu\n", err_info.line, err_info.column, err_info.offset);
        vox_mpool_destroy(mpool);
        return;
    }
    
    printf("解析成功，共 %zu 个成员\n\n", vox_json_get_object_count(root));
    
    /* 查找键值 */
    vox_json_elem_t* name_elem = vox_json_get_object_value(root, "name");
    if (name_elem) {
        vox_strview_t name = vox_json_get_string(name_elem);
        printf("  name = ");
        print_strview("", &name);
        printf("\n");
    }
    
    vox_json_elem_t* age_elem = vox_json_get_object_value(root, "age");
    if (age_elem) {
        int64_t age = vox_json_get_int(age_elem);
        printf("  age = %lld\n", (long long)age);
    }
    
    vox_json_elem_t* city_elem = vox_json_get_object_value(root, "city");
    if (city_elem) {
        vox_strview_t city = vox_json_get_string(city_elem);
        printf("  city = ");
        print_strview("", &city);
        printf("\n");
    }
    
    vox_mpool_destroy(mpool);
    printf("\n");
}

/* 示例2: 嵌套对象和数组 */
static void example_nested_structure(void) {
    printf("=== 示例 2: 嵌套对象和数组 ===\n");
    
    const char* json_str = "{\"users\":[{\"id\":1,\"name\":\"Alice\"},{\"id\":2,\"name\":\"Bob\"}],\"count\":2}";
    printf("JSON: %s\n", json_str);
    
    vox_mpool_t* mpool = vox_mpool_create();
    if (!mpool) {
        fprintf(stderr, "创建内存池失败\n");
        return;
    }
    
    vox_json_err_info_t err_info;
    vox_json_elem_t* root = vox_json_parse_str(mpool, json_str, &err_info);
    
    if (!root) {
        printf("解析错误: %s\n", err_info.message);
        printf("位置: 行 %d, 列 %d, 偏移 %zu\n", err_info.line, err_info.column, err_info.offset);
        vox_mpool_destroy(mpool);
        return;
    }
    
    printf("解析成功，共 %zu 个 tokens\n\n", vox_json_get_object_count(root));
    
    /* 访问 users 数组 */
    vox_json_elem_t* users_elem = vox_json_get_object_value(root, "users");
    if (users_elem && vox_json_is_type(users_elem, VOX_JSON_ARRAY)) {
        printf("访问 users 数组:\n");
        size_t array_size = vox_json_get_array_count(users_elem);
        printf("  数组大小: %zu\n", array_size);
        
        for (size_t i = 0; i < array_size; i++) {
            vox_json_elem_t* user = vox_json_get_array_elem(users_elem, i);
            if (user && vox_json_is_type(user, VOX_JSON_OBJECT)) {
                printf("  用户 %zu:\n", i);
                
                vox_json_elem_t* id_elem = vox_json_get_object_value(user, "id");
                if (id_elem) {
                    int64_t id = vox_json_get_int(id_elem);
                    printf("    id = %lld\n", (long long)id);
                }
                
                vox_json_elem_t* name_elem = vox_json_get_object_value(user, "name");
                if (name_elem) {
                    vox_strview_t name = vox_json_get_string(name_elem);
                    printf("    name = ");
                    print_strview("", &name);
                    printf("\n");
                }
            }
        }
    }
    
    vox_mpool_destroy(mpool);
    printf("\n");
}

/* 示例3: 原始值类型 */
static void example_primitive_types(void) {
    printf("=== 示例 3: 原始值类型 ===\n");
    
    const char* json_str = "{\"null_value\":null,\"true_value\":true,\"false_value\":false,\"number\":123.45}";
    printf("JSON: %s\n", json_str);
    
    vox_mpool_t* mpool = vox_mpool_create();
    if (!mpool) {
        fprintf(stderr, "创建内存池失败\n");
        return;
    }
    
    vox_json_err_info_t err_info;
    vox_json_elem_t* root = vox_json_parse_str(mpool, json_str, &err_info);
    
    if (!root) {
        printf("解析错误: %s\n", err_info.message);
        printf("位置: 行 %d, 列 %d, 偏移 %zu\n", err_info.line, err_info.column, err_info.offset);
        vox_mpool_destroy(mpool);
        return;
    }
    
    printf("解析成功，共 %zu 个 tokens\n\n", vox_json_get_object_count(root));
    
    vox_json_elem_t* null_elem = vox_json_get_object_value(root, "null_value");
    if (null_elem) {
        printf("  null_value 是 null: %s\n", 
               vox_json_is_type(null_elem, VOX_JSON_NULL) ? "true" : "false");
    }
    
    vox_json_elem_t* true_elem = vox_json_get_object_value(root, "true_value");
    if (true_elem) {
        bool val = vox_json_get_bool(true_elem);
        printf("  true_value 是 true: %s\n", val ? "true" : "false");
    }
    
    vox_json_elem_t* false_elem = vox_json_get_object_value(root, "false_value");
    if (false_elem) {
        bool val = vox_json_get_bool(false_elem);
        printf("  false_value 是 false: %s\n", val ? "false" : "true");
    }
    
    vox_json_elem_t* number_elem = vox_json_get_object_value(root, "number");
    if (number_elem) {
        double num = vox_json_get_number(number_elem);
        printf("  number = %f\n", num);
    }
    
    vox_mpool_destroy(mpool);
    printf("\n");
}

/* 示例4: 计算所需 token 数量 */
static void example_count_tokens(void) {
    printf("=== 示例 4: 计算所需 token 数量 ===\n");
    
    const char* json_str = "{\"a\":1,\"b\":2,\"c\":[1,2,3],\"d\":{\"e\":\"f\"}}";
    printf("JSON: %s\n", json_str);
    
    vox_mpool_t* mpool = vox_mpool_create();
    if (!mpool) {
        fprintf(stderr, "创建内存池失败\n");
        return;
    }
    
    vox_json_err_info_t err_info;
    vox_json_elem_t* root = vox_json_parse_str(mpool, json_str, &err_info);
    
    if (!root) {
        printf("解析错误: %s\n", err_info.message);
        printf("位置: 行 %d, 列 %d, 偏移 %zu\n", err_info.line, err_info.column, err_info.offset);
        vox_mpool_destroy(mpool);
        return;
    }
    
    /* 递归计算 token 数量 */
    size_t count = 0;
    if (vox_json_is_type(root, VOX_JSON_OBJECT)) {
        count = vox_json_get_object_count(root);
    } else if (vox_json_is_type(root, VOX_JSON_ARRAY)) {
        count = vox_json_get_array_count(root);
    } else {
        count = 1;
    }
    
    printf("需要 %zu 个 tokens\n", count);
    
    vox_mpool_destroy(mpool);
    printf("\n");
}

/* 示例5: 遍历数组 */
static void example_traverse_array(void) {
    printf("=== 示例 5: 遍历数组 ===\n");
    
    const char* json_str = "[1,2,3,4,5]";
    printf("JSON: %s\n", json_str);
    
    vox_mpool_t* mpool = vox_mpool_create();
    if (!mpool) {
        fprintf(stderr, "创建内存池失败\n");
        return;
    }
    
    vox_json_err_info_t err_info;
    vox_json_elem_t* root = vox_json_parse_str(mpool, json_str, &err_info);
    
    if (!root) {
        printf("解析错误: %s\n", err_info.message);
        vox_mpool_destroy(mpool);
        return;
    }
    
    printf("遍历数组元素:\n");
    vox_json_elem_t* item = vox_json_array_first(root);
    size_t index = 0;
    while (item) {
        if (vox_json_is_type(item, VOX_JSON_NUMBER)) {
            double num = vox_json_get_number(item);
            printf("  [%zu] = %g\n", index, num);
        }
        item = vox_json_array_next(item);
        index++;
    }
    
    vox_mpool_destroy(mpool);
    printf("\n");
}

/* 示例6: 遍历对象 */
static void example_traverse_object(void) {
    printf("=== 示例 6: 遍历对象 ===\n");
    
    const char* json_str = "{\"name\":\"John\",\"age\":30,\"city\":\"New York\"}";
    printf("JSON: %s\n", json_str);
    
    vox_mpool_t* mpool = vox_mpool_create();
    if (!mpool) {
        fprintf(stderr, "创建内存池失败\n");
        return;
    }
    
    vox_json_err_info_t err_info;
    vox_json_elem_t* root = vox_json_parse_str(mpool, json_str, &err_info);
    
    if (!root) {
        printf("解析错误: %s\n", err_info.message);
        vox_mpool_destroy(mpool);
        return;
    }
    
    printf("遍历对象成员:\n");
    vox_json_member_t* member = vox_json_object_first(root);
    while (member) {
        printf("  ");
        print_strview("", &member->name);
        printf(": ");
        
        vox_json_type_t type = vox_json_get_type(member->value);
        switch (type) {
            case VOX_JSON_STRING: {
                vox_strview_t sv = vox_json_get_string(member->value);
                print_strview("", &sv);
                break;
            }
            case VOX_JSON_NUMBER: {
                double num = vox_json_get_number(member->value);
                printf("%g", num);
                break;
            }
            case VOX_JSON_BOOLEAN: {
                bool val = vox_json_get_bool(member->value);
                printf("%s", val ? "true" : "false");
                break;
            }
            case VOX_JSON_NULL:
                printf("null");
                break;
            default:
                printf("(complex)");
                break;
        }
        printf("\n");
        
        member = vox_json_object_next(member);
    }
    
    vox_mpool_destroy(mpool);
    printf("\n");
}

/* 示例7: 带换行和格式化的 JSON */
static void example_formatted_json(void) {
    printf("=== 示例 7: 带换行和格式化的 JSON ===\n");
    
    /* 包含换行、制表符和空格的格式化 JSON */
    const char* json_str = 
        "{\n"
        "  \"name\": \"李四\",\n"
        "  \"age\": 25,\n"
        "  \"address\": {\n"
        "    \"street\": \"中关村大街\",\n"
        "    \"city\": \"北京\",\n"
        "    \"zipcode\": \"100080\"\n"
        "  },\n"
        "  \"hobbies\": [\n"
        "    \"读书\",\n"
        "    \"编程\",\n"
        "    \"旅行\"\n"
        "  ],\n"
        "  \"active\": true\n"
        "}";
    
    printf("JSON (带换行):\n%s\n", json_str);
    
    vox_mpool_t* mpool = vox_mpool_create();
    if (!mpool) {
        fprintf(stderr, "创建内存池失败\n");
        return;
    }
    
    vox_json_err_info_t err_info;
    vox_json_elem_t* root = vox_json_parse_str(mpool, json_str, &err_info);
    
    if (!root) {
        printf("解析错误: %s\n", err_info.message);
        printf("位置: 行 %d, 列 %d, 偏移 %zu\n", err_info.line, err_info.column, err_info.offset);
        vox_mpool_destroy(mpool);
        return;
    }
    
    printf("解析成功！\n\n");
    
    /* 访问各个字段 */
    vox_json_elem_t* name_elem = vox_json_get_object_value(root, "name");
    if (name_elem) {
        vox_strview_t name = vox_json_get_string(name_elem);
        printf("  name = ");
        print_strview("", &name);
        printf("\n");
    }
    
    vox_json_elem_t* age_elem = vox_json_get_object_value(root, "age");
    if (age_elem) {
        int64_t age = vox_json_get_int(age_elem);
        printf("  age = %lld\n", (long long)age);
    }
    
    /* 访问嵌套对象 */
    vox_json_elem_t* address_elem = vox_json_get_object_value(root, "address");
    if (address_elem && vox_json_is_type(address_elem, VOX_JSON_OBJECT)) {
        printf("  address:\n");
        vox_json_elem_t* street_elem = vox_json_get_object_value(address_elem, "street");
        if (street_elem) {
            vox_strview_t street = vox_json_get_string(street_elem);
            printf("    street = ");
            print_strview("", &street);
            printf("\n");
        }
        vox_json_elem_t* city_elem = vox_json_get_object_value(address_elem, "city");
        if (city_elem) {
            vox_strview_t city = vox_json_get_string(city_elem);
            printf("    city = ");
            print_strview("", &city);
            printf("\n");
        }
    }
    
    /* 访问数组 */
    vox_json_elem_t* hobbies_elem = vox_json_get_object_value(root, "hobbies");
    if (hobbies_elem && vox_json_is_type(hobbies_elem, VOX_JSON_ARRAY)) {
        printf("  hobbies:\n");
        size_t array_size = vox_json_get_array_count(hobbies_elem);
        for (size_t i = 0; i < array_size; i++) {
            vox_json_elem_t* hobby = vox_json_get_array_elem(hobbies_elem, i);
            if (hobby) {
                vox_strview_t hobby_str = vox_json_get_string(hobby);
                printf("    [%zu] = ", i);
                print_strview("", &hobby_str);
                printf("\n");
            }
        }
    }
    
    vox_mpool_destroy(mpool);
    printf("\n");
}

/* 示例8: 紧凑格式和宽松格式混合 */
static void example_mixed_format(void) {
    printf("=== 示例 8: 紧凑格式和宽松格式混合 ===\n");
    
    /* 混合使用紧凑和宽松格式的 JSON */
    const char* json_str = 
        "{\"id\":1,\"name\":\"王五\",\n"
        "\"tags\":[\"tag1\",\"tag2\",\"tag3\"],\n"
        "  \"metadata\":{\"version\":\"1.0\",\"author\":\"系统\"},\n"
        "\"status\":\"active\"\n"
        "}";
    
    printf("JSON (混合格式):\n%s\n", json_str);
    
    vox_mpool_t* mpool = vox_mpool_create();
    if (!mpool) {
        fprintf(stderr, "创建内存池失败\n");
        return;
    }
    
    vox_json_err_info_t err_info;
    vox_json_elem_t* root = vox_json_parse_str(mpool, json_str, &err_info);
    
    if (!root) {
        printf("解析错误: %s\n", err_info.message);
        printf("位置: 行 %d, 列 %d, 偏移 %zu\n", err_info.line, err_info.column, err_info.offset);
        vox_mpool_destroy(mpool);
        return;
    }
    
    printf("解析成功！\n\n");
    
    /* 遍历所有成员 */
    vox_json_member_t* member = vox_json_object_first(root);
    while (member) {
        printf("  ");
        print_strview("", &member->name);
        printf(": ");
        
        vox_json_type_t type = vox_json_get_type(member->value);
        switch (type) {
            case VOX_JSON_STRING: {
                vox_strview_t sv = vox_json_get_string(member->value);
                print_strview("", &sv);
                break;
            }
            case VOX_JSON_NUMBER: {
                double num = vox_json_get_number(member->value);
                printf("%g", num);
                break;
            }
            case VOX_JSON_ARRAY: {
                printf("[");
                size_t count = vox_json_get_array_count(member->value);
                for (size_t i = 0; i < count; i++) {
                    vox_json_elem_t* elem = vox_json_get_array_elem(member->value, i);
                    if (elem && vox_json_is_type(elem, VOX_JSON_STRING)) {
                        vox_strview_t sv = vox_json_get_string(elem);
                        print_strview("", &sv);
                    }
                    if (i < count - 1) printf(", ");
                }
                printf("]");
                break;
            }
            case VOX_JSON_OBJECT: {
                printf("{...}");
                break;
            }
            default:
                printf("(unknown)");
                break;
        }
        printf("\n");
        
        member = vox_json_object_next(member);
    }
    
    vox_mpool_destroy(mpool);
    printf("\n");
}

/* 示例9: 包含制表符和多个连续空格的 JSON */
static void example_tabs_and_spaces(void) {
    printf("=== 示例 9: 包含制表符和多个连续空格的 JSON ===\n");
    
    /* 包含制表符和多个连续空格的 JSON */
    const char* json_str = 
        "{\t\"key1\":\t\"value1\",\n"
        "\t\t\"key2\":\t\t123,\n"
        "  \"key3\":    \"value3\",\n"
        "\t  \"key4\":\t  [1,  2,  3]\n"
        "}";
    
    printf("JSON (包含制表符和多个空格):\n%s\n", json_str);
    
    vox_mpool_t* mpool = vox_mpool_create();
    if (!mpool) {
        fprintf(stderr, "创建内存池失败\n");
        return;
    }
    
    vox_json_err_info_t err_info;
    vox_json_elem_t* root = vox_json_parse_str(mpool, json_str, &err_info);
    
    if (!root) {
        printf("解析错误: %s\n", err_info.message);
        printf("位置: 行 %d, 列 %d, 偏移 %zu\n", err_info.line, err_info.column, err_info.offset);
        vox_mpool_destroy(mpool);
        return;
    }
    
    printf("解析成功！\n\n");
    
    /* 访问所有键值对 */
    vox_json_member_t* member = vox_json_object_first(root);
    while (member) {
        printf("  ");
        print_strview("", &member->name);
        printf(" = ");
        
        vox_json_type_t type = vox_json_get_type(member->value);
        if (type == VOX_JSON_STRING) {
            vox_strview_t sv = vox_json_get_string(member->value);
            print_strview("", &sv);
        } else if (type == VOX_JSON_NUMBER) {
            double num = vox_json_get_number(member->value);
            printf("%g", num);
        } else if (type == VOX_JSON_ARRAY) {
            printf("[");
            size_t count = vox_json_get_array_count(member->value);
            for (size_t i = 0; i < count; i++) {
                vox_json_elem_t* elem = vox_json_get_array_elem(member->value, i);
                if (elem && vox_json_is_type(elem, VOX_JSON_NUMBER)) {
                    double num = vox_json_get_number(elem);
                    printf("%g", num);
                }
                if (i < count - 1) printf(", ");
            }
            printf("]");
        }
        printf("\n");
        
        member = vox_json_object_next(member);
    }
    
    vox_mpool_destroy(mpool);
    printf("\n");
}

int main(void) {
    printf("========================================\n");
    printf("=== JSON 解析器示例 ===\n");
    printf("========================================\n\n");
    
    example_simple_object();
    example_nested_structure();
    example_primitive_types();
    example_count_tokens();
    example_traverse_array();
    example_traverse_object();
    example_formatted_json();
    example_mixed_format();
    example_tabs_and_spaces();
    
    printf("========================================\n");
    printf("所有示例执行完成\n");
    printf("========================================\n");
    
    return 0;
}
