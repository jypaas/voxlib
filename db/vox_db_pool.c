/*
 * vox_db_pool.c - 数据库连接池（纯连接管理，与 Redis 连接池设计一致）
 *
 * 初始连接数 + 最大连接数；超过初始部分的为临时连接，归还时自动关闭并移除。
 * 提供 acquire_async / release 及便捷接口（exec/query 内部借还连接）。
 */

#include "vox_db_pool.h"
#include "vox_db_internal.h"

#include "../vox_log.h"
#include "../vox_mutex.h"
#include "../vox_list.h"

#include <string.h>

struct vox_db_pool {
    vox_loop_t* loop;
    vox_mpool_t* mpool;
    vox_db_driver_t driver;
    char* conninfo;

    vox_db_callback_mode_t cb_mode;

    size_t initial_size;
    size_t max_size;
    vox_db_conn_t** conns;   /* [initial_size] 常驻连接 */

    vox_list_t idle_list;    /* 空闲的初始连接 (pool_conn_node_t) */
    vox_list_t in_use_list;  /* 正在使用的临时连接 (pool_conn_node_t) */
    vox_list_t waiting_list; /* 等待取连接的请求 (acquire_waiter_t) */

    size_t pending_temp;    /* 正在创建中的临时连接数 */
    bool destroyed;

    vox_mutex_t mu;

    vox_db_pool_connect_cb connect_cb;
    void* connect_user_data;
};

typedef struct pool_conn_node {
    vox_list_node_t node;
    vox_db_conn_t* conn;
} pool_conn_node_t;

typedef struct acquire_waiter {
    vox_list_node_t node;
    vox_db_pool_acquire_cb cb;
    void* user_data;
} acquire_waiter_t;

/* ----- 内部辅助（调用者已持锁） ----- */

static inline size_t pool_idle_count_locked(vox_db_pool_t* pool) {
    return vox_list_size(&pool->idle_list);
}

static inline size_t pool_in_use_temp_count_locked(vox_db_pool_t* pool) {
    return vox_list_size(&pool->in_use_list);
}

static inline bool pool_is_initial_conn(vox_db_pool_t* pool, vox_db_conn_t* conn) {
    for (size_t i = 0; i < pool->initial_size; i++) {
        if (pool->conns[i] == conn)
            return true;
    }
    return false;
}

static pool_conn_node_t* pool_remove_temp_from_in_use_locked(vox_db_pool_t* pool, vox_db_conn_t* conn) {
    vox_list_node_t* cur;
    vox_list_for_each(cur, &pool->in_use_list) {
        pool_conn_node_t* cn = vox_container_of(cur, pool_conn_node_t, node);
        if (cn->conn == conn) {
            vox_list_remove(&pool->in_use_list, cur);
            return cn;
        }
    }
    return NULL;
}

/* 从空闲列表弹出一个健康连接；无可用返回 NULL。会释放锁做 ping，返回前可能未持锁。 */
static vox_db_conn_t* pool_pop_idle_locked(vox_db_pool_t* pool) {
    while (!vox_list_empty(&pool->idle_list)) {
        vox_list_node_t* node = vox_list_pop_front(&pool->idle_list);
        pool_conn_node_t* cn = vox_container_of(node, pool_conn_node_t, node);
        vox_db_conn_t* conn = cn->conn;
        vox_mpool_free(pool->mpool, cn);

        vox_mutex_unlock(&pool->mu);
        int ping_ok = vox_db_conn_ping_and_reconnect(conn);
        vox_mutex_lock(&pool->mu);

        if (pool->destroyed) {
            vox_mutex_unlock(&pool->mu);
            return NULL;
        }
        if (ping_ok == 0) {
            return conn;
        }
        VOX_LOG_WARN("[db/pool] connection unhealthy on acquire, trying next");
        pool_conn_node_t* again = (pool_conn_node_t*)vox_mpool_alloc(pool->mpool, sizeof(pool_conn_node_t));
        if (again) {
            memset(again, 0, sizeof(*again));
            again->conn = conn;
            vox_list_push_back(&pool->idle_list, &again->node);
        } else {
            /* OOM：无法放回空闲列表，断开连接避免泄漏 */
            vox_db_disconnect(conn);
            for (size_t i = 0; i < pool->initial_size; i++) {
                if (pool->conns[i] == conn) {
                    pool->conns[i] = NULL;
                    break;
                }
            }
        }
    }
    return NULL;
}

