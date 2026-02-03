/*
 * vox_redis_pool.c - Redis 连接池（纯连接管理）
 *
 * 初始连接数 + 最大连接数；超过初始部分的为临时连接，归还时自动关闭并移除。
 * 仅提供 acquire_async / release，不执行 Redis 命令。
 */

#include "vox_redis_pool.h"
#include "../vox_log.h"
#include "../vox_mutex.h"
#include "../vox_list.h"

#include <string.h>

struct vox_redis_pool {
    vox_loop_t* loop;
    vox_mpool_t* mpool;

    char* host;
    uint16_t port;

    size_t initial_size;
    size_t max_size;
    vox_redis_client_t** clients;   /* [initial_size] 常驻连接 */

    vox_list_t idle_list;            /* 空闲的初始连接 (pool_client_node_t)，最多 initial_size 个 */
    vox_list_t in_use_list;          /* 正在使用的临时连接 (pool_client_node_t)，仅临时连接 */
    vox_list_t waiting_list;         /* 等待取连接的请求 (acquire_waiter_t) */

    size_t initial_done;             /* 已完成连接尝试的初始连接数 */
    size_t pending_temp;             /* 正在创建中的临时连接数（未入 in_use_list），用于限制不超过 max_size */
    bool connect_cb_fired;
    bool destroyed;

    vox_mutex_t mu;

    vox_redis_pool_connect_cb connect_cb;
    void* connect_user_data;
};

typedef struct pool_client_node {
    vox_list_node_t node;
    vox_redis_client_t* client;
} pool_client_node_t;

typedef struct acquire_waiter {
    vox_list_node_t node;
    vox_redis_pool_acquire_cb cb;
    void* user_data;
} acquire_waiter_t;

/* ----- 内部辅助 ----- */

/* 获取空闲链表大小（O(1)） */
static inline size_t pool_idle_count_locked(vox_redis_pool_t* pool) {
    return vox_list_size(&pool->idle_list);
}

/* 获取使用中的临时连接数（O(1)） */
static inline size_t pool_in_use_temp_count_locked(vox_redis_pool_t* pool) {
    return vox_list_size(&pool->in_use_list);
}

/* 判断是否是初始连接：通过空闲链表大小判断（O(1)）
 * 如果空闲链表未满，说明还有初始连接在使用，归还的连接一定是初始连接
 * 如果空闲链表已满，说明所有初始连接都空闲了，归还的连接一定是临时连接
 */
static inline bool VOX_UNUSED_FUNC pool_is_initial_client_by_idle(vox_redis_pool_t* pool) {
    return pool_idle_count_locked(pool) < pool->initial_size;
}

/* 从使用中临时连接链表移除指定 client（O(n) 但临时连接通常不多） */
static pool_client_node_t* pool_remove_temp_from_in_use_locked(vox_redis_pool_t* pool, vox_redis_client_t* client) {
    vox_list_node_t* cur;
    vox_list_for_each(cur, &pool->in_use_list) {
        pool_client_node_t* cn = vox_container_of(cur, pool_client_node_t, node);
        if (cn->client == client) {
            vox_list_remove(&pool->in_use_list, cur);
            return cn;
        }
    }
    return NULL;
}

/* 从空闲列表弹出一个客户端；无空闲返回 NULL（初始连接不需要加入 in_use_list） */
static vox_redis_client_t* pool_pop_idle_locked(vox_redis_pool_t* pool) {
    if (vox_list_empty(&pool->idle_list))
        return NULL;
    vox_list_node_t* node = vox_list_pop_front(&pool->idle_list);
    pool_client_node_t* cn = vox_container_of(node, pool_client_node_t, node);
    vox_redis_client_t* c = cn->client;
    
    /* 释放节点（初始连接在使用中时不需要节点，归还时再分配） */
    vox_mpool_free(pool->mpool, cn);
    return c;
}

