/*
 * db_async_complex_example.c - 复杂场景下的多个异步操作处理
 *
 * 本示例展示了处理多个异步操作的几种模式：
 * 1. 状态机模式 - 使用 phase/state 跟踪操作序列
 * 2. 回调链模式 - 每个操作完成后触发下一个
 * 3. 并行操作模式 - 多个操作并行执行，等待全部完成
 * 4. 错误处理和资源清理 - 统一的错误处理机制
 *
 * 适用场景：
 * - 需要执行多个相关的数据库操作
 * - 操作之间有依赖关系（顺序执行）
 * - 需要并行执行多个独立操作
 * - 需要统一的错误处理和资源清理
 */

#include "../vox_loop.h"
#include "../vox_log.h"
#include "../vox_mutex.h"
#include "../db/vox_db.h"
#include "../db/vox_db_pool.h"

#include <stdio.h>
#include <string.h>

/* ===== 模式1：状态机模式（适合顺序操作） ===== */

typedef enum {
    ASYNC_STATE_INIT = 0,
    ASYNC_STATE_CREATE_USERS_TABLE,
    ASYNC_STATE_CREATE_PROFILES_TABLE,
    ASYNC_STATE_GET_NEXT_USER_ID,
    ASYNC_STATE_INSERT_USER,
    ASYNC_STATE_GET_NEXT_PROFILE_ID,
    ASYNC_STATE_INSERT_PROFILE,
    ASYNC_STATE_QUERY_USER,
    ASYNC_STATE_COMPLETE,
    ASYNC_STATE_ERROR
} async_state_t;

typedef struct {
    vox_loop_t* loop;
    vox_db_conn_t* db;
    async_state_t state;
    int error_code;
    const char* error_msg;
    
    /* 操作结果 */
    int64_t user_id;
    int64_t profile_id;
    int64_t query_row_count;
    
    /* 参数（需要在整个异步序列中保持有效） */
    vox_db_value_t user_params[4];  /* id, name, email, age */
    vox_db_value_t profile_params[3];  /* id, user_id, bio */
    vox_db_value_t query_params[1];  /* user_id for query */
} state_machine_t;

static void state_machine_next(state_machine_t* sm);
static void state_machine_on_exec(vox_db_conn_t* conn, int status, int64_t affected_rows, void* user_data);
static void state_machine_on_row(vox_db_conn_t* conn, const vox_db_row_t* row, void* user_data);
static void state_machine_on_done(vox_db_conn_t* conn, int status, int64_t row_count, void* user_data);

static void state_machine_start(vox_loop_t* loop, void* user_data) {
    (void)loop;
    state_machine_t* sm = (state_machine_t*)user_data;
    if (!sm) return;
    
    sm->state = ASYNC_STATE_CREATE_USERS_TABLE;
    state_machine_next(sm);
}

