/*
 * db_orm_example.c - ORM 完整示例（同步 / 异步 / 协程，均从连接池取连接）
 *
 * 演示：实体描述符、建表（含自增主键与索引）、Insert / Select 单行与多行 / Update / Delete / 删表。
 * 1) 同步 API：vox_orm_*，连接从 vox_db_pool_acquire_async 取得，用毕 vox_db_pool_release
 * 2) 异步 API：vox_orm_*_async 链式回调，先 acquire 再跑链式，结束时 release
 * 3) 协程 API：vox_coroutine_db_pool_acquire_await 取连接，vox_coroutine_orm_*_await 操作，结束时 release
 *
 * 启动时传入数据库类型，根据类型使用对应数据库，例如：
 *   ./db_orm_example sqlite3
 *   ./db_orm_example mysql
 *   ./db_orm_example pgsql
 *   ./db_orm_example duckdb
 * 可选第二个参数为自定义 DSN，覆盖默认连接串。
 *
 * 依赖：至少启用一个 DB 驱动（VOX_USE_SQLITE3 或 VOX_USE_DUCKDB 等）
 */

#include "../vox_loop.h"
#include "../vox_log.h"
#include "../vox_vector.h"
#include "../db/vox_db.h"
#include "../db/vox_orm.h"
#include "../db/vox_db_pool.h"
#include "../coroutine/vox_coroutine.h"
#include "../coroutine/vox_coroutine_db.h"

#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <ctype.h>

/* ========== 实体：用户表 ========== */

typedef struct user_row {
    int64_t id;
    char    name[64];
    char    email[128];
    int64_t age;
    bool    active;
} user_row_t;

/* 描述符：列名、类型、offset、主键、自增、普通索引、唯一索引、buffer_size */
static const vox_orm_field_t user_fields[] = {
    { "id",     VOX_DB_TYPE_I64,  offsetof(user_row_t, id),     1, 1, 0, 0, 0 },   /* 主键+自增 */
    { "name",   VOX_DB_TYPE_TEXT, offsetof(user_row_t, name),   0, 0, 1, 0, 64 },  /* 普通索引 idx_users_name */
    { "email",  VOX_DB_TYPE_TEXT, offsetof(user_row_t, email),  0, 0, 0, 1, 128 }, /* 唯一索引 idx_users_email */
    { "age",    VOX_DB_TYPE_I64,  offsetof(user_row_t, age),    0, 0, 0, 0, 0 },
    { "active", VOX_DB_TYPE_BOOL, offsetof(user_row_t, active), 0, 0, 0, 0, 0 },
};

#define USER_TABLE       "users_sync"
#define USER_TABLE_ASYNC "users_async"
#define USER_TABLE_COROUTINE "users_coroutine"
#define USER_FIELD_COUNT (sizeof(user_fields) / sizeof(user_fields[0]))

/* 不区分大小写比较字符串，用于解析命令行数据库类型 */
static int str_iequal(const char* a, const char* b) {
    if (!a || !b) return 0;
    for (; *a && *b; a++, b++) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
            return 0;
    }
    return (*a == '\0' && *b == '\0');
}

/* 根据命令行参数解析驱动类型与默认 DSN；返回 0 成功，-1 未知类型 */
static int parse_db_type(const char* arg, vox_db_driver_t* out_driver, const char** out_dsn) {
    if (!arg || !out_driver || !out_dsn) return -1;
    if (str_iequal(arg, "sqlite3") || str_iequal(arg, "sqlite")) {
        *out_driver = VOX_DB_DRIVER_SQLITE3;
        *out_dsn = ":memory:";
        return 0;
    }
    if (str_iequal(arg, "duckdb")) {
        *out_driver = VOX_DB_DRIVER_DUCKDB;
        *out_dsn = ":memory:";
        return 0;
    }
    if (str_iequal(arg, "mysql")) {
        *out_driver = VOX_DB_DRIVER_MYSQL;
        *out_dsn = "host=127.0.0.1;port=3306;user=root;password=helloworld;db=test;charset=utf8mb4";
        return 0;
    }
    if (str_iequal(arg, "pgsql") || str_iequal(arg, "postgres") || str_iequal(arg, "postgresql")) {
        *out_driver = VOX_DB_DRIVER_PGSQL;
        *out_dsn = "host=127.0.0.1 port=5433 user=testdb password=testdb dbname=testdb";
        return 0;
    }
    return -1;
}

