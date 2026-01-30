/*
 * vox_coroutine_redis.c - Redis协程适配器实现
 */

#include "vox_coroutine_redis.h"
#include "../vox_log.h"
#include "../vox_mpool.h"
#include <string.h>

/* 内部状态：回调中写入，await 返回后由协程读取 */
typedef struct {
    vox_coroutine_promise_t* promise;
    int status;
    vox_redis_response_t* out_response;  /* 成功时直接拷贝到此，避免二次 memcpy */
    vox_mpool_t* mpool;
    char error_message[256];
} vox_coroutine_redis_state_t;

/* 连接池 acquire 状态 */
typedef struct {
    vox_coroutine_promise_t* promise;
    int status;
    vox_redis_client_t* client;  /* 成功时由回调写入 */
} vox_coroutine_redis_pool_acquire_state_t;

/* ===== 回调函数 ===== */

static void redis_connect_cb(vox_redis_client_t* client, int status, void* user_data) {
    (void)client;
    vox_coroutine_redis_state_t* state = (vox_coroutine_redis_state_t*)user_data;
    state->status = status;
    vox_coroutine_promise_complete(state->promise, status, NULL);
}

static void redis_response_cb(vox_redis_client_t* client, 
                              const vox_redis_response_t* response,
                              void* user_data) {
    (void)client;
    vox_coroutine_redis_state_t* state = (vox_coroutine_redis_state_t*)user_data;
    
    if (state->out_response && state->mpool) {
        if (vox_redis_response_copy(state->mpool, response, state->out_response) < 0)
            state->status = -1;
        else
            state->status = 0;
    } else {
        state->status = 0;
    }
    vox_coroutine_promise_complete(state->promise, state->status, NULL);
}

static void redis_error_cb(vox_redis_client_t* client, 
                           const char* message,
                           void* user_data) {
    (void)client;
    vox_coroutine_redis_state_t* state = (vox_coroutine_redis_state_t*)user_data;
    state->status = -1;
    
    if (message) {
        strncpy(state->error_message, message, sizeof(state->error_message) - 1);
        state->error_message[sizeof(state->error_message) - 1] = '\0';
    }
    
    vox_coroutine_promise_complete(state->promise, -1, NULL);
}

/* 连接池 acquire 回调 */
static void redis_pool_acquire_cb(vox_redis_pool_t* pool,
                                  vox_redis_client_t* client,
                                  int status,
                                  void* user_data) {
    (void)pool;
    vox_coroutine_redis_pool_acquire_state_t* state = (vox_coroutine_redis_pool_acquire_state_t*)user_data;
    state->status = status;
    state->client = client;
    vox_coroutine_promise_complete(state->promise, status, NULL);
}

/* ===== 协程适配实现 ===== */

int vox_coroutine_redis_connect_await(vox_coroutine_t* co,
                                       vox_redis_client_t* client,
                                       const char* host,
                                       uint16_t port) {
    if (!co || !client || !host)
        return -1;

    vox_loop_t* loop = vox_coroutine_get_loop(co);
    if (!loop)
        return -1;

    vox_coroutine_redis_state_t state = {0};
    state.promise = vox_coroutine_promise_create(loop);
    if (!state.promise)
        return -1;

    if (vox_redis_client_connect(client, host, port, redis_connect_cb, &state) < 0) {
        vox_coroutine_promise_destroy(state.promise);
        return -1;
    }

    int ret = vox_coroutine_await(co, state.promise);
    vox_coroutine_promise_destroy(state.promise);
    return (ret == 0 && state.status == 0) ? 0 : -1;
}

int vox_coroutine_redis_command_await(vox_coroutine_t* co,
                                       vox_redis_client_t* client,
                                       int argc,
                                       const char** argv,
                                       vox_redis_response_t* out_response) {
    if (!co || !client || argc <= 0 || !argv || !out_response)
        return -1;

    vox_loop_t* loop = vox_coroutine_get_loop(co);
    if (!loop)
        return -1;

    vox_coroutine_redis_state_t state = {0};
    state.promise = vox_coroutine_promise_create(loop);
    state.mpool = vox_loop_get_mpool(loop);
    state.out_response = out_response;
    if (!state.promise)
        return -1;
    if (!state.mpool) {
        vox_coroutine_promise_destroy(state.promise);
        return -1;
    }

    if (vox_redis_client_commandv(client, redis_response_cb, redis_error_cb, &state, argc, argv) < 0) {
        vox_coroutine_promise_destroy(state.promise);
        return -1;
    }

    int ret = vox_coroutine_await(co, state.promise);
    vox_coroutine_promise_destroy(state.promise);
    return (ret == 0 && state.status == 0) ? 0 : -1;
}

/* ===== 连接池协程实现 ===== */