static void state_machine_next(state_machine_t* sm) {
    if (!sm || !sm->db) return;
    
    switch (sm->state) {
        case ASYNC_STATE_CREATE_USERS_TABLE: {
            printf("[状态机] 步骤1: 创建 users 表\n");
            /* 兼容 SQLite/DuckDB：不使用 AUTOINCREMENT，插入前用 SELECT MAX(id)+1 取下一 id */
            const char* sql = "CREATE TABLE IF NOT EXISTS users(id INTEGER PRIMARY KEY, name VARCHAR(50), email VARCHAR(100), age INTEGER);";
            if (vox_db_exec_async(sm->db, sql, NULL, 0, state_machine_on_exec, sm) != 0) {
                sm->state = ASYNC_STATE_ERROR;
                sm->error_code = -1;
                sm->error_msg = "创建 users 表失败";
            }
            break;
        }
        
        case ASYNC_STATE_CREATE_PROFILES_TABLE: {
            printf("[状态机] 步骤2: 创建 profiles 表\n");
            const char* sql = "CREATE TABLE IF NOT EXISTS profiles(id INTEGER PRIMARY KEY, user_id INTEGER, bio TEXT);";
            if (vox_db_exec_async(sm->db, sql, NULL, 0, state_machine_on_exec, sm) != 0) {
                sm->state = ASYNC_STATE_ERROR;
                sm->error_code = -1;
                sm->error_msg = "创建 profiles 表失败";
            }
            break;
        }
        
        case ASYNC_STATE_GET_NEXT_USER_ID: {
            printf("[状态机] 步骤3: 获取下一用户ID\n");
            const char* sql = "SELECT COALESCE(MAX(id),0)+1 FROM users;";
            if (vox_db_query_async(sm->db, sql, NULL, 0, state_machine_on_row, state_machine_on_done, sm) != 0) {
                sm->state = ASYNC_STATE_ERROR;
                sm->error_code = -1;
                sm->error_msg = "获取用户ID失败";
            }
            break;
        }
        
        case ASYNC_STATE_INSERT_USER: {
            printf("[状态机] 步骤4: 插入用户\n");
            sm->user_params[0].type = VOX_DB_TYPE_I64;
            sm->user_params[0].u.i64 = sm->user_id;
            sm->user_params[1].type = VOX_DB_TYPE_TEXT;
            sm->user_params[1].u.text.ptr = "Alice";
            sm->user_params[1].u.text.len = 5;
            sm->user_params[2].type = VOX_DB_TYPE_TEXT;
            sm->user_params[2].u.text.ptr = "alice@example.com";
            sm->user_params[2].u.text.len = 18;
            sm->user_params[3].type = VOX_DB_TYPE_I64;
            sm->user_params[3].u.i64 = 25;
            
            const char* sql = "INSERT INTO users(id, name, email, age) VALUES(?, ?, ?, ?);";
            if (vox_db_exec_async(sm->db, sql, sm->user_params, 4, state_machine_on_exec, sm) != 0) {
                sm->state = ASYNC_STATE_ERROR;
                sm->error_code = -1;
                sm->error_msg = "插入用户失败";
            }
            break;
        }
        
        case ASYNC_STATE_GET_NEXT_PROFILE_ID: {
            printf("[状态机] 步骤5: 获取下一资料ID\n");
            const char* sql = "SELECT COALESCE(MAX(id),0)+1 FROM profiles;";
            if (vox_db_query_async(sm->db, sql, NULL, 0, state_machine_on_row, state_machine_on_done, sm) != 0) {
                sm->state = ASYNC_STATE_ERROR;
                sm->error_code = -1;
                sm->error_msg = "获取资料ID失败";
            }
            break;
        }
        
        case ASYNC_STATE_INSERT_PROFILE: {
            printf("[状态机] 步骤6: 插入用户资料\n");
            sm->profile_params[0].type = VOX_DB_TYPE_I64;
            sm->profile_params[0].u.i64 = sm->profile_id;
            sm->profile_params[1].type = VOX_DB_TYPE_I64;
            sm->profile_params[1].u.i64 = sm->user_id;
            sm->profile_params[2].type = VOX_DB_TYPE_TEXT;
            sm->profile_params[2].u.text.ptr = "Software Engineer";
            sm->profile_params[2].u.text.len = 17;
            
            const char* sql = "INSERT INTO profiles(id, user_id, bio) VALUES(?, ?, ?);";
            if (vox_db_exec_async(sm->db, sql, sm->profile_params, 3, state_machine_on_exec, sm) != 0) {
                sm->state = ASYNC_STATE_ERROR;
                sm->error_code = -1;
                sm->error_msg = "插入资料失败";
            }
            break;
        }
        
        case ASYNC_STATE_QUERY_USER: {
            printf("[状态机] 步骤7: 查询用户 (user_id=%lld)\n", (long long)sm->user_id);
            const char* sql = "SELECT u.id, u.name, u.email, u.age, p.bio FROM users u LEFT JOIN profiles p ON u.id = p.user_id WHERE u.id = ?;";
            sm->query_params[0].type = VOX_DB_TYPE_I64;
            sm->query_params[0].u.i64 = sm->user_id;
            
            if (vox_db_query_async(sm->db, sql, sm->query_params, 1, NULL, state_machine_on_done, sm) != 0) {
                sm->state = ASYNC_STATE_ERROR;
                sm->error_code = -1;
                sm->error_msg = "查询失败";
            }
            break;
        }
        
        case ASYNC_STATE_COMPLETE: {
            printf("[状态机] 完成！用户ID: %lld, 资料ID: %lld, 查询行数: %lld\n",
                   (long long)sm->user_id, (long long)sm->profile_id, (long long)sm->query_row_count);
            vox_loop_stop(sm->loop);
            break;
        }
        
        case ASYNC_STATE_ERROR: {
            printf("[状态机] 错误: %s (code: %d)\n", sm->error_msg ? sm->error_msg : "未知错误", sm->error_code);
            vox_loop_stop(sm->loop);
            break;
        }
        
        default:
            break;
    }
}

