/*
 * db_coroutine_example.c - 数据库协程适配器示例
 *
 * 展示如何使用协程适配接口进行数据库操作，避免回调地狱
 * 
 * 特点：
 * - 使用 async/await 风格的协程API
 * - 代码更简洁易读，线性执行流程
 * - 支持事务处理
 * - 自动内存管理（数据从loop的内存池分配）
 */

#include "../vox_loop.h"
#include "../vox_log.h"
#include "../db/vox_db.h"
#include "../db/vox_db_pool.h"
#include "../coroutine/vox_coroutine_db.h"
#include "../coroutine/vox_coroutine.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* 打印行数据的辅助函数 */
static void print_row(const vox_db_row_t* row) {
    if (!row) return;
    
    printf("  Row: ");
    for (size_t i = 0; i < row->column_count; i++) {
        const char* name = row->column_names && row->column_names[i] ? row->column_names[i] : "?";
        const vox_db_value_t* v = &row->values[i];
        
        printf("%s=", name);
        switch (v->type) {
            case VOX_DB_TYPE_NULL:
                printf("NULL");
                break;
            case VOX_DB_TYPE_I64:
                printf("%lld", (long long)v->u.i64);
                break;
            case VOX_DB_TYPE_U64:
                printf("%llu", (unsigned long long)v->u.u64);
                break;
            case VOX_DB_TYPE_F64:
                printf("%.2f", v->u.f64);
                break;
            case VOX_DB_TYPE_BOOL:
                printf("%s", v->u.boolean ? "true" : "false");
                break;
            case VOX_DB_TYPE_TEXT:
                printf("'%.*s'", (int)v->u.text.len, v->u.text.ptr ? v->u.text.ptr : "");
                break;
            case VOX_DB_TYPE_BLOB:
                printf("<blob %zu bytes>", v->u.blob.len);
                break;
            default:
                printf("?");
                break;
        }
        if (i + 1 < row->column_count) printf(", ");
    }
    printf("\n");
}

/* 示例1: 基本的数据库操作（创建表、插入、查询） */
VOX_COROUTINE_ENTRY(basic_db_operations, vox_db_conn_t*) {
    vox_db_conn_t* db = (vox_db_conn_t*)user_data;
    printf("\n=== 示例1: 基本数据库操作 ===\n");
    
    /* 1. 创建表 */
    printf("1. 创建表 users...\n");
    int64_t affected = 0;
    int status = vox_coroutine_db_exec_await(co, db,
        "CREATE TABLE IF NOT EXISTS users(id INTEGER PRIMARY KEY, name TEXT, age INTEGER);",
        NULL, 0, &affected);
    if (status != 0) {
        printf("  失败: %s\n", vox_db_last_error(db) ? vox_db_last_error(db) : "unknown error");
        return;
    }
    printf("  成功\n");
    
    /* 2. 插入数据（显式 id 以兼容 SQLite/DuckDB，DuckDB 无自增时需提供 id） */
    printf("2. 插入数据...\n");
    vox_db_value_t params[3];
    
    /* 插入第一条记录 */
    params[0].type = VOX_DB_TYPE_I64;
    params[0].u.i64 = 1;
    params[1].type = VOX_DB_TYPE_TEXT;
    params[1].u.text.ptr = "Alice";
    params[1].u.text.len = 5;
    params[2].type = VOX_DB_TYPE_I64;
    params[2].u.i64 = 25;
    
    status = vox_coroutine_db_exec_await(co, db,
        "INSERT INTO users(id, name, age) VALUES(?, ?, ?);",
        params, 3, &affected);
    if (status != 0) {
        printf("  插入失败: %s\n", vox_db_last_error(db) ? vox_db_last_error(db) : "unknown error");
        return;
    }
    printf("  插入成功，影响行数: %lld\n", (long long)affected);
    
    /* 插入第二条记录 */
    params[0].u.i64 = 2;
    params[1].u.text.ptr = "Bob";
    params[1].u.text.len = 3;
    params[2].u.i64 = 30;
    
    status = vox_coroutine_db_exec_await(co, db,
        "INSERT INTO users(id, name, age) VALUES(?, ?, ?);",
        params, 3, &affected);
    if (status != 0) {
        printf("  插入失败: %s\n", vox_db_last_error(db) ? vox_db_last_error(db) : "unknown error");
        return;
    }
    printf("  插入成功，影响行数: %lld\n", (long long)affected);
    
    /* 3. 查询数据 */
    printf("3. 查询所有用户...\n");
    vox_db_row_t* rows = NULL;
    int64_t row_count = 0;
    
    status = vox_coroutine_db_query_await(co, db,
        "SELECT id, name, age FROM users ORDER BY id;",
        NULL, 0, &rows, &row_count);
    if (status != 0) {
        printf("  查询失败: %s\n", vox_db_last_error(db) ? vox_db_last_error(db) : "unknown error");
        return;
    }
    
    printf("  查询成功，共 %lld 行:\n", (long long)row_count);
    if (rows) {
        for (int64_t i = 0; i < row_count; i++) {
            print_row(&rows[i]);
        }
    }
    
    printf("示例1完成\n");
}