/* 向空闲列表压入连接（仅初始连接；空闲链表未满时调用） */
static int pool_push_idle_locked(vox_db_pool_t* pool, vox_db_conn_t* conn) {
    if (pool_idle_count_locked(pool) >= pool->initial_size)
        return -1;
    pool_conn_node_t* cn = (pool_conn_node_t*)vox_mpool_alloc(pool->mpool, sizeof(pool_conn_node_t));
    if (!cn) return -1;
    memset(cn, 0, sizeof(*cn));
    cn->conn = conn;
    vox_list_push_back(&pool->idle_list, &cn->node);
    return 0;
}

static void pool_serve_one_waiter_locked(vox_db_pool_t* pool);

/* 在 worker 线程中创建临时连接时使用的上下文（需在 pool_temp_fail_cb 前定义） */
typedef struct temp_connect_ctx {
    vox_db_pool_t* pool;
    vox_db_pool_acquire_cb cb;
    void* user_data;
} temp_connect_ctx_t;

/* 临时连接创建失败时（alloc done 失败）用 ctx 切回 loop，单独回调 */
static void pool_temp_fail_cb(vox_loop_t* loop, void* user_data) {
    (void)loop;
    temp_connect_ctx_t* ctx = (temp_connect_ctx_t*)user_data;
    vox_db_pool_t* pool = ctx->pool;
    vox_db_pool_acquire_cb cb = ctx->cb;
    void* ud = ctx->user_data;

    if (vox_mutex_lock(&pool->mu) != 0) {
        vox_mpool_free(pool->mpool, ctx);
        return;
    }
    if (!pool->destroyed)
        pool->pending_temp--;
    vox_mutex_unlock(&pool->mu);
    if (cb)
        cb(pool, NULL, -1, ud);
    vox_mpool_free(pool->mpool, ctx);
    vox_mutex_lock(&pool->mu);
    pool_serve_one_waiter_locked(pool);
    vox_mutex_unlock(&pool->mu);
}

/* 临时连接在 worker 中创建完成，切回 loop 线程后调用 */
typedef struct temp_done_ctx {
    vox_db_pool_t* pool;
    vox_db_conn_t* conn;
    int status;
    vox_db_pool_acquire_cb cb;
    void* user_data;
} temp_done_ctx_t;

static void pool_temp_done_cb(vox_loop_t* loop, void* user_data) {
    (void)loop;
    temp_done_ctx_t* ctx = (temp_done_ctx_t*)user_data;
    vox_db_pool_t* pool = ctx->pool;
    vox_db_conn_t* conn = ctx->conn;
    int status = ctx->status;
    vox_db_pool_acquire_cb cb = ctx->cb;
    void* ud = ctx->user_data;

    if (vox_mutex_lock(&pool->mu) != 0) {
        if (conn && status == 0) vox_db_disconnect(conn);
        vox_mpool_free(pool->mpool, ctx);
        return;
    }
    if (pool->destroyed) {
        pool->pending_temp--;
        vox_mutex_unlock(&pool->mu);
        if (conn && status == 0) vox_db_disconnect(conn);
        vox_mpool_free(pool->mpool, ctx);
        return;
    }

    if (status != 0 || !conn) {
        pool->pending_temp--;
        vox_mutex_unlock(&pool->mu);
        if (cb) cb(pool, NULL, status, ud);
        vox_mpool_free(pool->mpool, ctx);
        vox_mutex_lock(&pool->mu);
        pool_serve_one_waiter_locked(pool);
        vox_mutex_unlock(&pool->mu);
        return;
    }

    (void)vox_db_set_callback_mode(conn, pool->cb_mode);
    pool->pending_temp--;
    pool_conn_node_t* cn = (pool_conn_node_t*)vox_mpool_alloc(pool->mpool, sizeof(pool_conn_node_t));
    if (!cn) {
        vox_mutex_unlock(&pool->mu);
        vox_db_disconnect(conn);
        if (cb) cb(pool, NULL, -1, ud);
        vox_mpool_free(pool->mpool, ctx);
        vox_mutex_lock(&pool->mu);
        pool_serve_one_waiter_locked(pool);
        vox_mutex_unlock(&pool->mu);
        return;
    }
    memset(cn, 0, sizeof(*cn));
    cn->conn = conn;
    vox_list_push_back(&pool->in_use_list, &cn->node);
    vox_mutex_unlock(&pool->mu);

    if (cb)
        cb(pool, conn, 0, ud);
    vox_mpool_free(pool->mpool, ctx);

    vox_mutex_lock(&pool->mu);
    pool_serve_one_waiter_locked(pool);
    vox_mutex_unlock(&pool->mu);
}

