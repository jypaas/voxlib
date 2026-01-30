/*
 * toml_example.c - TOML 解析器示例程序
 * 演示 vox_toml 的基本用法
 */

#include "../vox_toml.h"
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

/* 示例1: 简单键值对 */
static void example_simple_keyvalues(void) {
    printf("=== 示例 1: 简单键值对 ===\n");
    
    const char* toml_str = 
        "name = \"张三\"\n"
        "age = 30\n"
        "active = true\n"
        "pi = 3.14159";
    
    printf("TOML:\n%s\n", toml_str);
    
    vox_mpool_t* mpool = vox_mpool_create();
    if (!mpool) {
        fprintf(stderr, "创建内存池失败\n");
        return;
    }
    
    vox_toml_err_info_t err_info;
    vox_toml_table_t* root = vox_toml_parse_str(mpool, toml_str, &err_info);
    
    if (!root) {
        printf("解析错误: %s\n", err_info.message);
        printf("位置: 行 %d, 列 %d, 偏移 %zu\n", err_info.line, err_info.column, err_info.offset);
        vox_mpool_destroy(mpool);
        return;
    }
    
    printf("解析成功！\n\n");
    
    /* 访问各个值 */
    vox_toml_elem_t* name_val = vox_toml_get_value(root, "name");
    if (name_val) {
        vox_strview_t name = vox_toml_get_string(name_val);
        printf("  name = ");
        print_strview("", &name);
        printf("\n");
    }
    
    vox_toml_elem_t* age_val = vox_toml_get_value(root, "age");
    if (age_val) {
        int64_t age = vox_toml_get_integer(age_val);
        printf("  age = %lld\n", (long long)age);
    }
    
    vox_toml_elem_t* active_val = vox_toml_get_value(root, "active");
    if (active_val) {
        bool active = vox_toml_get_boolean(active_val);
        printf("  active = %s\n", active ? "true" : "false");
    }
    
    vox_toml_elem_t* pi_val = vox_toml_get_value(root, "pi");
    if (pi_val) {
        double pi = vox_toml_get_float(pi_val);
        printf("  pi = %f\n", pi);
    }
    
    vox_mpool_destroy(mpool);
    printf("\n");
}

/* 示例2: 数组 */
static void example_arrays(void) {
    printf("=== 示例 2: 数组 ===\n");
    
    const char* toml_str = 
        "numbers = [1, 2, 3, 4, 5]\n"
        "fruits = [\"apple\", \"banana\", \"orange\"]\n"
        "mixed = [1, \"two\", 3.0, true]";
    
    printf("TOML:\n%s\n", toml_str);
    
    vox_mpool_t* mpool = vox_mpool_create();
    if (!mpool) {
        fprintf(stderr, "创建内存池失败\n");
        return;
    }
    
    vox_toml_err_info_t err_info;
    vox_toml_table_t* root = vox_toml_parse_str(mpool, toml_str, &err_info);
    
    if (!root) {
        printf("解析错误: %s\n", err_info.message);
        vox_mpool_destroy(mpool);
        return;
    }
    
    printf("解析成功！\n\n");
    
    /* 访问数字数组 */
    vox_toml_elem_t* numbers_val = vox_toml_get_value(root, "numbers");
    if (numbers_val && vox_toml_is_type(numbers_val, VOX_TOML_ARRAY)) {
        printf("  numbers 数组:\n");
        size_t count = vox_toml_get_array_count(numbers_val);
        for (size_t i = 0; i < count; i++) {
            vox_toml_elem_t* elem = vox_toml_get_array_elem(numbers_val, i);
            if (elem) {
                if (vox_toml_is_type(elem, VOX_TOML_INTEGER)) {
                    printf("    [%zu] = %lld\n", i, (long long)vox_toml_get_integer(elem));
                }
            }
        }
    }
    
    /* 访问字符串数组 */
    vox_toml_elem_t* fruits_val = vox_toml_get_value(root, "fruits");
    if (fruits_val && vox_toml_is_type(fruits_val, VOX_TOML_ARRAY)) {
        printf("  fruits 数组:\n");
        vox_toml_elem_t* item = vox_toml_array_first(fruits_val);
        size_t index = 0;
        while (item) {
            if (vox_toml_is_type(item, VOX_TOML_STRING)) {
                vox_strview_t str = vox_toml_get_string(item);
                printf("    [%zu] = ", index);
                print_strview("", &str);
                printf("\n");
            }
            item = vox_toml_array_next(item);
            index++;
        }
    }
    
    vox_mpool_destroy(mpool);
    printf("\n");
}