/* 示例2: 使用事务处理多个操作 */
VOX_COROUTINE_ENTRY(transaction_example, vox_db_conn_t*) {
    vox_db_conn_t* db = (vox_db_conn_t*)user_data;
    printf("\n=== 示例2: 事务处理 ===\n");
    
    /* 开始事务 */
    printf("1. 开始事务...\n");
    int status = vox_coroutine_db_begin_transaction_await(co, db);
    if (status != 0) {
        printf("  开始事务失败: %s\n", vox_db_last_error(db) ? vox_db_last_error(db) : "unknown error");
        return;
    }
    printf("  事务已开始\n");
    
    /* 在事务中执行多个操作 */
    printf("2. 在事务中执行多个操作...\n");
    vox_db_value_t params[3];
    int64_t affected = 0;
    
    /* 操作1: 插入（id=3，示例1已插入 1、2） */
    params[0].type = VOX_DB_TYPE_I64;
    params[0].u.i64 = 3;
    params[1].type = VOX_DB_TYPE_TEXT;
    params[1].u.text.ptr = "Charlie";
    params[1].u.text.len = 7;
    params[2].type = VOX_DB_TYPE_I64;
    params[2].u.i64 = 28;
    
    status = vox_coroutine_db_exec_await(co, db,
        "INSERT INTO users(id, name, age) VALUES(?, ?, ?);",
        params, 3, &affected);
    if (status != 0) {
        printf("  插入失败，回滚事务: %s\n", vox_db_last_error(db) ? vox_db_last_error(db) : "unknown error");
        vox_coroutine_db_rollback_await(co, db);
        return;
    }
    printf("  插入成功，影响行数: %lld\n", (long long)affected);
    
    /* 操作2: 更新 */
    params[0].type = VOX_DB_TYPE_TEXT;
    params[0].u.text.ptr = "Alice Updated";
    params[0].u.text.len = 13;
    params[1].type = VOX_DB_TYPE_I64;
    params[1].u.i64 = 26;
    
    status = vox_coroutine_db_exec_await(co, db,
        "UPDATE users SET name=?, age=? WHERE name='Alice';",
        params, 2, &affected);
    if (status != 0) {
        printf("  更新失败，回滚事务: %s\n", vox_db_last_error(db) ? vox_db_last_error(db) : "unknown error");
        vox_coroutine_db_rollback_await(co, db);
        return;
    }
    printf("  更新成功，影响行数: %lld\n", (long long)affected);
    
    /* 提交事务 */
    printf("3. 提交事务...\n");
    status = vox_coroutine_db_commit_await(co, db);
    if (status != 0) {
        printf("  提交失败: %s\n", vox_db_last_error(db) ? vox_db_last_error(db) : "unknown error");
        vox_coroutine_db_rollback_await(co, db);
        return;
    }
    printf("  事务已提交\n");
    
    /* 验证结果 */
    printf("4. 验证结果...\n");
    vox_db_row_t* rows = NULL;
    int64_t row_count = 0;
    
    status = vox_coroutine_db_query_await(co, db,
        "SELECT id, name, age FROM users ORDER BY id;",
        NULL, 0, &rows, &row_count);
    if (status == 0 && rows) {
        printf("  当前用户列表（共 %lld 行）:\n", (long long)row_count);
        for (int64_t i = 0; i < row_count; i++) {
            print_row(&rows[i]);
        }
    }
    
    printf("示例2完成\n");
}