static void pool_temp_worker_cb(vox_loop_t* loop, void* user_data) {
    (void)loop;
    temp_connect_ctx_t* ctx = (temp_connect_ctx_t*)user_data;
    vox_db_pool_t* pool = ctx->pool;
    vox_db_conn_t* conn = vox_db_connect(pool->loop, pool->driver, pool->conninfo);
    int status = conn ? 0 : -1;

    temp_done_ctx_t* done = (temp_done_ctx_t*)vox_mpool_alloc(pool->mpool, sizeof(temp_done_ctx_t));
    if (!done) {
        if (conn) vox_db_disconnect(conn);
        vox_loop_queue_work(pool->loop, pool_temp_fail_cb, ctx);
        return;
    }
    memset(done, 0, sizeof(*done));
    done->pool = pool;
    done->conn = conn;
    done->status = status;
    done->cb = ctx->cb;
    done->user_data = ctx->user_data;
    vox_mpool_free(pool->mpool, ctx);

    vox_loop_queue_work(pool->loop, pool_temp_done_cb, done);
}

static void pool_serve_one_waiter_locked(vox_db_pool_t* pool) {
    if (pool->destroyed || vox_list_empty(&pool->waiting_list))
        return;

    vox_db_conn_t* idle = pool_pop_idle_locked(pool);
    if (idle) {
        vox_list_node_t* wn = vox_list_pop_front(&pool->waiting_list);
        acquire_waiter_t* w = vox_container_of(wn, acquire_waiter_t, node);
        vox_db_pool_acquire_cb cb = w->cb;
        void* ud = w->user_data;
        vox_mpool_free(pool->mpool, w);
        vox_mutex_unlock(&pool->mu);
        if (cb)
            cb(pool, idle, 0, ud);
        return;
    }

    size_t idle_count = pool_idle_count_locked(pool);
    size_t temp_count = pool_in_use_temp_count_locked(pool);
    size_t in_use_initial = pool->initial_size - idle_count;
    size_t total = idle_count + in_use_initial + temp_count;
    if (total + pool->pending_temp < pool->max_size) {
        pool->pending_temp++;
        vox_list_node_t* wn = vox_list_pop_front(&pool->waiting_list);
        acquire_waiter_t* w = vox_container_of(wn, acquire_waiter_t, node);
        temp_connect_ctx_t* ctx = (temp_connect_ctx_t*)vox_mpool_alloc(pool->mpool, sizeof(temp_connect_ctx_t));
        if (!ctx) {
            pool->pending_temp--;
            vox_list_push_front(&pool->waiting_list, wn);
            return;
        }
        ctx->pool = pool;
        ctx->cb = w->cb;
        ctx->user_data = w->user_data;
        vox_mpool_free(pool->mpool, w);

        vox_mutex_unlock(&pool->mu);
        if (vox_loop_queue_work(pool->loop, pool_temp_worker_cb, ctx) != 0) {
            vox_mutex_lock(&pool->mu);
            pool->pending_temp--;
            vox_mutex_unlock(&pool->mu);
            if (ctx->cb)
                ctx->cb(pool, NULL, -1, ctx->user_data);
            vox_mpool_free(pool->mpool, ctx);
            vox_mutex_lock(&pool->mu);
            pool_serve_one_waiter_locked(pool);
            vox_mutex_unlock(&pool->mu);
        }
        return;
    }
}

/* ----- 公共 API ----- */