/* 示例3: 表 */
static void example_tables(void) {
    printf("=== 示例 3: 表 ===\n");
    
    const char* toml_str = 
        "[database]\n"
        "host = \"localhost\"\n"
        "port = 5432\n"
        "name = \"mydb\"\n"
        "\n"
        "[server]\n"
        "host = \"0.0.0.0\"\n"
        "port = 8080";
    
    printf("TOML:\n%s\n", toml_str);
    
    vox_mpool_t* mpool = vox_mpool_create();
    if (!mpool) {
        fprintf(stderr, "创建内存池失败\n");
        return;
    }
    
    vox_toml_err_info_t err_info;
    vox_toml_table_t* root = vox_toml_parse_str(mpool, toml_str, &err_info);
    
    if (!root) {
        printf("解析错误: %s\n", err_info.message);
        vox_mpool_destroy(mpool);
        return;
    }
    
    printf("解析成功！\n\n");
    
    /* 访问 database 表 */
    vox_toml_table_t* db_table = vox_toml_find_subtable(root, "database");
    if (db_table) {
        printf("  [database] 表:\n");
        vox_toml_elem_t* host_val = vox_toml_get_value(db_table, "host");
        if (host_val) {
            vox_strview_t host = vox_toml_get_string(host_val);
            printf("    host = ");
            print_strview("", &host);
            printf("\n");
        }
        vox_toml_elem_t* port_val = vox_toml_get_value(db_table, "port");
        if (port_val) {
            printf("    port = %lld\n", (long long)vox_toml_get_integer(port_val));
        }
    }
    
    /* 访问 server 表 */
    vox_toml_table_t* server_table = vox_toml_find_subtable(root, "server");
    if (server_table) {
        printf("  [server] 表:\n");
        vox_toml_elem_t* host_val = vox_toml_get_value(server_table, "host");
        if (host_val) {
            vox_strview_t host = vox_toml_get_string(host_val);
            printf("    host = ");
            print_strview("", &host);
            printf("\n");
        }
        vox_toml_elem_t* port_val = vox_toml_get_value(server_table, "port");
        if (port_val) {
            printf("    port = %lld\n", (long long)vox_toml_get_integer(port_val));
        }
    }
    
    vox_mpool_destroy(mpool);
    printf("\n");
}

/* 示例4: 嵌套表 */
static void example_nested_tables(void) {
    printf("=== 示例 4: 嵌套表 ===\n");
    
    const char* toml_str = 
        "[server]\n"
        "host = \"0.0.0.0\"\n"
        "port = 8080\n"
        "\n"
        "[server.database]\n"
        "host = \"localhost\"\n"
        "port = 5432\n"
        "name = \"testdb\"";
    
    printf("TOML:\n%s\n", toml_str);
    
    vox_mpool_t* mpool = vox_mpool_create();
    if (!mpool) {
        fprintf(stderr, "创建内存池失败\n");
        return;
    }
    
    vox_toml_err_info_t err_info;
    vox_toml_table_t* root = vox_toml_parse_str(mpool, toml_str, &err_info);
    
    if (!root) {
        printf("解析错误: %s\n", err_info.message);
        vox_mpool_destroy(mpool);
        return;
    }
    
    printf("解析成功！\n\n");
    
    /* 访问嵌套表 */
    vox_toml_table_t* server_table = vox_toml_find_subtable(root, "server");
    if (server_table) {
        printf("  [server] 表:\n");
        vox_toml_elem_t* host_val = vox_toml_get_value(server_table, "host");
        if (host_val) {
            vox_strview_t host = vox_toml_get_string(host_val);
            printf("    host = ");
            print_strview("", &host);
            printf("\n");
        }
        
        /* 访问嵌套的 database 表 */
        vox_toml_table_t* db_table = vox_toml_find_subtable(server_table, "database");
        if (db_table) {
            printf("  [server.database] 表:\n");
            vox_toml_elem_t* db_host_val = vox_toml_get_value(db_table, "host");
            if (db_host_val) {
                vox_strview_t db_host = vox_toml_get_string(db_host_val);
                printf("    host = ");
                print_strview("", &db_host);
                printf("\n");
            }
            vox_toml_elem_t* db_port_val = vox_toml_get_value(db_table, "port");
            if (db_port_val) {
                printf("    port = %lld\n", (long long)vox_toml_get_integer(db_port_val));
            }
            vox_toml_elem_t* db_name_val = vox_toml_get_value(db_table, "name");
            if (db_name_val) {
                vox_strview_t db_name = vox_toml_get_string(db_name_val);
                printf("    name = ");
                print_strview("", &db_name);
                printf("\n");
            }
        } else {
            printf("  未找到 [server.database] 表\n");
        }
    }
    
    vox_mpool_destroy(mpool);
    printf("\n");
}

