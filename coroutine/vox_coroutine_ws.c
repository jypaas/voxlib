/*
 * vox_coroutine_ws.c - WebSocket协程适配器实现
 * 基于 websocket 模块 (vox_ws_client_t)，非 HTTP 模块
 */

#include "vox_coroutine_ws.h"
#include "../vox_loop.h"
#include "../vox_log.h"
#include "../vox_mpool.h"
#include <string.h>

/* 消息队列节点 */
typedef struct vox_ws_msg_node {
    vox_coroutine_ws_message_t message;
    struct vox_ws_msg_node* next;
} vox_ws_msg_node_t;

/* 协程侧 WebSocket 客户端包装（内部持有 websocket 模块的 vox_ws_client_t） */
struct vox_coroutine_ws_client {
    vox_loop_t* loop;
    vox_ws_client_t* ws_client;  /* websocket 模块客户端 */
    vox_mpool_t* mpool;          /* 用于消息队列节点 */
    
    vox_ws_msg_node_t* msg_head;
    vox_ws_msg_node_t* msg_tail;
    vox_coroutine_promise_t* recv_promise;
    vox_coroutine_promise_t* connect_promise;  /* 仅连接阶段使用，完成后置 NULL */
    
    bool connected;
    bool closed;
    char error_message[256];
};

/* ===== websocket 模块回调（config.user_data = wrapper，即 vox_coroutine_ws_client_t*） ===== */

static void ws_on_connect(vox_ws_client_t* ws_client, void* user_data) {
    vox_coroutine_ws_client_t* wrapper = (vox_coroutine_ws_client_t*)user_data;
    wrapper->ws_client = ws_client;
    wrapper->connected = true;
    if (wrapper->connect_promise) {
        vox_coroutine_promise_complete(wrapper->connect_promise, 0, NULL);
        wrapper->connect_promise = NULL;
    }
}

static void ws_on_message(vox_ws_client_t* ws_client, const void* data, size_t len,
                          vox_ws_message_type_t type, void* user_data) {
    (void)ws_client;
    vox_coroutine_ws_client_t* wrapper = (vox_coroutine_ws_client_t*)user_data;
    
    vox_ws_msg_node_t* node = (vox_ws_msg_node_t*)vox_mpool_alloc(wrapper->mpool, sizeof(vox_ws_msg_node_t));
    if (!node) return;
    
    node->message.data = vox_mpool_alloc(wrapper->mpool, len);
    if (!node->message.data) {
        vox_mpool_free(wrapper->mpool, node);
        return;
    }
    memcpy(node->message.data, data, len);
    node->message.len = len;
    node->message.is_text = (type == VOX_WS_MSG_TEXT);
    node->next = NULL;
    
    if (wrapper->msg_tail)
        wrapper->msg_tail->next = node;
    else
        wrapper->msg_head = node;
    wrapper->msg_tail = node;
    
    if (wrapper->recv_promise) {
        vox_coroutine_promise_complete(wrapper->recv_promise, 0, NULL);
        wrapper->recv_promise = NULL;
    }
}

static void ws_on_close(vox_ws_client_t* ws_client, uint16_t code, const char* reason, void* user_data) {
    (void)ws_client;
    (void)code;
    (void)reason;
    vox_coroutine_ws_client_t* wrapper = (vox_coroutine_ws_client_t*)user_data;
    wrapper->closed = true;
    if (wrapper->recv_promise) {
        vox_coroutine_promise_complete(wrapper->recv_promise, 1, NULL);
        wrapper->recv_promise = NULL;
    }
}

static void ws_on_error(vox_ws_client_t* ws_client, const char* error, void* user_data) {
    (void)ws_client;
    vox_coroutine_ws_client_t* wrapper = (vox_coroutine_ws_client_t*)user_data;
    if (error) {
        size_t n = strlen(error);
        if (n >= sizeof(wrapper->error_message)) n = sizeof(wrapper->error_message) - 1;
        memcpy(wrapper->error_message, error, n);
        wrapper->error_message[n] = '\0';
    }
    if (wrapper->connect_promise) {
        vox_coroutine_promise_complete(wrapper->connect_promise, -1, NULL);
        wrapper->connect_promise = NULL;
    } else if (wrapper->recv_promise) {
        vox_coroutine_promise_complete(wrapper->recv_promise, -1, NULL);
        wrapper->recv_promise = NULL;
    }
}

/* 延迟启动连接：在 loop 下一次迭代中调用，确保协程已 yield 并设置 promise->waiting_coroutine */
static void deferred_connect_cb(vox_loop_t* loop, void* user_data) {
    vox_coroutine_ws_client_t* wrapper = (vox_coroutine_ws_client_t*)user_data;
    (void)loop;
    if (!wrapper || !wrapper->ws_client) return;
    if (vox_ws_client_connect(wrapper->ws_client) != 0 && wrapper->connect_promise) {
        vox_coroutine_promise_complete(wrapper->connect_promise, -1, NULL);
        wrapper->connect_promise = NULL;
    }
}

/* ===== 协程适配实现 ===== */