/* 向空闲列表压入一个连接；在锁内调用。如果空闲链表已满（达到 initial_size），返回 -1 */
static int pool_push_idle_locked(vox_redis_pool_t* pool, vox_redis_client_t* client) {
    size_t idle_count = pool_idle_count_locked(pool);
    if (idle_count >= pool->initial_size) {
        /* 空闲链表已满，说明这是临时连接，不应该加入空闲链表 */
        return -1;
    }
    pool_client_node_t* cn = (pool_client_node_t*)vox_mpool_alloc(pool->mpool, sizeof(pool_client_node_t));
    if (!cn) return -1;
    memset(cn, 0, sizeof(*cn));
    cn->client = client;
    vox_list_push_back(&pool->idle_list, &cn->node);
    return 0;
}

/* 尝试满足一个等待者：给一个空闲连接或启动一个临时连接。在锁内调用。 */
static void pool_serve_one_waiter_locked(vox_redis_pool_t* pool);

/* 临时连接创建完成（成功或失败）后回调 */
typedef struct temp_connect_ctx {
    vox_redis_pool_t* pool;
    acquire_waiter_t waiter;
} temp_connect_ctx_t;

static void pool_temp_connect_cb(vox_redis_client_t* client, int status, void* user_data) {
    temp_connect_ctx_t* ctx = (temp_connect_ctx_t*)user_data;
    vox_redis_pool_t* pool = ctx->pool;
    vox_redis_pool_acquire_cb cb = ctx->waiter.cb;
    void* ud = ctx->waiter.user_data;

    if (vox_mutex_lock(&pool->mu) != 0) {
        if (client && status == 0) vox_redis_client_destroy(client);
        if (cb) cb(pool, NULL, -1, ud);
        vox_mpool_free(pool->mpool, ctx);
        return;
    }
    if (pool->destroyed) {
        pool->pending_temp--;
        vox_mutex_unlock(&pool->mu);
        if (client && status == 0) vox_redis_client_destroy(client);
        if (cb) cb(pool, NULL, -1, ud);
        vox_mpool_free(pool->mpool, ctx);
        return;
    }

    if (status != 0) {
        /* 连接失败，释放预留槽位 */
        pool->pending_temp--;
        vox_mutex_unlock(&pool->mu);
        if (client) vox_redis_client_destroy(client);
        if (cb) cb(pool, NULL, status, ud);
        vox_mpool_free(pool->mpool, ctx);
        vox_mutex_lock(&pool->mu);
        pool_serve_one_waiter_locked(pool);
        vox_mutex_unlock(&pool->mu);
        return;
    }

    /* 成功：临时连接创建成功，加入使用中链表 */
    pool->pending_temp--;
    pool_client_node_t* cn = (pool_client_node_t*)vox_mpool_alloc(pool->mpool, sizeof(pool_client_node_t));
    if (!cn) {
        vox_mutex_unlock(&pool->mu);
        vox_redis_client_destroy(client);
        if (cb) cb(pool, NULL, -1, ud);
        vox_mpool_free(pool->mpool, ctx);
        vox_mutex_lock(&pool->mu);
        pool_serve_one_waiter_locked(pool);
        vox_mutex_unlock(&pool->mu);
        return;
    }
    memset(cn, 0, sizeof(*cn));
    cn->client = client;
    vox_list_push_back(&pool->in_use_list, &cn->node);
    vox_mutex_unlock(&pool->mu);

    if (cb)
        cb(pool, client, 0, ud);
    vox_mpool_free(pool->mpool, ctx);

    vox_mutex_lock(&pool->mu);
    pool_serve_one_waiter_locked(pool);
    vox_mutex_unlock(&pool->mu);
}