vox_db_pool_t* vox_db_pool_create(vox_loop_t* loop,
                                   vox_db_driver_t driver,
                                   const char* conninfo,
                                   size_t initial_size,
                                   size_t max_size,
                                   vox_db_pool_connect_cb connect_cb,
                                   void* user_data) {
    if (!loop || !conninfo || initial_size == 0 || max_size < initial_size)
        return NULL;

    vox_mpool_t* mpool = vox_loop_get_mpool(loop);
    if (!mpool) return NULL;

    vox_db_pool_t* pool = (vox_db_pool_t*)vox_mpool_alloc(mpool, sizeof(vox_db_pool_t));
    if (!pool) return NULL;
    memset(pool, 0, sizeof(*pool));

    pool->loop = loop;
    pool->mpool = mpool;
    pool->driver = driver;
    pool->initial_size = initial_size;
    pool->max_size = max_size;
    pool->cb_mode = VOX_DB_CALLBACK_WORKER;
    pool->connect_cb = connect_cb;
    pool->connect_user_data = user_data;

    if (vox_mutex_create(&pool->mu) != 0) {
        vox_mpool_free(mpool, pool);
        return NULL;
    }
    vox_list_init(&pool->idle_list);
    vox_list_init(&pool->in_use_list);
    vox_list_init(&pool->waiting_list);

    size_t conninfo_len = strlen(conninfo) + 1;
    pool->conninfo = (char*)vox_mpool_alloc(mpool, conninfo_len);
    pool->conns = (vox_db_conn_t**)vox_mpool_alloc(mpool, sizeof(vox_db_conn_t*) * initial_size);
    if (!pool->conninfo || !pool->conns) {
        vox_db_pool_destroy(pool);
        return NULL;
    }
    memcpy(pool->conninfo, conninfo, conninfo_len);
    memset(pool->conns, 0, sizeof(vox_db_conn_t*) * initial_size);

    for (size_t i = 0; i < initial_size; i++) {
        pool->conns[i] = vox_db_connect(loop, driver, pool->conninfo);
        if (!pool->conns[i]) {
            VOX_LOG_ERROR("[db/pool] connect failed at %zu", i);
            vox_db_pool_destroy(pool);
            return NULL;
        }
        (void)vox_db_set_callback_mode(pool->conns[i], pool->cb_mode);
        if (pool_push_idle_locked(pool, pool->conns[i]) != 0) {
            vox_db_pool_destroy(pool);
            return NULL;
        }
    }

    if (connect_cb)
        connect_cb(pool, 0, user_data);

    return pool;
}

void vox_db_pool_destroy(vox_db_pool_t* pool) {
    if (!pool) return;

    if (vox_mutex_lock(&pool->mu) != 0) return;
    pool->destroyed = true;

    while (!vox_list_empty(&pool->waiting_list)) {
        vox_list_node_t* wn = vox_list_pop_front(&pool->waiting_list);
        acquire_waiter_t* w = vox_container_of(wn, acquire_waiter_t, node);
        if (w->cb)
            w->cb(pool, NULL, -1, w->user_data);
        vox_mpool_free(pool->mpool, w);
    }

    while (!vox_list_empty(&pool->idle_list)) {
        vox_list_node_t* node = vox_list_pop_front(&pool->idle_list);
        pool_conn_node_t* cn = vox_container_of(node, pool_conn_node_t, node);
        vox_mpool_free(pool->mpool, cn);
    }

    while (!vox_list_empty(&pool->in_use_list)) {
        vox_list_node_t* node = vox_list_pop_front(&pool->in_use_list);
        pool_conn_node_t* cn = vox_container_of(node, pool_conn_node_t, node);
        if (cn->conn && !pool_is_initial_conn(pool, cn->conn))
            vox_db_disconnect(cn->conn);
        vox_mpool_free(pool->mpool, cn);
    }

    vox_mutex_unlock(&pool->mu);

    for (size_t i = 0; i < pool->initial_size; i++) {
        if (pool->conns[i]) {
            vox_db_disconnect(pool->conns[i]);
            pool->conns[i] = NULL;
        }
    }

    vox_mutex_destroy(&pool->mu);
    if (pool->mpool) {
        if (pool->conninfo) vox_mpool_free(pool->mpool, pool->conninfo);
        if (pool->conns) vox_mpool_free(pool->mpool, pool->conns);
        vox_mpool_free(pool->mpool, pool);
    }
}