static void print_user(const user_row_t* u) {
    printf("  id=%lld name=%s email=%s age=%lld active=%s\n",
           (long long)u->id, u->name, u->email, (long long)u->age, u->active ? "true" : "false");
}

/* ========== 异步 ORM 状态与链式回调 ========== */

typedef struct {
    vox_loop_t* loop;
    vox_db_pool_t* pool;
    vox_db_conn_t* conn;
    int phase;
    user_row_t u1;
    user_row_t u2;
    user_row_t u2_updated; /* 用于 update id=2 */
    user_row_t row;
    vox_db_value_t id_param;
    vox_db_value_t id2_param;
    vox_vector_t* list;
    int64_t aff;
    int found;
} async_orm_ctx_t;

static void async_orm_next_create_table(async_orm_ctx_t* ctx);
static void async_orm_select_one_cb(vox_db_conn_t* conn, int status, void* row_struct, void* user_data);
static void async_orm_select_done_cb(vox_db_conn_t* conn, int status, int64_t row_count, void* user_data);

/* 异步出错时释放连接并 unref loop，避免程序挂起 */
static void async_orm_cleanup_on_error(async_orm_ctx_t* ctx) {
    if (!ctx) return;
    if (ctx->pool && ctx->conn) {
        vox_db_pool_release(ctx->pool, ctx->conn);
        ctx->conn = NULL;
    }
    vox_loop_unref(ctx->loop);
}

static void async_orm_exec_cb(vox_db_conn_t* conn, int status, int64_t affected_rows, void* user_data) {
    (void)conn;
    async_orm_ctx_t* ctx = (async_orm_ctx_t*)user_data;
    if (!ctx) return;
    if (status != 0) {
        fprintf(stderr, "[ORM async] phase %d failed: %s\n", ctx->phase, vox_db_last_error(ctx->conn) ? vox_db_last_error(ctx->conn) : "");
        async_orm_cleanup_on_error(ctx);
        return;
    }
    ctx->aff = affected_rows;
    switch (ctx->phase) {
        case 0: /* create_table done -> insert u1 */
            printf("[ORM async] create_table ok\n");
            ctx->phase = 1;
            vox_orm_insert_async(ctx->conn, USER_TABLE_ASYNC, user_fields, USER_FIELD_COUNT, &ctx->u1, async_orm_exec_cb, ctx);
            break;
        case 1: /* insert u1 done -> insert u2 */
            printf("[ORM async] insert u1 ok, affected=%lld\n", (long long)ctx->aff);
            ctx->phase = 2;
            vox_orm_insert_async(ctx->conn, USER_TABLE_ASYNC, user_fields, USER_FIELD_COUNT, &ctx->u2, async_orm_exec_cb, ctx);
            break;
        case 2: /* insert u2 done -> select_one */
            printf("[ORM async] insert u2 ok, affected=%lld\n", (long long)ctx->aff);
            ctx->phase = -1; /* select_one uses its own cb */
            if (vox_orm_select_one_async(ctx->conn, USER_TABLE_ASYNC, user_fields, USER_FIELD_COUNT, sizeof(user_row_t), "id = ?", &ctx->id_param, 1, async_orm_select_one_cb, ctx) != 0) {
                fprintf(stderr, "[ORM async] select_one_async submit failed\n");
                async_orm_cleanup_on_error(ctx);
            }
            break;
        case 8: /* update done -> delete */
            printf("[ORM async] update id=2 ok, affected=%lld\n", (long long)ctx->aff);
            ctx->phase = 4;
            vox_orm_delete_async(ctx->conn, USER_TABLE_ASYNC, "id = ?", &ctx->id2_param, 1, async_orm_exec_cb, ctx);
            break;
        case 4: /* delete done -> select again */
            printf("[ORM async] delete id=2 ok, affected=%lld\n", (long long)ctx->aff);
            ctx->phase = 5;
            ctx->list = vox_vector_create(vox_db_get_mpool(ctx->conn));
            if (!ctx->list) { async_orm_cleanup_on_error(ctx); return; }
            if (vox_orm_select_async(ctx->conn, USER_TABLE_ASYNC, user_fields, USER_FIELD_COUNT, sizeof(user_row_t), ctx->list, "1=1", NULL, 0, async_orm_select_done_cb, ctx) != 0) {
                fprintf(stderr, "[ORM async] select_async (after delete) failed\n");
                async_orm_cleanup_on_error(ctx);
                return;
            }
            break;
        case 6: /* drop_table done -> stop */
            if (status != 0) {
                fprintf(stderr, "[ORM async] drop_table failed: %s\n", vox_db_last_error(ctx->conn) ? vox_db_last_error(ctx->conn) : "");
            } else {
                printf("[ORM async] drop_table ok\n");
            }
            if (ctx->pool && ctx->conn) {
                vox_db_pool_release(ctx->pool, ctx->conn);
                ctx->conn = NULL;
            }
            vox_loop_unref(ctx->loop);
            break;
        default:
            printf("[ORM async] phase %d done\n", ctx->phase);
            if (ctx->pool && ctx->conn) {
                vox_db_pool_release(ctx->pool, ctx->conn);
                ctx->conn = NULL;
            }
            vox_loop_unref(ctx->loop);
            break;
    }
}