static void state_machine_on_exec(vox_db_conn_t* conn, int status, int64_t affected_rows, void* user_data) {
    (void)conn;
    (void)affected_rows;
    state_machine_t* sm = (state_machine_t*)user_data;
    if (!sm) return;
    
    if (status != 0) {
        sm->state = ASYNC_STATE_ERROR;
        sm->error_code = status;
        sm->error_msg = vox_db_last_error(conn);
        state_machine_next(sm);
        return;
    }
    
    switch (sm->state) {
        case ASYNC_STATE_CREATE_USERS_TABLE:
            sm->state = ASYNC_STATE_CREATE_PROFILES_TABLE;
            break;
        case ASYNC_STATE_CREATE_PROFILES_TABLE:
            sm->state = ASYNC_STATE_GET_NEXT_USER_ID;
            break;
        case ASYNC_STATE_INSERT_USER:
            sm->state = ASYNC_STATE_GET_NEXT_PROFILE_ID;
            break;
        case ASYNC_STATE_INSERT_PROFILE:
            sm->state = ASYNC_STATE_QUERY_USER;
            break;
        default:
            break;
    }
    
    state_machine_next(sm);
}

static void state_machine_on_row(vox_db_conn_t* conn, const vox_db_row_t* row, void* user_data) {
    (void)conn;
    state_machine_t* sm = (state_machine_t*)user_data;
    if (!sm || !row || row->column_count < 1) return;
    
    if (row->values[0].type == VOX_DB_TYPE_I64) {
        if (sm->state == ASYNC_STATE_GET_NEXT_USER_ID) {
            sm->user_id = row->values[0].u.i64;
        } else if (sm->state == ASYNC_STATE_GET_NEXT_PROFILE_ID) {
            sm->profile_id = row->values[0].u.i64;
        }
    }
}

static void state_machine_on_done(vox_db_conn_t* conn, int status, int64_t row_count, void* user_data) {
    (void)conn;
    state_machine_t* sm = (state_machine_t*)user_data;
    if (!sm) return;
    
    if (status != 0) {
        sm->state = ASYNC_STATE_ERROR;
        sm->error_code = status;
        const char* err_msg = vox_db_last_error(conn);
        if (err_msg && strlen(err_msg) > 0 && strcmp(err_msg, "not an error") != 0) {
            sm->error_msg = err_msg;
        } else {
            sm->error_msg = "查询失败";
        }
    } else {
        if (sm->state == ASYNC_STATE_GET_NEXT_USER_ID) {
            if (sm->user_id > 0) {
                sm->state = ASYNC_STATE_INSERT_USER;
            } else {
                sm->state = ASYNC_STATE_ERROR;
                sm->error_code = -1;
                sm->error_msg = "获取用户ID失败：ID为0或无效";
            }
        } else if (sm->state == ASYNC_STATE_GET_NEXT_PROFILE_ID) {
            if (sm->profile_id > 0) {
                sm->state = ASYNC_STATE_INSERT_PROFILE;
            } else {
                sm->state = ASYNC_STATE_ERROR;
                sm->error_code = -1;
                sm->error_msg = "获取资料ID失败：ID为0或无效";
            }
        } else {
            /* 查询用户完成 */
            sm->query_row_count = row_count;
            sm->state = ASYNC_STATE_COMPLETE;
        }
    }
    
    state_machine_next(sm);
}

/* ===== 模式2：回调链模式（适合简单的顺序操作） ===== */

typedef struct {
    vox_loop_t* loop;
    vox_db_conn_t* db;
    int step;
    int error;
} callback_chain_t;