int vox_db_pool_acquire_async(vox_db_pool_t* pool,
                             vox_db_pool_acquire_cb cb,
                             void* user_data) {
    if (!pool || !cb) return -1;

    if (vox_mutex_lock(&pool->mu) != 0) return -1;
    if (pool->destroyed) {
        vox_mutex_unlock(&pool->mu);
        return -1;
    }

    vox_db_conn_t* idle = pool_pop_idle_locked(pool);
    if (idle) {
        vox_mutex_unlock(&pool->mu);
        cb(pool, idle, 0, user_data);
        return 0;
    }

    /* 无空闲连接时先入队等待，由 release 时 pool_serve_one_waiter_locked 用空闲连接或按需创建临时连接服务。
     * 避免“无空闲就立即建临时连接”导致大量并发请求时创建过多连接（如 100 请求建 92 个临时连接）。 */
    acquire_waiter_t* w = (acquire_waiter_t*)vox_mpool_alloc(pool->mpool, sizeof(acquire_waiter_t));
    if (!w) {
        vox_mutex_unlock(&pool->mu);
        cb(pool, NULL, -1, user_data);
        return 0;
    }
    memset(w, 0, sizeof(*w));
    w->cb = cb;
    w->user_data = user_data;
    vox_list_push_back(&pool->waiting_list, &w->node);
    vox_mutex_unlock(&pool->mu);
    return 0;
}

void vox_db_pool_release(vox_db_pool_t* pool, vox_db_conn_t* conn) {
    if (!pool || !conn) return;

    if (vox_mutex_lock(&pool->mu) != 0) return;
    if (pool->destroyed) {
        vox_mutex_unlock(&pool->mu);
        return;
    }

    if (pool_is_initial_conn(pool, conn)) {
        pool_conn_node_t* cn = (pool_conn_node_t*)vox_mpool_alloc(pool->mpool, sizeof(pool_conn_node_t));
        if (cn) {
            memset(cn, 0, sizeof(*cn));
            cn->conn = conn;
            vox_list_push_back(&pool->idle_list, &cn->node);
        } else {
            /* OOM：无法放回空闲列表，断开并清空槽位避免连接泄漏 */
            vox_db_disconnect(conn);
            for (size_t i = 0; i < pool->initial_size; i++) {
                if (pool->conns[i] == conn) {
                    pool->conns[i] = NULL;
                    break;
                }
            }
        }
        pool_serve_one_waiter_locked(pool);
        vox_mutex_unlock(&pool->mu);
        return;
    }

    pool_conn_node_t* cn = pool_remove_temp_from_in_use_locked(pool, conn);
    vox_mutex_unlock(&pool->mu);
    if (cn)
        vox_mpool_free(pool->mpool, cn);
    vox_db_disconnect(conn);
    vox_mutex_lock(&pool->mu);
    pool_serve_one_waiter_locked(pool);
    vox_mutex_unlock(&pool->mu);
}

int vox_db_pool_set_callback_mode(vox_db_pool_t* pool, vox_db_callback_mode_t mode) {
    if (!pool) return -1;
    if (mode != VOX_DB_CALLBACK_WORKER && mode != VOX_DB_CALLBACK_LOOP) return -1;
    pool->cb_mode = mode;
    for (size_t i = 0; i < pool->initial_size; i++) {
        if (pool->conns[i])
            (void)vox_db_set_callback_mode(pool->conns[i], mode);
    }
    return 0;
}

vox_db_callback_mode_t vox_db_pool_get_callback_mode(vox_db_pool_t* pool) {
    return pool ? pool->cb_mode : VOX_DB_CALLBACK_WORKER;
}

size_t vox_db_pool_initial_size(vox_db_pool_t* pool) {
    return pool ? pool->initial_size : 0;
}

size_t vox_db_pool_max_size(vox_db_pool_t* pool) {
    return pool ? pool->max_size : 0;
}

size_t vox_db_pool_current_size(vox_db_pool_t* pool) {
    if (!pool) return 0;
    if (vox_mutex_lock(&pool->mu) != 0) return 0;
    size_t idle_count = pool_idle_count_locked(pool);
    size_t temp_count = pool_in_use_temp_count_locked(pool);
    size_t in_use_initial = pool->initial_size - idle_count;
    size_t total = idle_count + in_use_initial + temp_count;
    vox_mutex_unlock(&pool->mu);
    return total;
}