/* 示例5: 内联表 */
static void example_inline_tables(void) {
    printf("=== 示例 5: 内联表 ===\n");
    
    const char* toml_str = 
        "point = { x = 1, y = 2, z = 3 }\n"
        "color = { r = 255, g = 128, b = 0 }";
    
    printf("TOML:\n%s\n", toml_str);
    
    vox_mpool_t* mpool = vox_mpool_create();
    if (!mpool) {
        fprintf(stderr, "创建内存池失败\n");
        return;
    }
    
    vox_toml_err_info_t err_info;
    vox_toml_table_t* root = vox_toml_parse_str(mpool, toml_str, &err_info);
    
    if (!root) {
        printf("解析错误: %s\n", err_info.message);
        vox_mpool_destroy(mpool);
        return;
    }
    
    printf("解析成功！\n\n");
    
    /* 访问内联表 */
    vox_toml_elem_t* point_val = vox_toml_get_value(root, "point");
    if (point_val && vox_toml_is_type(point_val, VOX_TOML_INLINE_TABLE)) {
        printf("  point 内联表:\n");
        vox_toml_elem_t* x_val = vox_toml_get_inline_table_value(point_val, "x");
        if (x_val) {
            printf("    x = %lld\n", (long long)vox_toml_get_integer(x_val));
        }
        vox_toml_elem_t* y_val = vox_toml_get_inline_table_value(point_val, "y");
        if (y_val) {
            printf("    y = %lld\n", (long long)vox_toml_get_integer(y_val));
        }
        vox_toml_elem_t* z_val = vox_toml_get_inline_table_value(point_val, "z");
        if (z_val) {
            printf("    z = %lld\n", (long long)vox_toml_get_integer(z_val));
        }
    }
    
    vox_toml_elem_t* color_val = vox_toml_get_value(root, "color");
    if (color_val && vox_toml_is_type(color_val, VOX_TOML_INLINE_TABLE)) {
        printf("  color 内联表:\n");
        vox_toml_elem_t* r_val = vox_toml_get_inline_table_value(color_val, "r");
        if (r_val) {
            printf("    r = %lld\n", (long long)vox_toml_get_integer(r_val));
        }
        vox_toml_elem_t* g_val = vox_toml_get_inline_table_value(color_val, "g");
        if (g_val) {
            printf("    g = %lld\n", (long long)vox_toml_get_integer(g_val));
        }
        vox_toml_elem_t* b_val = vox_toml_get_inline_table_value(color_val, "b");
        if (b_val) {
            printf("    b = %lld\n", (long long)vox_toml_get_integer(b_val));
        }
    }
    
    vox_mpool_destroy(mpool);
    printf("\n");
}

/* 示例6: 日期时间 */
static void example_datetime(void) {
    printf("=== 示例 6: 日期时间 ===\n");
    
    const char* toml_str = 
        "created = 1979-05-27T07:32:00Z\n"
        "updated = 2024-01-01T12:00:00+08:00\n"
        "date = 2024-01-01\n"
        "time = 12:00:00";
    
    printf("TOML:\n%s\n", toml_str);
    
    vox_mpool_t* mpool = vox_mpool_create();
    if (!mpool) {
        fprintf(stderr, "创建内存池失败\n");
        return;
    }
    
    vox_toml_err_info_t err_info;
    vox_toml_table_t* root = vox_toml_parse_str(mpool, toml_str, &err_info);
    
    if (!root) {
        printf("解析错误: %s\n", err_info.message);
        vox_mpool_destroy(mpool);
        return;
    }
    
    printf("解析成功！\n\n");
    
    vox_toml_elem_t* created_val = vox_toml_get_value(root, "created");
    if (created_val) {
        if (vox_toml_is_type(created_val, VOX_TOML_DATETIME)) {
            vox_strview_t dt = vox_toml_get_datetime(created_val);
            printf("  created = ");
            print_strview("", &dt);
            printf("\n");
        } else {
            printf("  created 类型: %d (期望 DATETIME)\n", vox_toml_get_type(created_val));
        }
    }
    
    vox_toml_elem_t* updated_val = vox_toml_get_value(root, "updated");
    if (updated_val) {
        if (vox_toml_is_type(updated_val, VOX_TOML_DATETIME)) {
            vox_strview_t dt = vox_toml_get_datetime(updated_val);
            printf("  updated = ");
            print_strview("", &dt);
            printf("\n");
        } else {
            printf("  updated 类型: %d (期望 DATETIME)\n", vox_toml_get_type(updated_val));
        }
    }
    
    vox_toml_elem_t* date_val = vox_toml_get_value(root, "date");
    if (date_val) {
        if (vox_toml_is_type(date_val, VOX_TOML_DATE)) {
            vox_strview_t date = vox_toml_get_date(date_val);
            printf("  date = ");
            print_strview("", &date);
            printf("\n");
        } else {
            printf("  date 类型: %d (期望 DATE)\n", vox_toml_get_type(date_val));
        }
    }
    
    vox_toml_elem_t* time_val = vox_toml_get_value(root, "time");
    if (time_val) {
        if (vox_toml_is_type(time_val, VOX_TOML_TIME)) {
            vox_strview_t time = vox_toml_get_time(time_val);
            printf("  time = ");
            print_strview("", &time);
            printf("\n");
        } else {
            printf("  time 类型: %d (期望 TIME)\n", vox_toml_get_type(time_val));
        }
    }
    
    vox_mpool_destroy(mpool);
    printf("\n");
}