static void chain_step2(vox_db_conn_t* conn, int status, int64_t affected, void* user_data);
static void chain_step3(vox_db_conn_t* conn, int status, int64_t affected, void* user_data);
static void chain_complete(vox_db_conn_t* conn, int status, int64_t affected, void* user_data);

static void chain_step1(vox_db_conn_t* conn, int status, int64_t affected, void* user_data) {
    (void)conn;
    (void)affected;
    callback_chain_t* chain = (callback_chain_t*)user_data;
    if (!chain) return;
    
    if (status != 0) {
        printf("[回调链] 步骤1失败: %s\n", vox_db_last_error(conn) ? vox_db_last_error(conn) : "未知错误");
        chain->error = status;
        vox_loop_stop(chain->loop);
        return;
    }
    
    printf("[回调链] 步骤1完成，继续步骤2\n");
    chain->step = 2;
    
    const char* sql = "INSERT INTO users(id, name, email, age) VALUES(3, 'Bob', 'bob@example.com', 30);";
    vox_db_exec_async(chain->db, sql, NULL, 0, chain_step2, chain);
}

static void chain_step2(vox_db_conn_t* conn, int status, int64_t affected, void* user_data) {
    (void)conn;
    (void)affected;
    callback_chain_t* chain = (callback_chain_t*)user_data;
    if (!chain) return;
    
    if (status != 0) {
        printf("[回调链] 步骤2失败: %s\n", vox_db_last_error(conn) ? vox_db_last_error(conn) : "未知错误");
        chain->error = status;
        vox_loop_stop(chain->loop);
        return;
    }
    
    printf("[回调链] 步骤2完成，继续步骤3\n");
    chain->step = 3;
    
    const char* sql = "INSERT INTO users(id, name, email, age) VALUES(4, 'Charlie', 'charlie@example.com', 35);";
    vox_db_exec_async(chain->db, sql, NULL, 0, chain_step3, chain);
}

static void chain_step3(vox_db_conn_t* conn, int status, int64_t affected, void* user_data) {
    (void)conn;
    callback_chain_t* chain = (callback_chain_t*)user_data;
    if (!chain) return;
    
    if (status != 0) {
        printf("[回调链] 步骤3失败: %s\n", vox_db_last_error(conn) ? vox_db_last_error(conn) : "未知错误");
        chain->error = status;
        vox_loop_stop(chain->loop);
        return;
    }
    
    printf("[回调链] 步骤3完成，完成链式操作\n");
    chain->step = 4;
    
    /* 完成 */
    chain_complete(conn, status, affected, chain);
}

static void chain_complete(vox_db_conn_t* conn, int status, int64_t affected, void* user_data) {
    (void)conn;
    (void)affected;
    (void)status;
    callback_chain_t* chain = (callback_chain_t*)user_data;
    if (!chain) return;
    
    printf("[回调链] 所有步骤完成！\n");
    vox_loop_stop(chain->loop);
}

static void start_chain(vox_loop_t* l, void* data) {
    (void)l;
    callback_chain_t* c = (callback_chain_t*)data;
    /* 表已由测试1创建；使用显式 id 以兼容 SQLite/DuckDB（无自增时需提供 id） */
    const char* sql = "INSERT INTO users(id, name, email, age) VALUES(2, 'Alice', 'alice@example.com', 25);";
    vox_db_exec_async(c->db, sql, NULL, 0, chain_step1, c);
}

/* ===== 模式3：并行操作模式（等待多个操作全部完成） ===== */
/* 注意：单个数据库连接不支持并发操作，需要使用连接池来实现真正的并行 */

typedef struct {
    vox_loop_t* loop;
    vox_db_pool_t* pool;  /* 使用连接池支持并行操作 */
    int pending_count;  /* 待完成的操作数 */
    int completed_count;  /* 已完成的操作数 */
    int error_count;  /* 失败的操作数 */
    int init_count;  /* 初始化（创建表）完成数 */
    int pool_size;  /* 连接池大小 */
    vox_mutex_t mutex;  /* 保护计数器 */
} parallel_ops_t;