int vox_coroutine_ws_connect_await(vox_coroutine_t* co,
                                    vox_loop_t* loop,
                                    const char* url,
                                    vox_coroutine_ws_client_t** out_client) {
    if (!co || !loop || !url || !out_client) return -1;

    vox_mpool_t* mpool = vox_mpool_create();
    if (!mpool) return -1;

    vox_coroutine_ws_client_t* client = (vox_coroutine_ws_client_t*)vox_mpool_alloc(mpool, sizeof(vox_coroutine_ws_client_t));
    if (!client) {
        vox_mpool_destroy(mpool);
        return -1;
    }
    memset(client, 0, sizeof(vox_coroutine_ws_client_t));
    client->loop = loop;
    client->mpool = mpool;

    client->connect_promise = vox_coroutine_promise_create(loop);
    if (!client->connect_promise) {
        vox_mpool_free(mpool, client);
        vox_mpool_destroy(mpool);
        return -1;
    }

    vox_ws_client_config_t config = {0};
    config.loop = loop;
    config.url = url;
    config.on_connect = ws_on_connect;
    config.on_message = ws_on_message;
    config.on_close = ws_on_close;
    config.on_error = ws_on_error;
    config.user_data = client;

    vox_ws_client_t* ws_client = vox_ws_client_create(&config);
    if (!ws_client) {
        vox_coroutine_promise_destroy(client->connect_promise);
        vox_mpool_free(mpool, client);
        vox_mpool_destroy(mpool);
        return -1;
    }
    client->ws_client = ws_client;

    /* 先 yield 并设置 promise->waiting_coroutine，再在下一轮 loop 中启动连接，避免 on_connect 在 await 之前同步触发 */
    if (vox_loop_queue_work(loop, deferred_connect_cb, client) != 0) {
        vox_ws_client_destroy(ws_client);
        vox_coroutine_promise_destroy(client->connect_promise);
        vox_mpool_free(mpool, client);
        vox_mpool_destroy(mpool);
        return -1;
    }

    int ret = vox_coroutine_await(co, client->connect_promise);
    vox_coroutine_promise_destroy(client->connect_promise);
    client->connect_promise = NULL;

    if (ret == 0 && client->connected) {
        *out_client = client;
        return 0;
    }
    vox_ws_client_destroy(ws_client);
    vox_mpool_free(mpool, client);
    vox_mpool_destroy(mpool);
    return -1;
}

int vox_coroutine_ws_recv_await(vox_coroutine_t* co,
                                 vox_coroutine_ws_client_t* client,
                                 vox_coroutine_ws_message_t* out_message) {
    if (!co || !client || !out_message) {
        return -1;
    }

    /* 检查是否已关闭 */
    if (client->closed) {
        return 1;
    }

    /* 检查消息队列 */
    if (client->msg_head) {
        /* 从队列取出消息 */
        vox_ws_msg_node_t* node = client->msg_head;
        client->msg_head = node->next;
        if (!client->msg_head) {
            client->msg_tail = NULL;
        }
        
        *out_message = node->message;
        vox_mpool_free(client->mpool, node);
        return 0;
    }

    /* 队列为空，等待新消息 */
    vox_loop_t* loop = vox_coroutine_get_loop(co);
    client->recv_promise = vox_coroutine_promise_create(loop);
    if (!client->recv_promise) {
        return -1;
    }

    /* 等待消息到达 */
    int ret = vox_coroutine_await(co, client->recv_promise);
    
    vox_coroutine_promise_destroy(client->recv_promise);
    client->recv_promise = NULL;

    /* 检查是否是关闭事件 */
    if (ret == 1 || client->closed) {
        return 1;
    }

    if (ret < 0) {
        return -1;
    }

    /* 再次尝试从队列取消息 */
    if (client->msg_head) {
        vox_ws_msg_node_t* node = client->msg_head;
        client->msg_head = node->next;
        if (!client->msg_head) {
            client->msg_tail = NULL;
        }
        
        *out_message = node->message;
        vox_mpool_free(client->mpool, node);
        return 0;
    }

    return -1;
}

int vox_coroutine_ws_send_text_await(vox_coroutine_t* co,
                                      vox_coroutine_ws_client_t* client,
                                      const char* text,
                                      size_t len) {
    (void)co;
    if (!client || !text || !client->ws_client) return -1;
    return vox_ws_client_send_text(client->ws_client, text, len);
}

int vox_coroutine_ws_send_binary_await(vox_coroutine_t* co,
                                        vox_coroutine_ws_client_t* client,
                                        const void* data,
                                        size_t len) {
    (void)co;
    if (!client || !data || !client->ws_client) return -1;
    return vox_ws_client_send_binary(client->ws_client, data, len);
}

int vox_coroutine_ws_close_await(vox_coroutine_t* co,
                                  vox_coroutine_ws_client_t* client,
                                  int code,
                                  const char* reason) {
    (void)co;
    if (!client || !client->ws_client) return -1;
    return vox_ws_client_close(client->ws_client, (uint16_t)code, reason);
}

void vox_coroutine_ws_disconnect(vox_coroutine_ws_client_t* client) {
    if (!client) return;

    if (client->ws_client) {
        vox_ws_client_destroy(client->ws_client);
        client->ws_client = NULL;
    }

    vox_mpool_t* mpool = client->mpool;
    vox_ws_msg_node_t* node = client->msg_head;
    while (node) {
        vox_ws_msg_node_t* next = node->next;
        if (node->message.data)
            vox_mpool_free(mpool, node->message.data);
        vox_mpool_free(mpool, node);
        node = next;
    }
    client->msg_head = NULL;
    client->msg_tail = NULL;

    vox_mpool_free(mpool, client);
    vox_mpool_destroy(mpool);
}

void vox_coroutine_ws_message_free(vox_coroutine_ws_message_t* message) {
    if (!message) {
        return;
    }

    /* 注意：消息数据由客户端的内存池管理，在disconnect时统一释放 */
    /* 这里只是标记为无效，不实际释放 */
    message->data = NULL;
    message->len = 0;
}