/* 示例7: 遍历表 */
static void example_traverse_table(void) {
    printf("=== 示例 7: 遍历表 ===\n");
    
    const char* toml_str = 
        "[config]\n"
        "name = \"MyApp\"\n"
        "version = \"1.0.0\"\n"
        "debug = true\n"
        "port = 8080";
    
    printf("TOML:\n%s\n", toml_str);
    
    vox_mpool_t* mpool = vox_mpool_create();
    if (!mpool) {
        fprintf(stderr, "创建内存池失败\n");
        return;
    }
    
    vox_toml_err_info_t err_info;
    vox_toml_table_t* root = vox_toml_parse_str(mpool, toml_str, &err_info);
    
    if (!root) {
        printf("解析错误: %s\n", err_info.message);
        vox_mpool_destroy(mpool);
        return;
    }
    
    printf("解析成功！\n\n");
    
    vox_toml_table_t* config_table = vox_toml_find_subtable(root, "config");
    if (config_table) {
        printf("  遍历 [config] 表的键值对:\n");
        vox_toml_keyvalue_t* kv = vox_toml_table_first_keyvalue(config_table);
        while (kv) {
            printf("    ");
            print_strview("", &kv->key);
            printf(" = ");
            
            vox_toml_type_t type = vox_toml_get_type(kv->value);
            switch (type) {
                case VOX_TOML_STRING: {
                    vox_strview_t sv = vox_toml_get_string(kv->value);
                    print_strview("", &sv);
                    break;
                }
                case VOX_TOML_INTEGER: {
                    printf("%lld", (long long)vox_toml_get_integer(kv->value));
                    break;
                }
                case VOX_TOML_BOOLEAN: {
                    printf("%s", vox_toml_get_boolean(kv->value) ? "true" : "false");
                    break;
                }
                default:
                    printf("(complex)");
                    break;
            }
            printf("\n");
            
            kv = vox_toml_table_next_keyvalue(kv);
        }
    }
    
    vox_mpool_destroy(mpool);
    printf("\n");
}

/* 示例8: 复杂配置 */
static void example_complex_config(void) {
    printf("=== 示例 8: 复杂配置 ===\n");
    
    const char* toml_str = 
        "title = \"TOML 示例\"\n"
        "\n"
        "[owner]\n"
        "name = \"Tom Preston-Werner\"\n"
        "dob = 1979-05-27T07:32:00Z\n"
        "\n"
        "[database]\n"
        "server = \"192.168.1.1\"\n"
        "ports = [8001, 8002, 8003]\n"
        "connection_max = 5000\n"
        "enabled = true\n"
        "\n"
        "[servers]\n"
        "\n"
        "[servers.alpha]\n"
        "ip = \"10.0.0.1\"\n"
        "dc = \"eqdc10\"\n"
        "\n"
        "[servers.beta]\n"
        "ip = \"10.0.0.2\"\n"
        "dc = \"eqdc10\"";
    
    printf("TOML:\n%s\n", toml_str);
    
    vox_mpool_t* mpool = vox_mpool_create();
    if (!mpool) {
        fprintf(stderr, "创建内存池失败\n");
        return;
    }
    
    vox_toml_err_info_t err_info;
    vox_toml_table_t* root = vox_toml_parse_str(mpool, toml_str, &err_info);
    
    if (!root) {
        printf("解析错误: %s\n", err_info.message);
        printf("位置: 行 %d, 列 %d, 偏移 %zu\n", err_info.line, err_info.column, err_info.offset);
        vox_mpool_destroy(mpool);
        return;
    }
    
    printf("解析成功！\n\n");
    
    /* 访问根表的键值对 */
    vox_toml_elem_t* title_val = vox_toml_get_value(root, "title");
    if (title_val) {
        vox_strview_t title = vox_toml_get_string(title_val);
        printf("  title = ");
        print_strview("", &title);
        printf("\n");
    }
    
    /* 访问 owner 表 */
    vox_toml_table_t* owner_table = vox_toml_find_subtable(root, "owner");
    if (owner_table) {
        printf("  [owner] 表:\n");
        vox_toml_elem_t* name_val = vox_toml_get_value(owner_table, "name");
        if (name_val) {
            vox_strview_t name = vox_toml_get_string(name_val);
            printf("    name = ");
            print_strview("", &name);
            printf("\n");
        }
        vox_toml_elem_t* dob_val = vox_toml_get_value(owner_table, "dob");
        if (dob_val) {
            if (vox_toml_is_type(dob_val, VOX_TOML_DATETIME)) {
                vox_strview_t dob = vox_toml_get_datetime(dob_val);
                printf("    dob = ");
                print_strview("", &dob);
                printf("\n");
            }
        }
    }
    
    /* 访问 database 表 */
    vox_toml_table_t* db_table = vox_toml_find_subtable(root, "database");
    if (db_table) {
        printf("  [database] 表:\n");
        vox_toml_elem_t* server_val = vox_toml_get_value(db_table, "server");
        if (server_val) {
            vox_strview_t server = vox_toml_get_string(server_val);
            printf("    server = ");
            print_strview("", &server);
            printf("\n");
        }
        vox_toml_elem_t* ports_val = vox_toml_get_value(db_table, "ports");
        if (ports_val && vox_toml_is_type(ports_val, VOX_TOML_ARRAY)) {
            printf("    ports = [");
            size_t count = vox_toml_get_array_count(ports_val);
            for (size_t i = 0; i < count; i++) {
                vox_toml_elem_t* elem = vox_toml_get_array_elem(ports_val, i);
                if (elem) {
                    printf("%lld", (long long)vox_toml_get_integer(elem));
                    if (i < count - 1) printf(", ");
                }
            }
            printf("]\n");
        }
        vox_toml_elem_t* conn_max_val = vox_toml_get_value(db_table, "connection_max");
        if (conn_max_val) {
            printf("    connection_max = %lld\n", (long long)vox_toml_get_integer(conn_max_val));
        }
        vox_toml_elem_t* enabled_val = vox_toml_get_value(db_table, "enabled");
        if (enabled_val) {
            printf("    enabled = %s\n", vox_toml_get_boolean(enabled_val) ? "true" : "false");
        }
    }
    
    /* 访问 servers.alpha 表 */
    vox_toml_table_t* servers_table = vox_toml_find_subtable(root, "servers");
    if (servers_table) {
        vox_toml_table_t* alpha_table = vox_toml_find_subtable(servers_table, "alpha");
        if (alpha_table) {
            printf("  [servers.alpha] 表:\n");
            vox_toml_elem_t* ip_val = vox_toml_get_value(alpha_table, "ip");
            if (ip_val) {
                vox_strview_t ip = vox_toml_get_string(ip_val);
                printf("    ip = ");
                print_strview("", &ip);
                printf("\n");
            }
            vox_toml_elem_t* dc_val = vox_toml_get_value(alpha_table, "dc");
            if (dc_val) {
                vox_strview_t dc = vox_toml_get_string(dc_val);
                printf("    dc = ");
                print_strview("", &dc);
                printf("\n");
            }
        }
        
        /* 访问 servers.beta 表 */
        vox_toml_table_t* beta_table = vox_toml_find_subtable(servers_table, "beta");
        if (beta_table) {
            printf("  [servers.beta] 表:\n");
            vox_toml_elem_t* ip_val = vox_toml_get_value(beta_table, "ip");
            if (ip_val) {
                vox_strview_t ip = vox_toml_get_string(ip_val);
                printf("    ip = ");
                print_strview("", &ip);
                printf("\n");
            }
            vox_toml_elem_t* dc_val = vox_toml_get_value(beta_table, "dc");
            if (dc_val) {
                vox_strview_t dc = vox_toml_get_string(dc_val);
                printf("    dc = ");
                print_strview("", &dc);
                printf("\n");
            }
        }
    }
    
    vox_mpool_destroy(mpool);
    printf("\n");
}