static void parallel_on_complete_pool(vox_db_conn_t* conn, int status, int64_t affected, void* user_data) {
    (void)conn;
    (void)affected;
    parallel_ops_t* ops = (parallel_ops_t*)user_data;
    if (!ops) return;
    
    vox_mutex_lock(&ops->mutex);
    ops->completed_count++;
    if (status != 0) {
        ops->error_count++;
        const char* err_msg = vox_db_last_error(conn);
        if (err_msg && strlen(err_msg) > 0 && strcmp(err_msg, "not an error") != 0) {
            printf("[并行操作] 一个操作失败: %s\n", err_msg);
        } else {
            printf("[并行操作] 一个操作失败: 错误代码 %d\n", status);
        }
    }
    
    /* 检查是否全部完成 */
    if (ops->completed_count >= ops->pending_count) {
        printf("[并行操作] 所有操作完成！成功: %d, 失败: %d\n",
               ops->completed_count - ops->error_count, ops->error_count);
        vox_mutex_unlock(&ops->mutex);
        vox_loop_stop(ops->loop);
    } else {
        vox_mutex_unlock(&ops->mutex);
    }
}

static void parallel_start_inserts(parallel_ops_t* ops) {
    if (!ops || !ops->pool) return;
    
    printf("[并行操作] 启动3个并行插入操作\n");
    
    /* 并行执行3个插入操作 */
    ops->pending_count = 3;
    ops->completed_count = 0;
    ops->error_count = 0;
    
    /* 参数需要在整个异步操作期间保持有效；id 显式传入以兼容 SQLite/DuckDB */
    static vox_db_value_t params1[4], params2[4], params3[4];
    
    params1[0].type = VOX_DB_TYPE_I64;
    params1[0].u.i64 = 1;
    params1[1].type = VOX_DB_TYPE_TEXT;
    params1[1].u.text.ptr = "David";
    params1[1].u.text.len = 5;
    params1[2].type = VOX_DB_TYPE_TEXT;
    params1[2].u.text.ptr = "david@example.com";
    params1[2].u.text.len = 17;
    params1[3].type = VOX_DB_TYPE_I64;
    params1[3].u.i64 = 28;
    vox_db_pool_exec_async(ops->pool, "INSERT INTO users(id, name, email, age) VALUES(?, ?, ?, ?);",
                          params1, 4, parallel_on_complete_pool, ops);
    
    params2[0].type = VOX_DB_TYPE_I64;
    params2[0].u.i64 = 2;
    params2[1].type = VOX_DB_TYPE_TEXT;
    params2[1].u.text.ptr = "Eve";
    params2[1].u.text.len = 3;
    params2[2].type = VOX_DB_TYPE_TEXT;
    params2[2].u.text.ptr = "eve@example.com";
    params2[2].u.text.len = 15;
    params2[3].type = VOX_DB_TYPE_I64;
    params2[3].u.i64 = 32;
    vox_db_pool_exec_async(ops->pool, "INSERT INTO users(id, name, email, age) VALUES(?, ?, ?, ?);",
                          params2, 4, parallel_on_complete_pool, ops);
    
    params3[0].type = VOX_DB_TYPE_I64;
    params3[0].u.i64 = 3;
    params3[1].type = VOX_DB_TYPE_TEXT;
    params3[1].u.text.ptr = "Frank";
    params3[1].u.text.len = 5;
    params3[2].type = VOX_DB_TYPE_TEXT;
    params3[2].u.text.ptr = "frank@example.com";
    params3[2].u.text.len = 17;
    params3[3].type = VOX_DB_TYPE_I64;
    params3[3].u.i64 = 29;
    vox_db_pool_exec_async(ops->pool, "INSERT INTO users(id, name, email, age) VALUES(?, ?, ?, ?);",
                          params3, 4, parallel_on_complete_pool, ops);
}