/* 示例3: 事务回滚演示 */
VOX_COROUTINE_ENTRY(rollback_example, vox_db_conn_t*) {
    vox_db_conn_t* db = (vox_db_conn_t*)user_data;
    printf("\n=== 示例3: 事务回滚演示 ===\n");
    
    /* 开始事务 */
    printf("1. 开始事务...\n");
    int status = vox_coroutine_db_begin_transaction_await(co, db);
    if (status != 0) {
        printf("  开始事务失败\n");
        return;
    }
    printf("  事务已开始\n");
    
    /* 执行一个操作（id=4，示例1/2已插入 1、2、3） */
    printf("2. 插入数据...\n");
    vox_db_value_t params[3];
    params[0].type = VOX_DB_TYPE_I64;
    params[0].u.i64 = 4;
    params[1].type = VOX_DB_TYPE_TEXT;
    params[1].u.text.ptr = "Test User";
    params[1].u.text.len = 9;
    params[2].type = VOX_DB_TYPE_I64;
    params[2].u.i64 = 99;
    
    int64_t affected = 0;
    status = vox_coroutine_db_exec_await(co, db,
        "INSERT INTO users(id, name, age) VALUES(?, ?, ?);",
        params, 3, &affected);
    if (status != 0) {
        printf("  插入失败\n");
        vox_coroutine_db_rollback_await(co, db);
        return;
    }
    printf("  插入成功，影响行数: %lld\n", (long long)affected);
    
    /* 模拟错误，回滚事务 */
    printf("3. 模拟错误，回滚事务...\n");
    status = vox_coroutine_db_rollback_await(co, db);
    if (status != 0) {
        printf("  回滚失败\n");
        return;
    }
    printf("  事务已回滚\n");
    
    /* 验证数据未插入 */
    printf("4. 验证数据未插入（应该找不到 'Test User'）...\n");
    vox_db_row_t* rows = NULL;
    int64_t row_count = 0;
    
    params[0].u.text.ptr = "Test User";
    params[0].u.text.len = 9;
    
    status = vox_coroutine_db_query_await(co, db,
        "SELECT id, name, age FROM users WHERE name=?;",
        params, 1, &rows, &row_count);
    if (status == 0) {
        if (row_count == 0) {
            printf("  验证成功：数据未插入（回滚生效）\n");
        } else {
            printf("  验证失败：数据已插入（回滚未生效）\n");
        }
    }
    
    printf("示例3完成\n");
}