/* 示例9: 注释解析 */
static void example_comments(void) {
    printf("=== 示例 9: 注释解析 ===\n");
    
    const char* toml_str = 
        "# 这是一个行注释\n"
        "name = \"张三\"  # 行尾注释\n"
        "age = 30  # 年龄\n"
        "\n"
        "# 表注释\n"
        "[server]\n"
        "host = \"localhost\"  # 主机地址\n"
        "port = 8080  # 端口号\n"
        "\n"
        "# 多行注释示例\n"
        "# 这些注释都会被正确跳过\n"
        "version = \"1.0.0\"";
    
    printf("TOML:\n%s\n", toml_str);
    
    vox_mpool_t* mpool = vox_mpool_create();
    if (!mpool) {
        fprintf(stderr, "创建内存池失败\n");
        return;
    }
    
    vox_toml_err_info_t err_info;
    vox_toml_table_t* root = vox_toml_parse_str(mpool, toml_str, &err_info);
    
    if (!root) {
        printf("解析错误: %s\n", err_info.message);
        printf("位置: 行 %d, 列 %d, 偏移 %zu\n", err_info.line, err_info.column, err_info.offset);
        vox_mpool_destroy(mpool);
        return;
    }
    
    printf("解析成功！注释已被正确跳过。\n\n");
    
    /* 访问根表的键值对 */
    vox_toml_elem_t* name_val = vox_toml_get_value(root, "name");
    if (name_val) {
        vox_strview_t name = vox_toml_get_string(name_val);
        printf("  name = ");
        print_strview("", &name);
        printf("\n");
    }
    
    vox_toml_elem_t* age_val = vox_toml_get_value(root, "age");
    if (age_val) {
        printf("  age = %lld\n", (long long)vox_toml_get_integer(age_val));
    }
    
    vox_toml_elem_t* version_val = vox_toml_get_value(root, "version");
    if (version_val) {
        vox_strview_t version = vox_toml_get_string(version_val);
        printf("  version = ");
        print_strview("", &version);
        printf("\n");
    }
    
    /* 访问 server 表 */
    vox_toml_table_t* server_table = vox_toml_find_subtable(root, "server");
    if (server_table) {
        printf("  [server] 表:\n");
        vox_toml_elem_t* host_val = vox_toml_get_value(server_table, "host");
        if (host_val) {
            vox_strview_t host = vox_toml_get_string(host_val);
            printf("    host = ");
            print_strview("", &host);
            printf("\n");
        }
        vox_toml_elem_t* port_val = vox_toml_get_value(server_table, "port");
        if (port_val) {
            printf("    port = %lld\n", (long long)vox_toml_get_integer(port_val));
        }
    }
    
    vox_mpool_destroy(mpool);
    printf("\n");
}