static void async_orm_select_one_cb(vox_db_conn_t* conn, int status, void* row_struct, void* user_data) {
    (void)conn;
    async_orm_ctx_t* ctx = (async_orm_ctx_t*)user_data;
    if (!ctx) return;
    if (status != 0) {
        fprintf(stderr, "[ORM async] select_one failed\n");
        async_orm_cleanup_on_error(ctx);
        return;
    }
    if (row_struct) {
        memcpy(&ctx->row, row_struct, sizeof(ctx->row));
        ctx->found = 1;
    } else {
        ctx->found = 0;
    }
    if (ctx->found) {
        printf("[ORM async] select_one id=1: ");
        print_user(&ctx->row);
    } else {
        printf("[ORM async] select_one id=1: not found\n");
    }
    ctx->phase = 7; /* select_one done -> select all */
    ctx->list = vox_vector_create(vox_db_get_mpool(ctx->conn));
    if (!ctx->list) { async_orm_cleanup_on_error(ctx); return; }
    if (vox_orm_select_async(ctx->conn, USER_TABLE_ASYNC, user_fields, USER_FIELD_COUNT, sizeof(user_row_t), ctx->list, "1=1", NULL, 0, async_orm_select_done_cb, ctx) != 0) {
        fprintf(stderr, "[ORM async] select_async (after select_one) failed\n");
        async_orm_cleanup_on_error(ctx);
        return;
    }
}