int vox_coroutine_redis_pool_acquire_await(vox_coroutine_t* co,
                                           vox_redis_pool_t* pool,
                                           vox_redis_client_t** out_client) {
    if (!co || !pool || !out_client)
        return -1;

    vox_loop_t* loop = vox_coroutine_get_loop(co);
    if (!loop)
        return -1;

    vox_coroutine_redis_pool_acquire_state_t state = {0};
    state.promise = vox_coroutine_promise_create(loop);
    if (!state.promise)
        return -1;

    if (vox_redis_pool_acquire_async(pool, redis_pool_acquire_cb, &state) != 0) {
        vox_coroutine_promise_destroy(state.promise);
        return -1;
    }

    int ret = vox_coroutine_await(co, state.promise);
    vox_coroutine_promise_destroy(state.promise);
    if (ret == 0 && state.status == 0 && state.client) {
        *out_client = state.client;
        return 0;
    }
    *out_client = NULL;
    return -1;
}

int vox_coroutine_redis_pool_command_await(vox_coroutine_t* co,
                                           vox_redis_pool_t* pool,
                                           int argc,
                                           const char** argv,
                                           vox_redis_response_t* out_response) {
    vox_redis_client_t* client = NULL;
    if (vox_coroutine_redis_pool_acquire_await(co, pool, &client) != 0)
        return -1;
    int ret = vox_coroutine_redis_command_await(co, client, argc, argv, out_response);
    vox_redis_pool_release(pool, client);
    return ret;
}

int vox_coroutine_redis_pool_ping_await(vox_coroutine_t* co,
                                         vox_redis_pool_t* pool,
                                         vox_redis_response_t* out_response) {
    const char* args[] = {"PING"};
    return vox_coroutine_redis_pool_command_await(co, pool, 1, args, out_response);
}

int vox_coroutine_redis_pool_get_await(vox_coroutine_t* co,
                                        vox_redis_pool_t* pool,
                                        const char* key,
                                        vox_redis_response_t* out_response) {
    const char* args[] = {"GET", key};
    return vox_coroutine_redis_pool_command_await(co, pool, 2, args, out_response);
}

int vox_coroutine_redis_pool_set_await(vox_coroutine_t* co,
                                        vox_redis_pool_t* pool,
                                        const char* key,
                                        const char* value,
                                        vox_redis_response_t* out_response) {
    const char* args[] = {"SET", key, value};
    return vox_coroutine_redis_pool_command_await(co, pool, 3, args, out_response);
}

int vox_coroutine_redis_pool_del_await(vox_coroutine_t* co,
                                        vox_redis_pool_t* pool,
                                        const char* key,
                                        vox_redis_response_t* out_response) {
    const char* args[] = {"DEL", key};
    return vox_coroutine_redis_pool_command_await(co, pool, 2, args, out_response);
}

/* ===== 便捷函数实现 ===== */

int vox_coroutine_redis_ping_await(vox_coroutine_t* co,
                                    vox_redis_client_t* client,
                                    vox_redis_response_t* out_response) {
    const char* args[] = {"PING"};
    return vox_coroutine_redis_command_await(co, client, 1, args, out_response);
}

int vox_coroutine_redis_get_await(vox_coroutine_t* co,
                                   vox_redis_client_t* client,
                                   const char* key,
                                   vox_redis_response_t* out_response) {
    const char* args[] = {"GET", key};
    return vox_coroutine_redis_command_await(co, client, 2, args, out_response);
}

int vox_coroutine_redis_set_await(vox_coroutine_t* co,
                                   vox_redis_client_t* client,
                                   const char* key,
                                   const char* value,
                                   vox_redis_response_t* out_response) {
    const char* args[] = {"SET", key, value};
    return vox_coroutine_redis_command_await(co, client, 3, args, out_response);
}

int vox_coroutine_redis_del_await(vox_coroutine_t* co,
                                   vox_redis_client_t* client,
                                   const char* key,
                                   vox_redis_response_t* out_response) {
    const char* args[] = {"DEL", key};
    return vox_coroutine_redis_command_await(co, client, 2, args, out_response);
}

int vox_coroutine_redis_exists_await(vox_coroutine_t* co,
                                      vox_redis_client_t* client,
                                      const char* key,
                                      vox_redis_response_t* out_response) {
    const char* args[] = {"EXISTS", key};
    return vox_coroutine_redis_command_await(co, client, 2, args, out_response);
}

int vox_coroutine_redis_incr_await(vox_coroutine_t* co,
                                    vox_redis_client_t* client,
                                    const char* key,
                                    vox_redis_response_t* out_response) {
    const char* args[] = {"INCR", key};
    return vox_coroutine_redis_command_await(co, client, 2, args, out_response);
}

int vox_coroutine_redis_decr_await(vox_coroutine_t* co,
                                    vox_redis_client_t* client,
                                    const char* key,
                                    vox_redis_response_t* out_response) {
    const char* args[] = {"DECR", key};
    return vox_coroutine_redis_command_await(co, client, 2, args, out_response);
}