size_t vox_db_pool_available(vox_db_pool_t* pool) {
    if (!pool) return 0;
    if (vox_mutex_lock(&pool->mu) != 0) return 0;
    size_t n = pool_idle_count_locked(pool);
    vox_mutex_unlock(&pool->mu);
    return n;
}
/* ----- 便捷接口（内部借还连接） ----- */

typedef struct {
    vox_db_pool_t* pool;
    vox_db_conn_t* conn;  /* 用于 release */
    const char* sql;
    const vox_db_value_t* params;
    size_t nparams;
    vox_db_exec_cb user_cb;
    void* user_data;
} pool_exec_wrap_t;

static void pool_exec_cb(vox_db_conn_t* conn, int status, int64_t affected_rows, void* user_data) {
    pool_exec_wrap_t* w = (pool_exec_wrap_t*)user_data;
    if (!w || !w->pool) return;
    if (w->user_cb)
        w->user_cb(conn, status, affected_rows, w->user_data);
    vox_db_conn_end(conn);
    vox_db_pool_release(w->pool, w->conn);
    vox_mpool_free(w->pool->mpool, w);
}

static void pool_acquire_exec_cb(vox_db_pool_t* pool, vox_db_conn_t* conn, int status, void* user_data) {
    pool_exec_wrap_t* w = (pool_exec_wrap_t*)user_data;
    if (!w) return;
    if (status != 0 || !conn) {
        if (w->user_cb)
            w->user_cb(NULL, status, 0, w->user_data);
        vox_mpool_free(pool->mpool, w);
        return;
    }
    w->conn = conn;
    if (vox_db_exec_async(conn, w->sql, w->params, w->nparams, pool_exec_cb, w) != 0) {
        vox_db_pool_release(pool, conn);
        if (w->user_cb)
            w->user_cb(NULL, -1, 0, w->user_data);
        vox_mpool_free(pool->mpool, w);
    }
}

int vox_db_pool_exec_async(vox_db_pool_t* pool,
                           const char* sql,
                           const vox_db_value_t* params,
                           size_t nparams,
                           vox_db_exec_cb cb,
                           void* user_data) {
    if (!pool || !sql) return -1;

    pool_exec_wrap_t* w = (pool_exec_wrap_t*)vox_mpool_alloc(pool->mpool, sizeof(pool_exec_wrap_t));
    if (!w) return -1;
    memset(w, 0, sizeof(*w));
    w->pool = pool;
    w->sql = sql;
    w->params = params;
    w->nparams = nparams;
    w->user_cb = cb;
    w->user_data = user_data;

    if (vox_db_pool_acquire_async(pool, pool_acquire_exec_cb, w) != 0) {
        vox_mpool_free(pool->mpool, w);
        return -1;
    }
    return 0;
}

typedef struct {
    vox_db_pool_t* pool;
    vox_db_conn_t* conn;
    const char* sql;
    const vox_db_value_t* params;
    size_t nparams;
    vox_db_row_cb user_row_cb;
    vox_db_done_cb user_done_cb;
    void* user_data;
} pool_query_wrap_t;

static void pool_row_cb(vox_db_conn_t* conn, const vox_db_row_t* row, void* user_data) {
    pool_query_wrap_t* w = (pool_query_wrap_t*)user_data;
    if (w && w->user_row_cb)
        w->user_row_cb(conn, row, w->user_data);
}

static void pool_done_cb(vox_db_conn_t* conn, int status, int64_t row_count, void* user_data) {
    pool_query_wrap_t* w = (pool_query_wrap_t*)user_data;
    if (!w || !w->pool) return;
    if (w->user_done_cb)
        w->user_done_cb(conn, status, row_count, w->user_data);
    vox_db_conn_end(conn);
    vox_db_pool_release(w->pool, w->conn);
    /* 当 use_loop_thread_for_async 时，行回调在 db_row_dispatch 中已同步执行，done 时无待执行 row，可立即释放 w */
    vox_mpool_free(w->pool->mpool, w);
}

static void pool_acquire_query_cb(vox_db_pool_t* pool, vox_db_conn_t* conn, int status, void* user_data) {
    pool_query_wrap_t* w = (pool_query_wrap_t*)user_data;
    if (!w) return;
    if (status != 0 || !conn) {
        if (w->user_done_cb)
            w->user_done_cb(NULL, status, 0, w->user_data);
        vox_mpool_free(pool->mpool, w);
        return;
    }
    w->conn = conn;
    if (vox_db_query_async(conn, w->sql, w->params, w->nparams, pool_row_cb, pool_done_cb, w) != 0) {
        vox_db_pool_release(pool, conn);
        if (w->user_done_cb)
            w->user_done_cb(NULL, -1, 0, w->user_data);
        vox_mpool_free(pool->mpool, w);
    }
}