static void async_orm_select_done_cb(vox_db_conn_t* conn, int status, int64_t row_count, void* user_data) {
    (void)conn;
    async_orm_ctx_t* ctx = (async_orm_ctx_t*)user_data;
    if (!ctx) return;
    if (status != 0) {
        fprintf(stderr, "[ORM async] select failed: %s\n", vox_db_last_error(ctx->conn) ? vox_db_last_error(ctx->conn) : "");
        async_orm_cleanup_on_error(ctx);
        return;
    }
    if (ctx->phase == 7) {
        printf("[ORM async] select all, row_count=%lld\n", (long long)row_count);
        for (size_t i = 0; i < vox_vector_size(ctx->list); i++) {
            user_row_t* p = (user_row_t*)vox_vector_get(ctx->list, i);
            if (p) print_user(p);
        }
        ctx->phase = 8; /* update id=2 */
        ctx->u2_updated.id = 2;
        ctx->u2_updated.age = 23;
        ctx->u2_updated.active = true;
        strncpy(ctx->u2_updated.name, "bob", sizeof(ctx->u2_updated.name) - 1);
        strncpy(ctx->u2_updated.email, "bob@example.com", sizeof(ctx->u2_updated.email) - 1);
        vox_orm_update_async(ctx->conn, USER_TABLE_ASYNC, user_fields, USER_FIELD_COUNT, &ctx->u2_updated, "id = ?", &ctx->id2_param, 1, async_orm_exec_cb, ctx);
        return;
    }
    /* phase 5: after delete, select again */
    printf("[ORM async] after delete, row_count=%lld\n", (long long)row_count);
    for (size_t i = 0; i < vox_vector_size(ctx->list); i++) {
        user_row_t* p = (user_row_t*)vox_vector_get(ctx->list, i);
        if (p) print_user(p);
    }
    ctx->phase = 6;
    vox_orm_drop_table_async(ctx->conn, USER_TABLE_ASYNC, async_orm_exec_cb, ctx);
}

/* 开头先删表再建表，避免上次未完成时残留导致唯一约束冲突 */
static void async_orm_drop_done_then_create(vox_db_conn_t* conn, int status, int64_t affected_rows, void* user_data) {
    (void)status;
    (void)affected_rows;
    (void)conn;
    async_orm_ctx_t* ctx = (async_orm_ctx_t*)user_data;
    if (!ctx || !ctx->conn) return;
    if (vox_orm_create_table_async(ctx->conn, USER_TABLE_ASYNC, user_fields, USER_FIELD_COUNT, async_orm_exec_cb, ctx) != 0) {
        fprintf(stderr, "[ORM async] create_table_async submit failed\n");
        async_orm_cleanup_on_error(ctx);
    }
}

static void async_orm_next_create_table(async_orm_ctx_t* ctx) {
    if (!ctx || !ctx->conn) return;
    if (vox_orm_drop_table_async(ctx->conn, USER_TABLE_ASYNC, async_orm_drop_done_then_create, ctx) != 0) {
        fprintf(stderr, "[ORM async] drop_table_async submit failed\n");
        async_orm_cleanup_on_error(ctx);
    }
}

static void async_orm_acquired_cb(vox_db_pool_t* pool, vox_db_conn_t* conn, int status, void* user_data) {
    (void)pool;
    async_orm_ctx_t* ctx = (async_orm_ctx_t*)user_data;
    if (!ctx) return;
    if (status != 0 || !conn) {
        fprintf(stderr, "[ORM async] acquire failed\n");
        vox_loop_unref(ctx->loop);
        return;
    }
    ctx->conn = conn;
    vox_db_set_callback_mode(conn, VOX_DB_CALLBACK_LOOP);
    async_orm_next_create_table(ctx);
}

static void run_async_orm(vox_loop_t* loop, vox_db_pool_t* pool) {
    vox_loop_ref(loop);
    static async_orm_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.loop = loop;
    ctx.pool = pool;
    ctx.conn = NULL;
    ctx.phase = 0;
    ctx.u1.id = 0; ctx.u1.age = 0; ctx.u1.active = false;
    strncpy(ctx.u1.name, "alice", sizeof(ctx.u1.name) - 1);
    strncpy(ctx.u1.email, "alice@example.com", sizeof(ctx.u1.email) - 1);
    ctx.u1.age = 20; ctx.u1.active = true;
    ctx.u2.id = 0; ctx.u2.age = 0; ctx.u2.active = false;
    strncpy(ctx.u2.name, "bob", sizeof(ctx.u2.name) - 1);
    strncpy(ctx.u2.email, "bob@example.com", sizeof(ctx.u2.email) - 1);
    ctx.u2.age = 22; ctx.u2.active = true;
    ctx.id_param.type = VOX_DB_TYPE_I64; ctx.id_param.u.i64 = 1;
    ctx.id2_param.type = VOX_DB_TYPE_I64; ctx.id2_param.u.i64 = 2;
    if (vox_db_pool_acquire_async(pool, async_orm_acquired_cb, &ctx) != 0) {
        fprintf(stderr, "[ORM async] acquire_async failed\n");
        vox_loop_unref(loop);
    }
}