/* ===== 哈希命令 ===== */

int vox_coroutine_redis_hset_await(vox_coroutine_t* co,
                                    vox_redis_client_t* client,
                                    const char* key,
                                    const char* field,
                                    const char* value,
                                    vox_redis_response_t* out_response) {
    const char* args[] = {"HSET", key, field, value};
    return vox_coroutine_redis_command_await(co, client, 4, args, out_response);
}

int vox_coroutine_redis_hget_await(vox_coroutine_t* co,
                                    vox_redis_client_t* client,
                                    const char* key,
                                    const char* field,
                                    vox_redis_response_t* out_response) {
    const char* args[] = {"HGET", key, field};
    return vox_coroutine_redis_command_await(co, client, 3, args, out_response);
}

int vox_coroutine_redis_hdel_await(vox_coroutine_t* co,
                                    vox_redis_client_t* client,
                                    const char* key,
                                    const char* field,
                                    vox_redis_response_t* out_response) {
    const char* args[] = {"HDEL", key, field};
    return vox_coroutine_redis_command_await(co, client, 3, args, out_response);
}

int vox_coroutine_redis_hexists_await(vox_coroutine_t* co,
                                       vox_redis_client_t* client,
                                       const char* key,
                                       const char* field,
                                       vox_redis_response_t* out_response) {
    const char* args[] = {"HEXISTS", key, field};
    return vox_coroutine_redis_command_await(co, client, 3, args, out_response);
}

/* ===== 列表命令 ===== */

int vox_coroutine_redis_lpush_await(vox_coroutine_t* co,
                                     vox_redis_client_t* client,
                                     const char* key,
                                     const char* value,
                                     vox_redis_response_t* out_response) {
    const char* args[] = {"LPUSH", key, value};
    return vox_coroutine_redis_command_await(co, client, 3, args, out_response);
}

int vox_coroutine_redis_rpush_await(vox_coroutine_t* co,
                                     vox_redis_client_t* client,
                                     const char* key,
                                     const char* value,
                                     vox_redis_response_t* out_response) {
    const char* args[] = {"RPUSH", key, value};
    return vox_coroutine_redis_command_await(co, client, 3, args, out_response);
}

int vox_coroutine_redis_lpop_await(vox_coroutine_t* co,
                                    vox_redis_client_t* client,
                                    const char* key,
                                    vox_redis_response_t* out_response) {
    const char* args[] = {"LPOP", key};
    return vox_coroutine_redis_command_await(co, client, 2, args, out_response);
}

int vox_coroutine_redis_rpop_await(vox_coroutine_t* co,
                                    vox_redis_client_t* client,
                                    const char* key,
                                    vox_redis_response_t* out_response) {
    const char* args[] = {"RPOP", key};
    return vox_coroutine_redis_command_await(co, client, 2, args, out_response);
}

int vox_coroutine_redis_llen_await(vox_coroutine_t* co,
                                    vox_redis_client_t* client,
                                    const char* key,
                                    vox_redis_response_t* out_response) {
    const char* args[] = {"LLEN", key};
    return vox_coroutine_redis_command_await(co, client, 2, args, out_response);
}

/* ===== 集合命令 ===== */

int vox_coroutine_redis_sadd_await(vox_coroutine_t* co,
                                    vox_redis_client_t* client,
                                    const char* key,
                                    const char* member,
                                    vox_redis_response_t* out_response) {
    const char* args[] = {"SADD", key, member};
    return vox_coroutine_redis_command_await(co, client, 3, args, out_response);
}

int vox_coroutine_redis_srem_await(vox_coroutine_t* co,
                                    vox_redis_client_t* client,
                                    const char* key,
                                    const char* member,
                                    vox_redis_response_t* out_response) {
    const char* args[] = {"SREM", key, member};
    return vox_coroutine_redis_command_await(co, client, 3, args, out_response);
}

int vox_coroutine_redis_smembers_await(vox_coroutine_t* co,
                                        vox_redis_client_t* client,
                                        const char* key,
                                        vox_redis_response_t* out_response) {
    const char* args[] = {"SMEMBERS", key};
    return vox_coroutine_redis_command_await(co, client, 2, args, out_response);
}

int vox_coroutine_redis_scard_await(vox_coroutine_t* co,
                                     vox_redis_client_t* client,
                                     const char* key,
                                     vox_redis_response_t* out_response) {
    const char* args[] = {"SCARD", key};
    return vox_coroutine_redis_command_await(co, client, 2, args, out_response);
}

int vox_coroutine_redis_sismember_await(vox_coroutine_t* co,
                                         vox_redis_client_t* client,
                                         const char* key,
                                         const char* member,
                                         vox_redis_response_t* out_response) {
    const char* args[] = {"SISMEMBER", key, member};
    return vox_coroutine_redis_command_await(co, client, 3, args, out_response);
}