static void pool_serve_one_waiter_locked(vox_redis_pool_t* pool) {
    if (pool->destroyed || vox_list_empty(&pool->waiting_list))
        return;

    vox_redis_client_t* idle = pool_pop_idle_locked(pool);
    if (idle) {
        /* 已从 idle_list 移到 in_use_list */
        vox_list_node_t* wn = vox_list_pop_front(&pool->waiting_list);
        acquire_waiter_t* w = vox_container_of(wn, acquire_waiter_t, node);
        vox_redis_pool_acquire_cb cb = w->cb;
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
        ctx->waiter.cb = w->cb;
        ctx->waiter.user_data = w->user_data;
        vox_mpool_free(pool->mpool, w);

        vox_redis_client_t* client = vox_redis_client_create(pool->loop);
        if (!client) {
            pool->pending_temp--;
            if (ctx->waiter.cb)
                ctx->waiter.cb(pool, NULL, -1, ctx->waiter.user_data);
            vox_mpool_free(pool->mpool, ctx);
            return;
        }
        vox_mutex_unlock(&pool->mu);
        if (vox_redis_client_connect(client, pool->host, pool->port, pool_temp_connect_cb, ctx) != 0) {
            vox_mutex_lock(&pool->mu);
            pool->pending_temp--;
            vox_mutex_unlock(&pool->mu);
            vox_redis_client_destroy(client);
            if (ctx->waiter.cb)
                ctx->waiter.cb(pool, NULL, -1, ctx->waiter.user_data);
            vox_mpool_free(pool->mpool, ctx);
            vox_mutex_lock(&pool->mu);
            pool_serve_one_waiter_locked(pool);
            vox_mutex_unlock(&pool->mu);
            return;
        }
        vox_mutex_lock(&pool->mu);
        return;
    }
}

static void pool_initial_connect_cb(vox_redis_client_t* client, int status, void* user_data) {
    vox_redis_pool_t* pool = (vox_redis_pool_t*)user_data;
    if (!pool || pool->destroyed) return;

    if (vox_mutex_lock(&pool->mu) != 0) return;

    pool->initial_done++;
    if (status == 0) {
        /* 初始连接建立成功，加入空闲列表 */
        if (pool_push_idle_locked(pool, client) != 0) {
            /* OOM：无法加入空闲列表，销毁连接避免泄漏 */
            for (size_t i = 0; i < pool->initial_size; i++) {
                if (pool->clients[i] == client) {
                    pool->clients[i] = NULL;
                    break;
                }
            }
            vox_redis_client_destroy(client);
        }
    } else {
        /* 初始连接失败，销毁并清空槽位 */
        for (size_t i = 0; i < pool->initial_size; i++) {
            if (pool->clients[i] == client) {
                pool->clients[i] = NULL;
                break;
            }
        }
        vox_redis_client_destroy(client);
    }

    bool all_done = (pool->initial_done >= pool->initial_size);
    bool fire_cb = all_done && !pool->connect_cb_fired && pool->connect_cb;
    if (fire_cb)
        pool->connect_cb_fired = true;

    vox_redis_pool_connect_cb cb = pool->connect_cb;
    void* cb_data = pool->connect_user_data;

    /* 至少有一个初始连接成功则 final_status=0（成功时已 push 到 idle_list） */
    int final_status = vox_list_empty(&pool->idle_list) ? -1 : 0;

    vox_mutex_unlock(&pool->mu);

    if (fire_cb && cb)
        cb(pool, final_status, cb_data);

    /* 可能已有等待者，尝试分配 */
    vox_mutex_lock(&pool->mu);
    pool_serve_one_waiter_locked(pool);
    vox_mutex_unlock(&pool->mu);
}

/* ----- 公共 API ----- */