static void run_sync_orm(vox_loop_t *loop, vox_db_conn_t *conn) {
    (void)loop;
    /* 开头先删表，避免上次未完成时残留数据导致唯一约束冲突（pgsql/mysql 持久化库） */
    (void)vox_orm_drop_table(conn, USER_TABLE);
    /* ---------- 1. 建表（自动建主键、自增、索引 idx_users_name / idx_users_email） ---------- */
    if (vox_orm_create_table(conn, USER_TABLE, user_fields, USER_FIELD_COUNT) != 0) {
        fprintf(stderr, "create_table failed: %s\n", vox_db_last_error(conn) ? vox_db_last_error(conn) : "");
        return;
    }
    printf("[ORM] create_table ok\n");

    /* ---------- 2. 插入两行（id 由库自增，不填） ---------- */
    user_row_t u1 = { .id = 0, .age = 0, .active = false };
    strncpy(u1.name, "alice", sizeof(u1.name) - 1);
    strncpy(u1.email, "alice@example.com", sizeof(u1.email) - 1);
    u1.age = 20;
    u1.active = true;

    int64_t aff = 0;
    if (vox_orm_insert(conn, USER_TABLE, user_fields, USER_FIELD_COUNT, &u1, &aff) != 0) {
        fprintf(stderr, "insert u1 failed\n");
        return;
    }
    printf("[ORM] insert u1 ok, affected=%lld\n", (long long)aff);

    user_row_t u2 = { .id = 0, .age = 0, .active = false };
    strncpy(u2.name, "bob", sizeof(u2.name) - 1);
    strncpy(u2.email, "bob@example.com", sizeof(u2.email) - 1);
    u2.age = 22;
    u2.active = true;
    if (vox_orm_insert(conn, USER_TABLE, user_fields, USER_FIELD_COUNT, &u2, &aff) != 0) {
        fprintf(stderr, "insert u2 failed\n");
        return;
    }
    printf("[ORM] insert u2 ok, affected=%lld\n", (long long)aff);

    /* ---------- 3. 按主键查单行（id=1） ---------- */
    user_row_t row = { 0 };
    vox_db_value_t id_param = { .type = VOX_DB_TYPE_I64, .u.i64 = 1 };
    int found = 0;
    if (vox_orm_select_one(conn, USER_TABLE, user_fields, USER_FIELD_COUNT,
                           &row, sizeof(row), "id = ?", &id_param, 1, &found) != 0) {
        fprintf(stderr, "[ORM] select_one id=1 failed: %s\n", vox_db_last_error(conn) ? vox_db_last_error(conn) : "");
        return;
    }
    if (found) {
        printf("[ORM] select_one id=1: ");
        print_user(&row);
    } else {
        printf("[ORM] select_one id=1: not found\n");
    }

    /* ---------- 4. 查多行（WHERE 1=1，全部） ---------- */
    vox_mpool_t* mpool = vox_db_get_mpool(conn);
    vox_vector_t* list = vox_vector_create(mpool);
    if (!list) {
        fprintf(stderr, "vox_vector_create failed\n");
        return;
    }
    int64_t row_count = 0;
    if (vox_orm_select(conn, USER_TABLE, user_fields, USER_FIELD_COUNT,
                       sizeof(user_row_t), list, &row_count,
                       "1=1", NULL, 0) != 0) {
        fprintf(stderr, "select failed: %s\n", vox_db_last_error(conn) ? vox_db_last_error(conn) : "");
        vox_vector_destroy(list);
        return;
    }
    printf("[ORM] select all, row_count=%lld\n", (long long)row_count);
    for (size_t i = 0; i < vox_vector_size(list); i++) {
        user_row_t* p = (user_row_t*)vox_vector_get(list, i);
        if (p) print_user(p);
    }
    vox_vector_clear(list);

    /* ---------- 5. 更新 id=2 的 age ---------- */
    user_row_t u2_updated = { .id = 2, .age = 0, .active = false };
    strncpy(u2_updated.name, "bob", sizeof(u2_updated.name) - 1);
    strncpy(u2_updated.email, "bob@example.com", sizeof(u2_updated.email) - 1);
    u2_updated.age = 23;
    u2_updated.active = true;
    vox_db_value_t id2 = { .type = VOX_DB_TYPE_I64, .u.i64 = 2 };
    if (vox_orm_update(conn, USER_TABLE, user_fields, USER_FIELD_COUNT,
                      &u2_updated, "id = ?", &id2, 1, &aff) != 0) {
        fprintf(stderr, "update failed\n");
        vox_vector_destroy(list);
        return;
    }
    printf("[ORM] update id=2 ok, affected=%lld\n", (long long)aff);

    /* ---------- 6. 删除 id=2 ---------- */
    if (vox_orm_delete(conn, USER_TABLE, "id = ?", &id2, 1, &aff) != 0) {
        fprintf(stderr, "delete failed\n");
        vox_vector_destroy(list);
        return;
    }
    printf("[ORM] delete id=2 ok, affected=%lld\n", (long long)aff);

    /* ---------- 7. 再查多行确认只剩一条 ---------- */
    vox_vector_clear(list);
    if (vox_orm_select(conn, USER_TABLE, user_fields, USER_FIELD_COUNT,
                       sizeof(user_row_t), list, &row_count, "1=1", NULL, 0) != 0) {
        fprintf(stderr, "select failed: %s\n", vox_db_last_error(conn) ? vox_db_last_error(conn) : "");
        vox_vector_destroy(list);
        return;
    }
    printf("[ORM] after delete, row_count=%lld\n", (long long)row_count);
    for (size_t i = 0; i < vox_vector_size(list); i++) {
        user_row_t* p = (user_row_t*)vox_vector_get(list, i);
        if (p) print_user(p);
    }

    vox_vector_destroy(list);

    /* ---------- 8. 删表 ---------- */
    if (vox_orm_drop_table(conn, USER_TABLE) != 0) {
        fprintf(stderr, "[ORM] drop_table failed: %s\n", vox_db_last_error(conn) ? vox_db_last_error(conn) : "");
    } else {
        printf("[ORM] drop_table ok\n");
    }

    printf("[ORM] sync example done\n");
}