/* 示例10: 序列化和写入文件 */
static void example_serialize(void) {
    printf("=== 示例 10: 序列化和写入文件 ===\n");
    
    const char* toml_str = 
        "name = \"测试配置\"\n"
        "version = \"1.0.0\"\n"
        "debug = true\n"
        "\n"
        "[server]\n"
        "host = \"localhost\"\n"
        "port = 8080\n"
        "\n"
        "[database]\n"
        "host = \"127.0.0.1\"\n"
        "port = 5432\n"
        "name = \"mydb\"";
    
    printf("原始 TOML:\n%s\n\n", toml_str);
    
    vox_mpool_t* mpool = vox_mpool_create();
    if (!mpool) {
        fprintf(stderr, "创建内存池失败\n");
        return;
    }
    
    /* 解析 TOML */
    vox_toml_err_info_t err_info;
    vox_toml_table_t* root = vox_toml_parse_str(mpool, toml_str, &err_info);
    
    if (!root) {
        printf("解析错误: %s\n", err_info.message);
        vox_mpool_destroy(mpool);
        return;
    }
    
    printf("解析成功！\n\n");
    
    /* 序列化为字符串 */
    size_t output_size = 0;
    char* serialized = vox_toml_to_string(mpool, root, &output_size);
    if (serialized) {
        printf("序列化后的 TOML (%zu 字节):\n%s\n", output_size, serialized);
    } else {
        printf("序列化失败\n");
    }
    
    /* 写入文件 */
    const char* output_file = "output.toml";
    if (vox_toml_write_file(mpool, root, output_file) == 0) {
        printf("\n成功写入文件: %s\n", output_file);
    } else {
        printf("\n写入文件失败: %s\n", output_file);
    }
    
    vox_mpool_destroy(mpool);
    printf("\n");
}

/* 示例11: 从文件读取和写入 */
static void example_file_io(void) {
    printf("=== 示例 11: 从文件读取和写入 ===\n");
    
    vox_mpool_t* mpool = vox_mpool_create();
    if (!mpool) {
        fprintf(stderr, "创建内存池失败\n");
        return;
    }
    
    /* 先创建一个测试文件 */
    const char* test_file = "test_config.toml";
    const char* test_content = 
        "app_name = \"My Application\"\n"
        "version = \"1.0.0\"\n"
        "debug = true\n"
        "\n"
        "[database]\n"
        "host = \"localhost\"\n"
        "port = 5432\n"
        "name = \"mydb\"\n"
        "\n"
        "[server]\n"
        "host = \"0.0.0.0\"\n"
        "port = 8080";
    
    /* 写入测试文件 */
    FILE* f = fopen(test_file, "w");
    if (f) {
        fputs(test_content, f);
        fclose(f);
        printf("创建测试文件: %s\n", test_file);
    }
    
    /* 从文件读取 */
    FILE* file = fopen(test_file, "r");
    if (!file) {
        printf("无法打开文件: %s\n", test_file);
        vox_mpool_destroy(mpool);
        return;
    }
    
    /* 获取文件大小 */
    fseek(file, 0, SEEK_END);
    long file_size = ftell(file);
    fseek(file, 0, SEEK_SET);
    
    /* 读取文件内容 */
    char* buffer = (char*)vox_mpool_alloc(mpool, file_size + 1);
    if (!buffer) {
        fclose(file);
        vox_mpool_destroy(mpool);
        return;
    }
    
    size_t read_size = fread(buffer, 1, file_size, file);
    buffer[read_size] = '\0';
    fclose(file);
    
    printf("从文件读取 %zu 字节\n\n", read_size);
    
    /* 解析 TOML */
    vox_toml_err_info_t err_info;
    size_t parse_size = read_size;
    vox_toml_table_t* root = vox_toml_parse(mpool, buffer, &parse_size, &err_info);
    
    if (!root) {
        printf("解析错误: %s\n", err_info.message);
        printf("位置: 行 %d, 列 %d\n", err_info.line, err_info.column);
        vox_mpool_destroy(mpool);
        return;
    }
    
    printf("解析成功！\n\n");
    
    /* 读取配置 */
    vox_toml_elem_t* app_name = vox_toml_get_value(root, "app_name");
    if (app_name) {
        vox_strview_t name = vox_toml_get_string(app_name);
        printf("应用名称: ");
        print_strview("", &name);
        printf("\n");
    }
    
    vox_toml_table_t* db_table = vox_toml_find_subtable(root, "database");
    if (db_table) {
        vox_toml_elem_t* db_host = vox_toml_get_value(db_table, "host");
        if (db_host) {
            vox_strview_t host = vox_toml_get_string(db_host);
            printf("数据库主机: ");
            print_strview("", &host);
            printf("\n");
        }
    }
    
    /* 写入修改后的配置到新文件 */
    const char* output_file = "modified_config.toml";
    if (vox_toml_write_file(mpool, root, output_file) == 0) {
        printf("\n成功写入修改后的配置到: %s\n", output_file);
    } else {
        printf("\n写入文件失败: %s\n", output_file);
    }
    
    vox_mpool_destroy(mpool);
    printf("\n");
}