/* 示例4: 使用连接池（acquire -> 单连接多次操作 -> release，与 vox_coroutine_redis 一致） */
VOX_COROUTINE_ENTRY(pool_example, vox_db_pool_t*) {
    vox_db_pool_t* pool = (vox_db_pool_t*)user_data;
    vox_db_conn_t* conn = NULL;
    printf("\n=== 示例4: 连接池并发操作 ===\n");

    if (vox_coroutine_db_pool_acquire_await(co, pool, &conn) != 0) {
        printf("  从连接池获取连接失败\n");
        return;
    }

    /* 1. 创建表 */
    printf("1. 使用连接池创建表 products...\n");
    int64_t affected = 0;
    int status = vox_coroutine_db_exec_await(co, conn,
        "CREATE TABLE IF NOT EXISTS products(id INTEGER PRIMARY KEY, name TEXT, price REAL, stock INTEGER);",
        NULL, 0, &affected);
    if (status != 0) {
        printf("  创建表失败\n");
        vox_db_pool_release(pool, conn);
        return;
    }
    printf("  创建表成功\n");

    /* 2. 批量插入数据（显式 id 以兼容 DuckDB） */
    printf("2. 使用连接池批量插入产品数据...\n");
    vox_db_value_t params[4];
    int success_count = 0;

    const char* products[][2] = {
        {"Laptop", "999.99"},
        {"Mouse", "29.99"},
        {"Keyboard", "79.99"},
        {"Monitor", "299.99"},
        {"Headphones", "149.99"}
    };
    const int stock[] = {10, 50, 30, 15, 25};

    for (int i = 0; i < 5; i++) {
        params[0].type = VOX_DB_TYPE_I64;
        params[0].u.i64 = (int64_t)(i + 1);
        params[1].type = VOX_DB_TYPE_TEXT;
        params[1].u.text.ptr = products[i][0];
        params[1].u.text.len = strlen(products[i][0]);
        params[2].type = VOX_DB_TYPE_F64;
        params[2].u.f64 = atof(products[i][1]);
        params[3].type = VOX_DB_TYPE_I64;
        params[3].u.i64 = stock[i];

        status = vox_coroutine_db_exec_await(co, conn,
            "INSERT INTO products(id, name, price, stock) VALUES(?, ?, ?, ?);",
            params, 4, &affected);
        if (status == 0) {
            success_count++;
            printf("  插入产品 '%s' 成功\n", products[i][0]);
        } else {
            printf("  插入产品 '%s' 失败\n", products[i][0]);
        }
    }
    printf("  批量插入完成，成功 %d/5\n", success_count);

    /* 3. 查询所有产品 */
    printf("3. 使用连接池查询所有产品...\n");
    vox_db_row_t* rows = NULL;
    int64_t row_count = 0;

    status = vox_coroutine_db_query_await(co, conn,
        "SELECT id, name, price, stock FROM products ORDER BY id;",
        NULL, 0, &rows, &row_count);
    if (status != 0) {
        printf("  查询失败\n");
        vox_db_pool_release(pool, conn);
        return;
    }

    printf("  查询成功，共 %lld 个产品:\n", (long long)row_count);
    if (rows) {
        for (int64_t i = 0; i < row_count; i++) {
            print_row(&rows[i]);
        }
    }

    /* 4. 更新库存 */
    printf("4. 使用连接池更新产品库存...\n");
    params[0].type = VOX_DB_TYPE_I64;
    params[0].u.i64 = 5;  /* 新库存 */
    params[1].type = VOX_DB_TYPE_TEXT;
    params[1].u.text.ptr = "Laptop";
    params[1].u.text.len = 6;

    status = vox_coroutine_db_exec_await(co, conn,
        "UPDATE products SET stock=? WHERE name=?;",
        params, 2, &affected);
    if (status == 0) {
        printf("  更新成功，影响行数: %lld\n", (long long)affected);
    } else {
        printf("  更新失败\n");
    }

    /* 5. 再次查询验证更新 */
    printf("5. 验证更新结果...\n");
    params[0].type = VOX_DB_TYPE_TEXT;
    params[0].u.text.ptr = "Laptop";
    params[0].u.text.len = 6;

    rows = NULL;
    row_count = 0;
    status = vox_coroutine_db_query_await(co, conn,
        "SELECT id, name, price, stock FROM products WHERE name=?;",
        params, 1, &rows, &row_count);
    if (status == 0 && rows && row_count > 0) {
        printf("  查询结果:\n");
        print_row(&rows[0]);
        if (rows[0].values[3].type == VOX_DB_TYPE_I64 && rows[0].values[3].u.i64 == 5) {
            printf("  验证成功：库存已更新为 5\n");
        } else {
            printf("  验证失败：库存未正确更新\n");
        }
    }

    vox_db_pool_release(pool, conn);
    printf("示例4完成\n");
}