/* ========== 协程 ORM 示例（从连接池取连接，用毕归还） ========== */
VOX_COROUTINE_ENTRY(orm_coroutine_entry, vox_db_pool_t*) {
    vox_db_pool_t* pool = (vox_db_pool_t*)user_data;
    vox_db_conn_t* conn = NULL;
    vox_vector_t* list = NULL;
    printf("\n[ORM coroutine] start\n");
    if (vox_coroutine_db_pool_acquire_await(co, pool, &conn) != 0 || !conn) {
        fprintf(stderr, "[ORM coroutine] pool acquire failed\n");
        goto done;
    }
    vox_db_set_callback_mode(conn, VOX_DB_CALLBACK_LOOP);
    /* 开头先删表，避免上次未完成时残留导致唯一约束冲突 */
    (void)vox_coroutine_orm_drop_table_await(co, conn, USER_TABLE_COROUTINE);
    if (vox_coroutine_orm_create_table_await(co, conn, USER_TABLE_COROUTINE, user_fields, USER_FIELD_COUNT) != 0) {
        fprintf(stderr, "[ORM coroutine] create_table failed\n");
        goto done;
    }
    printf("[ORM coroutine] create_table ok\n");
    user_row_t u1 = { .id = 0, .age = 0, .active = false };
    strncpy(u1.name, "alice", sizeof(u1.name) - 1);
    strncpy(u1.email, "alice@example.com", sizeof(u1.email) - 1);
    u1.age = 20; u1.active = true;
    int64_t aff = 0;
    if (vox_coroutine_orm_insert_await(co, conn, USER_TABLE_COROUTINE, user_fields, USER_FIELD_COUNT, &u1, &aff) != 0) {
        fprintf(stderr, "[ORM coroutine] insert u1 failed\n");
        goto done;
    }
    printf("[ORM coroutine] insert u1 ok, affected=%lld\n", (long long)aff);
    user_row_t u2 = { .id = 0, .age = 0, .active = false };
    strncpy(u2.name, "bob", sizeof(u2.name) - 1);
    strncpy(u2.email, "bob@example.com", sizeof(u2.email) - 1);
    u2.age = 22; u2.active = true;
    if (vox_coroutine_orm_insert_await(co, conn, USER_TABLE_COROUTINE, user_fields, USER_FIELD_COUNT, &u2, &aff) != 0) {
        fprintf(stderr, "[ORM coroutine] insert u2 failed\n");
        goto done;
    }
    printf("[ORM coroutine] insert u2 ok, affected=%lld\n", (long long)aff);
    user_row_t row = { 0 };
    vox_db_value_t id_param = { .type = VOX_DB_TYPE_I64, .u.i64 = 1 };
    int found = 0;
    if (vox_coroutine_orm_select_one_await(co, conn, USER_TABLE_COROUTINE, user_fields, USER_FIELD_COUNT,
            &row, sizeof(row), "id = ?", &id_param, 1, &found) != 0) {
        fprintf(stderr, "[ORM coroutine] select_one failed\n");
        goto done;
    }
    if (found) {
        printf("[ORM coroutine] select_one id=1: ");
        print_user(&row);
    } else {
        printf("[ORM coroutine] select_one id=1: not found\n");
    }
    vox_mpool_t* mpool = vox_db_get_mpool(conn);
    list = vox_vector_create(mpool);
    if (!list) { fprintf(stderr, "[ORM coroutine] vector create failed\n"); goto done; }
    int64_t row_count = 0;
    if (vox_coroutine_orm_select_await(co, conn, USER_TABLE_COROUTINE, user_fields, USER_FIELD_COUNT,
            sizeof(user_row_t), list, &row_count, "1=1", NULL, 0) != 0) {
        fprintf(stderr, "[ORM coroutine] select failed: %s\n", vox_db_last_error(conn) ? vox_db_last_error(conn) : "");
        vox_vector_destroy(list);
        goto done;
    }
    printf("[ORM coroutine] select all, row_count=%lld\n", (long long)row_count);
    for (size_t i = 0; i < vox_vector_size(list); i++) {
        user_row_t* p = (user_row_t*)vox_vector_get(list, i);
        if (p) print_user(p);
    }
    user_row_t u2_up = { .id = 2, .age = 0, .active = false };
    strncpy(u2_up.name, "bob", sizeof(u2_up.name) - 1);
    strncpy(u2_up.email, "bob@example.com", sizeof(u2_up.email) - 1);
    u2_up.age = 23; u2_up.active = true;
    vox_db_value_t id2 = { .type = VOX_DB_TYPE_I64, .u.i64 = 2 };
    if (vox_coroutine_orm_update_await(co, conn, USER_TABLE_COROUTINE, user_fields, USER_FIELD_COUNT,
            &u2_up, "id = ?", &id2, 1, &aff) != 0) {
        fprintf(stderr, "[ORM coroutine] update failed\n");
        vox_vector_destroy(list);
        goto done;
    }
    printf("[ORM coroutine] update id=2 ok, affected=%lld\n", (long long)aff);
    if (vox_coroutine_orm_delete_await(co, conn, USER_TABLE_COROUTINE, "id = ?", &id2, 1, &aff) != 0) {
        fprintf(stderr, "[ORM coroutine] delete failed\n");
        vox_vector_destroy(list);
        goto done;
    }
    printf("[ORM coroutine] delete id=2 ok, affected=%lld\n", (long long)aff);
    vox_vector_clear(list);
    if (vox_coroutine_orm_select_await(co, conn, USER_TABLE_COROUTINE, user_fields, USER_FIELD_COUNT,
            sizeof(user_row_t), list, &row_count, "1=1", NULL, 0) != 0) {
        vox_vector_destroy(list);
        goto done;
    }
    printf("[ORM coroutine] after delete, row_count=%lld\n", (long long)row_count);
    for (size_t i = 0; i < vox_vector_size(list); i++) {
        user_row_t* p = (user_row_t*)vox_vector_get(list, i);
        if (p) print_user(p);
    }
    vox_vector_destroy(list);
    list = NULL;
    if (vox_coroutine_orm_drop_table_await(co, conn, USER_TABLE_COROUTINE) != 0) {
        fprintf(stderr, "[ORM coroutine] drop_table failed: %s\n", vox_db_last_error(conn) ? vox_db_last_error(conn) : "");
        goto done;
    }
    printf("[ORM coroutine] drop_table ok, example done\n");
done:
    if (pool && conn) {
        vox_db_pool_release(pool, conn);
        conn = NULL;
    }
    vox_loop_unref(vox_coroutine_get_loop(co));  /* 协程结束（含错误路径）释放 loop 引用以便 loop 可退出 */
}