/* 示例12: 实际应用场景 - 应用配置管理 */
static void example_app_config(void) {
    printf("=== 示例 12: 应用配置管理 ===\n");
    
    const char* config_toml = 
        "# 应用配置\n"
        "app_name = \"Web Server\"\n"
        "version = \"2.1.0\"\n"
        "debug = false\n"
        "max_connections = 1000\n"
        "\n"
        "# 服务器配置\n"
        "[server]\n"
        "host = \"0.0.0.0\"\n"
        "port = 8080\n"
        "timeout = 30\n"
        "\n"
        "# 数据库配置\n"
        "[database]\n"
        "host = \"localhost\"\n"
        "port = 5432\n"
        "name = \"production\"\n"
        "pool_size = 20\n"
        "\n"
        "# 日志配置\n"
        "[logging]\n"
        "level = \"info\"\n"
        "file = \"/var/log/app.log\"\n"
        "max_size = 10485760  # 10MB\n"
        "\n"
        "# 缓存服务器\n"
        "[cache]\n"
        "enabled = true\n"
        "servers = [\"cache1.example.com\", \"cache2.example.com\"]\n"
        "ttl = 3600";
    
    printf("配置 TOML:\n%s\n", config_toml);
    
    vox_mpool_t* mpool = vox_mpool_create();
    if (!mpool) {
        fprintf(stderr, "创建内存池失败\n");
        return;
    }
    
    vox_toml_err_info_t err_info;
    vox_toml_table_t* config = vox_toml_parse_str(mpool, config_toml, &err_info);
    
    if (!config) {
        printf("解析错误: %s\n", err_info.message);
        vox_mpool_destroy(mpool);
        return;
    }
    
    printf("\n解析成功！读取配置：\n\n");
    
    /* 读取应用信息 */
    vox_toml_elem_t* app_name = vox_toml_get_value(config, "app_name");
    if (app_name) {
        vox_strview_t name = vox_toml_get_string(app_name);
        printf("应用名称: ");
        print_strview("", &name);
        printf("\n");
    }
    
    vox_toml_elem_t* max_conn = vox_toml_get_value(config, "max_connections");
    if (max_conn) {
        printf("最大连接数: %lld\n", (long long)vox_toml_get_integer(max_conn));
    }
    
    /* 读取服务器配置 */
    vox_toml_table_t* server = vox_toml_find_subtable(config, "server");
    if (server) {
        printf("\n服务器配置:\n");
        vox_toml_keyvalue_t* kv = vox_toml_table_first_keyvalue(server);
        while (kv) {
            printf("  ");
            print_strview("", &kv->key);
            printf(" = ");
            
            vox_toml_type_t type = vox_toml_get_type(kv->value);
            if (type == VOX_TOML_STRING) {
                vox_strview_t sv = vox_toml_get_string(kv->value);
                print_strview("", &sv);
            } else if (type == VOX_TOML_INTEGER) {
                printf("%lld", (long long)vox_toml_get_integer(kv->value));
            }
            printf("\n");
            
            kv = vox_toml_table_next_keyvalue(kv);
        }
    }
    
    /* 读取缓存配置 */
    vox_toml_table_t* cache = vox_toml_find_subtable(config, "cache");
    if (cache) {
        printf("\n缓存配置:\n");
        vox_toml_elem_t* enabled = vox_toml_get_value(cache, "enabled");
        if (enabled) {
            printf("  启用: %s\n", vox_toml_get_boolean(enabled) ? "是" : "否");
        }
        
        vox_toml_elem_t* servers = vox_toml_get_value(cache, "servers");
        if (servers && vox_toml_is_type(servers, VOX_TOML_ARRAY)) {
            printf("  服务器列表:\n");
            size_t count = vox_toml_get_array_count(servers);
            for (size_t i = 0; i < count; i++) {
                vox_toml_elem_t* server_elem = vox_toml_get_array_elem(servers, i);
                if (server_elem) {
                    vox_strview_t sv = vox_toml_get_string(server_elem);
                    printf("    [%zu] ", i);
                    print_strview("", &sv);
                    printf("\n");
                }
            }
        }
    }
    
    vox_mpool_destroy(mpool);
    printf("\n");
}