/* 主协程：依次执行所有示例 */
VOX_COROUTINE_ENTRY(main_coroutine, void* user_data) {
    typedef struct {
        vox_loop_t* loop;
        vox_db_conn_t* db;
        vox_db_pool_t* pool;
    } app_data_t;
    
    app_data_t* app = (app_data_t*)user_data;
    if (!app || !app->db) {
        printf("错误：无效的应用数据\n");
        return;
    }
    
    printf("========================================\n");
    printf("数据库协程适配器示例\n");
    printf("========================================\n");
    
    /* 执行示例1 */
    basic_db_operations(co, app->db);
    
    /* 执行示例2 */
    transaction_example(co, app->db);
    
    /* 执行示例3 */
    rollback_example(co, app->db);
    
    /* 执行示例4（使用连接池） */
    if (app->pool) {
        pool_example(co, app->pool);
    }
    
    printf("\n========================================\n");
    printf("所有示例执行完成\n");
    printf("========================================\n");
    
    /* 停止事件循环 */
    vox_loop_stop(app->loop);
}

int main(void) {
    vox_log_set_level(VOX_LOG_INFO);
    
    /* 创建事件循环 */
    vox_loop_t* loop = vox_loop_create();
    if (!loop) {
        fprintf(stderr, "vox_loop_create failed\n");
        return 1;
    }
    
    /* 连接数据库（优先使用SQLite3，如果未启用则尝试其他驱动） */
    vox_db_conn_t* db = NULL;
    
    /* 尝试SQLite3 */
    db = vox_db_connect(loop, VOX_DB_DRIVER_SQLITE3, ":memory:");
    if (!db) {
        /* 尝试DuckDB */
        db = vox_db_connect(loop, VOX_DB_DRIVER_DUCKDB, ":memory:");
    }
    
    if (!db) {
        fprintf(stderr, "无法连接到数据库。请确保至少启用了一个数据库驱动（SQLite3或DuckDB）\n");
        vox_loop_destroy(loop);
        return 1;
    }
    
    printf("数据库连接成功\n");
    
    /* 创建连接池（用于示例4） */
    vox_db_pool_t* pool = NULL;
    
    /* 尝试使用SQLite3创建连接池 */
    pool = vox_db_pool_create_ex(loop, VOX_DB_DRIVER_SQLITE3, ":memory:", 2, 5);
    if (!pool) {
        /* 尝试使用DuckDB创建连接池 */
        pool = vox_db_pool_create_ex(loop, VOX_DB_DRIVER_DUCKDB, ":memory:", 2, 5);
    }
    
    if (pool) {
        printf("连接池创建成功（初始连接数: 2, 最大连接数: 5）\n");
    } else {
        printf("连接池创建失败（将跳过示例4）\n");
    }
    
    /* 准备应用数据 */
    typedef struct {
        vox_loop_t* loop;
        vox_db_conn_t* db;
        vox_db_pool_t* pool;
    } app_data_t;
    
    app_data_t app = {
        .loop = loop,
        .db = db,
        .pool = pool
    };
    
    /* 启动主协程 */
    printf("启动协程...\n");
    VOX_COROUTINE_START(loop, main_coroutine, &app);
    
    /* 运行事件循环 */
    printf("运行事件循环...\n");
    vox_loop_run(loop, VOX_RUN_DEFAULT);
    
    /* 清理 */
    vox_db_disconnect(db);
    if (pool) {
        vox_db_pool_destroy(pool);
    }
    vox_loop_destroy(loop);
    
    printf("程序退出\n");
    return 0;
}