vox_redis_pool_t* vox_redis_pool_create(vox_loop_t* loop,
                                        const char* host,
                                        uint16_t port,
                                        size_t initial_size,
                                        size_t max_size,
                                        vox_redis_pool_connect_cb connect_cb,
                                        void* user_data) {
    if (!loop || !host || initial_size == 0 || max_size < initial_size)
        return NULL;

    vox_mpool_t* mpool = vox_loop_get_mpool(loop);
    if (!mpool) return NULL;

    vox_redis_pool_t* pool = (vox_redis_pool_t*)vox_mpool_alloc(mpool, sizeof(vox_redis_pool_t));
    if (!pool) return NULL;
    memset(pool, 0, sizeof(*pool));

    pool->loop = loop;
    pool->mpool = mpool;
    pool->port = port;
    pool->initial_size = initial_size;
    pool->max_size = max_size;
    pool->connect_cb = connect_cb;
    pool->connect_user_data = user_data;

    if (vox_mutex_create(&pool->mu) != 0) {
        vox_mpool_free(mpool, pool);
        return NULL;
    }
    vox_list_init(&pool->idle_list);
    vox_list_init(&pool->in_use_list);
    vox_list_init(&pool->waiting_list);

    size_t host_len = strlen(host) + 1;
    pool->host = (char*)vox_mpool_alloc(mpool, host_len);
    pool->clients = (vox_redis_client_t**)vox_mpool_alloc(mpool, sizeof(vox_redis_client_t*) * initial_size);
    if (!pool->host || !pool->clients) {
        vox_redis_pool_destroy(pool);
        return NULL;
    }
    memcpy(pool->host, host, host_len);
    memset(pool->clients, 0, sizeof(vox_redis_client_t*) * initial_size);

    for (size_t i = 0; i < initial_size; i++) {
        vox_redis_client_t* client = vox_redis_client_create(loop);
        if (!client) {
            VOX_LOG_ERROR("[redis/pool] create client %zu failed", i);
            vox_redis_pool_destroy(pool);
            return NULL;
        }
        pool->clients[i] = client;
        if (vox_redis_client_connect(client, host, port, pool_initial_connect_cb, pool) != 0) {
            VOX_LOG_ERROR("[redis/pool] connect client %zu failed", i);
            vox_redis_pool_destroy(pool);
            return NULL;
        }
    }
    return pool;
}

void vox_redis_pool_destroy(vox_redis_pool_t* pool) {
    if (!pool) return;

    if (vox_mutex_lock(&pool->mu) != 0) return;
    pool->destroyed = true;

    /* 清空等待队列：可选在此对未完成的 acquire 做失败回调；此处简化，不回调避免 use-after-free */
    while (!vox_list_empty(&pool->waiting_list)) {
        vox_list_node_t* wn = vox_list_pop_front(&pool->waiting_list);
        acquire_waiter_t* w = vox_container_of(wn, acquire_waiter_t, node);
        if (w->cb)
            w->cb(pool, NULL, -1, w->user_data);
        vox_mpool_free(pool->mpool, w);
    }

    /* 清空空闲列表（仅节点，不关 client，下面统一关） */
    while (!vox_list_empty(&pool->idle_list)) {
        vox_list_node_t* node = vox_list_pop_front(&pool->idle_list);
        pool_client_node_t* cn = vox_container_of(node, pool_client_node_t, node);
        vox_mpool_free(pool->mpool, cn);
    }

    /* 清空使用中列表（临时连接需要销毁，初始连接在 clients 数组中统一销毁） */
    while (!vox_list_empty(&pool->in_use_list)) {
        vox_list_node_t* node = vox_list_pop_front(&pool->in_use_list);
        pool_client_node_t* cn = vox_container_of(node, pool_client_node_t, node);
        /* 判断是否是临时连接：如果不在 clients 数组中，就是临时连接 */
        bool is_temp = true;
        for (size_t i = 0; i < pool->initial_size; i++) {
            if (pool->clients[i] == cn->client) {
                is_temp = false;
                break;
            }
        }
        if (is_temp) {
            vox_redis_client_destroy(cn->client);
        }
        vox_mpool_free(pool->mpool, cn);
    }

    vox_mutex_unlock(&pool->mu);

    for (size_t i = 0; i < pool->initial_size; i++) {
        if (pool->clients[i]) {
            vox_redis_client_destroy(pool->clients[i]);
            pool->clients[i] = NULL;
        }
    }

    vox_mutex_destroy(&pool->mu);
    if (pool->mpool) {
        if (pool->host) vox_mpool_free(pool->mpool, pool->host);
        if (pool->clients) vox_mpool_free(pool->mpool, pool->clients);
        vox_mpool_free(pool->mpool, pool);
    }
}