/* 示例13: 序列化往返测试 */
static void example_roundtrip(void) {
    printf("=== 示例 13: 序列化往返测试 ===\n");
    
    const char* original = 
        "title = \"测试配置\"\n"
        "numbers = [1, 2, 3]\n"
        "point = { x = 10, y = 20 }\n"
        "[server]\n"
        "host = \"localhost\"\n"
        "port = 8080";
    
    printf("原始 TOML:\n%s\n\n", original);
    
    vox_mpool_t* mpool = vox_mpool_create();
    if (!mpool) {
        fprintf(stderr, "创建内存池失败\n");
        return;
    }
    
    /* 解析 */
    vox_toml_table_t* root1 = vox_toml_parse_str(mpool, original, NULL);
    if (!root1) {
        printf("解析失败\n");
        vox_mpool_destroy(mpool);
        return;
    }
    
    /* 序列化 */
    size_t size = 0;
    char* serialized = vox_toml_to_string(mpool, root1, &size);
    if (!serialized) {
        printf("序列化失败\n");
        vox_mpool_destroy(mpool);
        return;
    }
    
    printf("序列化结果 (%zu 字节):\n%s\n\n", size, serialized);
    
    /* 再次解析 */
    vox_toml_table_t* root2 = vox_toml_parse_str(mpool, serialized, NULL);
    if (!root2) {
        printf("再次解析失败\n");
        vox_mpool_destroy(mpool);
        return;
    }
    
    /* 验证数据一致性 */
    bool all_match = true;
    
    /* 验证 title */
    vox_toml_elem_t* title1 = vox_toml_get_value(root1, "title");
    vox_toml_elem_t* title2 = vox_toml_get_value(root2, "title");
    if (title1 && title2) {
        vox_strview_t t1 = vox_toml_get_string(title1);
        vox_strview_t t2 = vox_toml_get_string(title2);
        if (t1.len != t2.len || memcmp(t1.ptr, t2.ptr, t1.len) != 0) {
            printf("✗ title 不一致\n");
            printf("  原始: ");
            print_strview("", &t1);
            printf("\n");
            printf("  解析: ");
            print_strview("", &t2);
            printf("\n");
            all_match = false;
        }
    } else if (title1 != title2) {
        printf("✗ title 存在性不一致\n");
        all_match = false;
    }
    
    /* 验证 numbers 数组 */
    vox_toml_elem_t* numbers1 = vox_toml_get_value(root1, "numbers");
    vox_toml_elem_t* numbers2 = vox_toml_get_value(root2, "numbers");
    if (numbers1 && numbers2) {
        size_t count1 = vox_toml_get_array_count(numbers1);
        size_t count2 = vox_toml_get_array_count(numbers2);
        if (count1 != count2) {
            printf("✗ numbers 数组长度不一致: %zu vs %zu\n", count1, count2);
            all_match = false;
        } else {
            for (size_t i = 0; i < count1; i++) {
                vox_toml_elem_t* e1 = vox_toml_get_array_elem(numbers1, i);
                vox_toml_elem_t* e2 = vox_toml_get_array_elem(numbers2, i);
                if (e1 && e2) {
                    int64_t v1 = vox_toml_get_integer(e1);
                    int64_t v2 = vox_toml_get_integer(e2);
                    if (v1 != v2) {
                        printf("✗ numbers[%zu] 不一致: %lld vs %lld\n", i, (long long)v1, (long long)v2);
                        all_match = false;
                    }
                }
            }
        }
    } else if (numbers1 != numbers2) {
        printf("✗ numbers 数组存在性不一致\n");
        all_match = false;
    }
    
    /* 验证 point 内联表 */
    vox_toml_elem_t* point1 = vox_toml_get_value(root1, "point");
    vox_toml_elem_t* point2 = vox_toml_get_value(root2, "point");
    if (point1 && point2) {
        vox_toml_elem_t* x1 = vox_toml_get_inline_table_value(point1, "x");
        vox_toml_elem_t* x2 = vox_toml_get_inline_table_value(point2, "x");
        if (x1 && x2) {
            int64_t v1 = vox_toml_get_integer(x1);
            int64_t v2 = vox_toml_get_integer(x2);
            if (v1 != v2) {
                printf("✗ point.x 不一致: %lld vs %lld\n", (long long)v1, (long long)v2);
                all_match = false;
            }
        }
        
        vox_toml_elem_t* y1 = vox_toml_get_inline_table_value(point1, "y");
        vox_toml_elem_t* y2 = vox_toml_get_inline_table_value(point2, "y");
        if (y1 && y2) {
            int64_t v1 = vox_toml_get_integer(y1);
            int64_t v2 = vox_toml_get_integer(y2);
            if (v1 != v2) {
                printf("✗ point.y 不一致: %lld vs %lld\n", (long long)v1, (long long)v2);
                all_match = false;
            }
        }
    } else if (point1 != point2) {
        printf("✗ point 内联表存在性不一致\n");
        all_match = false;
    }
    
    /* 验证 server 表 */
    vox_toml_table_t* server1 = vox_toml_find_subtable(root1, "server");
    vox_toml_table_t* server2 = vox_toml_find_subtable(root2, "server");
    if (server1 && server2) {
        vox_toml_elem_t* host1 = vox_toml_get_value(server1, "host");
        vox_toml_elem_t* host2 = vox_toml_get_value(server2, "host");
        if (host1 && host2) {
            vox_strview_t h1 = vox_toml_get_string(host1);
            vox_strview_t h2 = vox_toml_get_string(host2);
            if (h1.len != h2.len || memcmp(h1.ptr, h2.ptr, h1.len) != 0) {
                printf("✗ server.host 不一致\n");
                all_match = false;
            }
        }
        
        vox_toml_elem_t* port1 = vox_toml_get_value(server1, "port");
        vox_toml_elem_t* port2 = vox_toml_get_value(server2, "port");
        if (port1 && port2) {
            int64_t p1 = vox_toml_get_integer(port1);
            int64_t p2 = vox_toml_get_integer(port2);
            if (p1 != p2) {
                printf("✗ server.port 不一致: %lld vs %lld\n", (long long)p1, (long long)p2);
                all_match = false;
            }
        }
    } else if (server1 != server2) {
        printf("✗ server 表存在性不一致\n");
        all_match = false;
    }
    
    if (all_match) {
        printf("✓ 往返测试成功：所有数据一致性验证通过\n");
    } else {
        printf("✗ 往返测试失败：部分数据不一致（见上方详细信息）\n");
    }
    
    vox_mpool_destroy(mpool);
    printf("\n");
}

int main(void) {
    printf("========================================\n");
    printf("=== TOML 解析器示例 ===\n");
    printf("========================================\n\n");
    
    example_simple_keyvalues();
    example_arrays();
    example_tables();
    example_nested_tables();
    example_inline_tables();
    example_datetime();
    example_traverse_table();
    example_complex_config();
    example_comments();
    example_serialize();
    example_file_io();
    example_app_config();
    example_roundtrip();
    
    printf("========================================\n");
    printf("所有示例执行完成\n");
    printf("========================================\n");
    
    return 0;
}