static void print_usage(const char* prog) {
    fprintf(stderr, "Usage: %s <database_type> [dsn]\n", prog ? prog : "db_orm_example");
    fprintf(stderr, "  database_type: sqlite3|sqlite, duckdb, mysql, pgsql|postgres|postgresql\n");
    fprintf(stderr, "  dsn: optional connection string (default: in-memory for sqlite/duckdb, or built-in for mysql/pgsql)\n");
    fprintf(stderr, "Example: %s mysql\n", prog ? prog : "db_orm_example");
    fprintf(stderr, "Example: %s sqlite3\n", prog ? prog : "db_orm_example");
}

int main(int argc, char** argv) {
    vox_log_set_level(VOX_LOG_DEBUG);

    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    vox_db_driver_t driver;
    const char* dsn = NULL;
    if (parse_db_type(argv[1], &driver, &dsn) != 0) {
        fprintf(stderr, "Unknown database type: %s\n", argv[1]);
        print_usage(argv[0]);
        return 1;
    }
    /* 若传入第二个参数则作为自定义 DSN 覆盖默认值 */
    if (argc >= 3)
        dsn = argv[2];

    vox_loop_t* loop = vox_loop_create();
    if (!loop) {
        fprintf(stderr, "vox_loop_create failed\n");
        return 1;
    }

    vox_db_pool_t* pool = vox_db_pool_create(loop, driver, dsn, 2, 4, NULL, NULL);
    if (!pool) {
        fprintf(stderr, "failed to create DB pool (driver=%s, dsn=%s). Check driver is enabled and DSN is valid.\n", argv[1], dsn);
        vox_loop_destroy(loop);
        return 1;
    }
    printf("[ORM] using database: %s (dsn=%s)\n", argv[1], dsn);

    printf("\n--- Sync ORM ---\n");
    vox_db_conn_t* conn = vox_db_pool_acquire_sync(pool);
    if (!conn) {
        fprintf(stderr, "[ORM] sync acquire failed (no idle conn)\n");
        vox_db_pool_destroy(pool);
        vox_loop_destroy(loop);
        return 1;
    }
    run_sync_orm(loop, conn);
    vox_db_pool_release(pool, conn);

    printf("\n--- Coroutine ORM ---\n");
    vox_loop_ref(loop);
    VOX_COROUTINE_START(loop, orm_coroutine_entry, pool);

    printf("\n--- Async ORM ---\n");
    run_async_orm(loop, pool);

    vox_loop_run(loop, VOX_RUN_DEFAULT);

    vox_db_pool_destroy(pool);
    vox_loop_destroy(loop);

    printf("\n[ORM] all examples done\n");
    return 0;
}