int vox_redis_pool_acquire_async(vox_redis_pool_t* pool,
                                 vox_redis_pool_acquire_cb cb,
                                 void* user_data) {
    if (!pool || !cb) return -1;

    if (vox_mutex_lock(&pool->mu) != 0) return -1;
    if (pool->destroyed) {
        vox_mutex_unlock(&pool->mu);
        return -1;
    }

    vox_redis_client_t* idle = pool_pop_idle_locked(pool);
    if (idle) {
        vox_mutex_unlock(&pool->mu);
        cb(pool, idle, 0, user_data);
        return 0;
    }

    /* 无空闲连接时先入队等待，由 release 时 pool_serve_one_waiter_locked 用空闲连接或按需创建临时连接服务。
     * 避免“无空闲就立即建临时连接”导致大量并发请求时创建过多连接（与 db pool 行为一致）。 */
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

void vox_redis_pool_release(vox_redis_pool_t* pool, vox_redis_client_t* client) {
    if (!pool || !client) return;

    if (vox_mutex_lock(&pool->mu) != 0) return;
    if (pool->destroyed) {
        vox_mutex_unlock(&pool->mu);
        return;
    }

    /* 判断是初始连接还是临时连接（O(1) - 通过空闲链表大小判断） */
    size_t idle_count = pool_idle_count_locked(pool);
    if (idle_count < pool->initial_size) {
        /* 空闲链表未满，说明这是初始连接，加入空闲链表 */
        pool_client_node_t* cn = (pool_client_node_t*)vox_mpool_alloc(pool->mpool, sizeof(pool_client_node_t));
        if (cn) {
            memset(cn, 0, sizeof(*cn));
            cn->client = client;
            vox_list_push_back(&pool->idle_list, &cn->node);
        } else {
            /* OOM：无法放回空闲列表，断开并清空槽位避免连接泄漏 */
            for (size_t i = 0; i < pool->initial_size; i++) {
                if (pool->clients[i] == client) {
                    pool->clients[i] = NULL;
                    break;
                }
            }
            vox_redis_client_destroy(client);
        }
        pool_serve_one_waiter_locked(pool);
        vox_mutex_unlock(&pool->mu);
        return;
    }

    /* 空闲链表已满，说明这是临时连接，从 in_use_list 移除并销毁 */
    pool_client_node_t* cn = pool_remove_temp_from_in_use_locked(pool, client);
    vox_mutex_unlock(&pool->mu);
    if (cn) {
        vox_mpool_free(pool->mpool, cn);
    }
    vox_redis_client_destroy(client);
    vox_mutex_lock(&pool->mu);
    pool_serve_one_waiter_locked(pool);
    vox_mutex_unlock(&pool->mu);
}

size_t vox_redis_pool_initial_size(vox_redis_pool_t* pool) {
    return pool ? pool->initial_size : 0;
}

size_t vox_redis_pool_max_size(vox_redis_pool_t* pool) {
    return pool ? pool->max_size : 0;
}

size_t vox_redis_pool_current_size(vox_redis_pool_t* pool) {
    if (!pool) return 0;
    if (vox_mutex_lock(&pool->mu) != 0) return 0;
    size_t idle_count = pool_idle_count_locked(pool);
    size_t temp_count = pool_in_use_temp_count_locked(pool);
    /* 总连接数 = 空闲初始连接 + 使用中的初始连接 + 临时连接 */
    /* 使用中的初始连接数 = initial_size - idle_count */
    size_t in_use_initial = pool->initial_size - idle_count;
    size_t total = idle_count + in_use_initial + temp_count;
    vox_mutex_unlock(&pool->mu);
    return total;
}

size_t vox_redis_pool_available(vox_redis_pool_t* pool) {
    if (!pool) return 0;
    if (vox_mutex_lock(&pool->mu) != 0) return 0;
    size_t n = pool_idle_count_locked(pool);
    vox_mutex_unlock(&pool->mu);
    return n;
}