static void parallel_on_init_complete(vox_db_conn_t* conn, int status, int64_t affected, void* user_data) {
    (void)conn;
    (void)affected;
    parallel_ops_t* ops = (parallel_ops_t*)user_data;
    if (!ops) return;
    
    vox_mutex_lock(&ops->mutex);
    ops->init_count++;
    if (status != 0) {
        const char* err_msg = vox_db_last_error(conn);
        printf("[并行操作] 初始化失败: %s\n", err_msg ? err_msg : "未知错误");
    }
    
    /* 检查是否所有连接都初始化完成 */
    if (ops->init_count >= ops->pool_size) {
        vox_mutex_unlock(&ops->mutex);
        /* 所有连接都初始化完成，开始执行插入操作 */
        parallel_start_inserts(ops);
    } else {
        vox_mutex_unlock(&ops->mutex);
    }
}

static void parallel_ops_start(vox_loop_t* loop, void* user_data) {
    (void)loop;
    parallel_ops_t* ops = (parallel_ops_t*)user_data;
    if (!ops || !ops->pool) return;
    
    printf("[并行操作] 初始化连接池（在每个连接中创建表）\n");
    
    ops->init_count = 0;
    const char* create_table_sql = "CREATE TABLE IF NOT EXISTS users(id INTEGER PRIMARY KEY, name VARCHAR(50), email VARCHAR(100), age INTEGER);";
    
    /* 为每个连接执行创建表操作 */
    for (int i = 0; i < ops->pool_size; i++) {
        vox_db_pool_exec_async(ops->pool, create_table_sql, NULL, 0, parallel_on_init_complete, ops);
    }
}

/* ===== 主函数 ===== */

int main(void) {
    vox_log_set_level(VOX_LOG_INFO);
    
    printf("=== 复杂异步操作示例 ===\n\n");
    
    vox_loop_t* loop = vox_loop_create();
    if (!loop) {
        fprintf(stderr, "创建事件循环失败\n");
        return 1;
    }
    
    vox_db_conn_t* db = vox_db_connect(loop, VOX_DB_DRIVER_SQLITE3, ":memory:");
    if (!db) {
        db = vox_db_connect(loop, VOX_DB_DRIVER_DUCKDB, ":memory:");
    }
    if (!db) {
        VOX_LOG_ERROR("无法连接数据库");
        vox_loop_destroy(loop);
        return 1;
    }
    
    /* 使用 LOOP 模式，回调在 loop 线程执行 */
    vox_db_set_callback_mode(db, VOX_DB_CALLBACK_LOOP);
    
    /* 测试1：状态机模式 */
    printf("--- 测试1：状态机模式 ---\n");
    {
        state_machine_t sm = {0};
        sm.loop = loop;
        sm.db = db;
        sm.state = ASYNC_STATE_INIT;
        
        vox_loop_queue_work_immediate(loop, state_machine_start, &sm);
        vox_loop_run(loop, VOX_RUN_DEFAULT);
    }
    
    printf("\n--- 测试2：回调链模式 ---\n");
    {
        callback_chain_t chain = {0};
        chain.loop = loop;
        chain.db = db;
        chain.step = 1;
        
        /* 启动回调链 */
        vox_loop_queue_work_immediate(loop, start_chain, &chain);
        vox_loop_run(loop, VOX_RUN_DEFAULT);
    }
    
    printf("\n--- 测试3：并行操作模式 ---\n");
    {
        /* 创建连接池以支持真正的并行操作 */
        const int pool_size = 3;
        vox_db_pool_t* pool = vox_db_pool_create_ex(loop, VOX_DB_DRIVER_SQLITE3, ":memory:", pool_size, pool_size);
        if (!pool) {
            pool = vox_db_pool_create_ex(loop, VOX_DB_DRIVER_DUCKDB, ":memory:", pool_size, pool_size);
        }
        if (!pool) {
            printf("[并行操作] 警告：无法创建连接池，跳过并行操作测试\n");
        } else {
            parallel_ops_t ops = {0};
            ops.loop = loop;
            ops.pool = pool;
            ops.pool_size = pool_size;
            vox_mutex_create(&ops.mutex);
            
            vox_loop_queue_work_immediate(loop, parallel_ops_start, &ops);
            vox_loop_run(loop, VOX_RUN_DEFAULT);
            
            vox_mutex_destroy(&ops.mutex);
            vox_db_pool_destroy(pool);
        }
    }
    
    printf("\n=== 所有测试完成 ===\n");
    
    vox_db_disconnect(db);
    vox_loop_destroy(loop);
    return 0;
}