int vox_db_pool_query_async(vox_db_pool_t* pool,
                             const char* sql,
                             const vox_db_value_t* params,
                             size_t nparams,
                             vox_db_row_cb row_cb,
                             vox_db_done_cb done_cb,
                             void* user_data) {
    if (!pool || !sql) return -1;

    pool_query_wrap_t* w = (pool_query_wrap_t*)vox_mpool_alloc(pool->mpool, sizeof(pool_query_wrap_t));
    if (!w) return -1;
    memset(w, 0, sizeof(*w));
    w->pool = pool;
    w->sql = sql;
    w->params = params;
    w->nparams = nparams;
    w->user_row_cb = row_cb;
    w->user_done_cb = done_cb;
    w->user_data = user_data;

    if (vox_db_pool_acquire_async(pool, pool_acquire_query_cb, w) != 0) {
        vox_mpool_free(pool->mpool, w);
        return -1;
    }
    return 0;
}

vox_db_conn_t* vox_db_pool_acquire_sync(vox_db_pool_t* pool) {
    if (!pool) return NULL;
    for (;;) {
        if (vox_mutex_lock(&pool->mu) != 0) return NULL;
        if (pool->destroyed) {
            vox_mutex_unlock(&pool->mu);
            return NULL;
        }
        vox_db_conn_t* idle = pool_pop_idle_locked(pool);
        if (idle) {
            vox_mutex_unlock(&pool->mu);
            return idle;
        }
        size_t idle_count = pool_idle_count_locked(pool);
        size_t temp_count = pool_in_use_temp_count_locked(pool);
        size_t in_use_initial = pool->initial_size - idle_count;
        size_t total = idle_count + in_use_initial + temp_count;
        if (total + pool->pending_temp >= pool->max_size) {
            vox_mutex_unlock(&pool->mu);
            return NULL;
        }
        pool->pending_temp++;
        vox_mutex_unlock(&pool->mu);

        vox_db_conn_t* conn = vox_db_connect(pool->loop, pool->driver, pool->conninfo);
        if (!conn) {
            vox_mutex_lock(&pool->mu);
            pool->pending_temp--;
            vox_mutex_unlock(&pool->mu);
            return NULL;
        }
        (void)vox_db_set_callback_mode(conn, pool->cb_mode);

        vox_mutex_lock(&pool->mu);
        pool->pending_temp--;
        pool_conn_node_t* cn = (pool_conn_node_t*)vox_mpool_alloc(pool->mpool, sizeof(pool_conn_node_t));
        if (!cn) {
            vox_mutex_unlock(&pool->mu);
            vox_db_disconnect(conn);
            return NULL;
        }
        memset(cn, 0, sizeof(*cn));
        cn->conn = conn;
        vox_list_push_back(&pool->in_use_list, &cn->node);
        vox_mutex_unlock(&pool->mu);
        return conn;
    }
}

int vox_db_pool_exec(vox_db_pool_t* pool,
                     const char* sql,
                     const vox_db_value_t* params,
                     size_t nparams,
                     int64_t* out_affected_rows) {
    if (!pool || !sql) return -1;
    vox_db_conn_t* conn = vox_db_pool_acquire_sync(pool);
    if (!conn) return -1;
    int rc = vox_db_exec(conn, sql, params, nparams, out_affected_rows);
    vox_db_pool_release(pool, conn);
    return rc;
}

int vox_db_pool_query(vox_db_pool_t* pool,
                      const char* sql,
                      const vox_db_value_t* params,
                      size_t nparams,
                      vox_db_row_cb row_cb,
                      void* row_user_data,
                      int64_t* out_row_count) {
    if (!pool || !sql) return -1;
    vox_db_conn_t* conn = vox_db_pool_acquire_sync(pool);
    if (!conn) return -1;
    int rc = vox_db_query(conn, sql, params, nparams, row_cb, row_user_data, out_row_count);
    vox_db_pool_release(pool, conn);
    return rc;
}

