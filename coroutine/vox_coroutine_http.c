/*
 * vox_coroutine_http.c - HTTP协程适配器实现
 */

#include "vox_coroutine_http.h"
#include "../vox_log.h"
#include "../vox_string.h"
#include "../vox_mpool.h"
#include <string.h>

/* 内部状态结构 */
typedef struct {
    vox_coroutine_promise_t* promise;
    vox_coroutine_http_response_t* response;
    vox_mpool_t* mpool;
    vox_string_t* body_buffer;  /* 使用指针而不是嵌入对象 */
    bool headers_allocated;
    size_t header_capacity;
} vox_coroutine_http_state_t;

/* ===== 回调函数 ===== */

static void http_on_status(vox_http_client_req_t* req, int status_code, int http_major, int http_minor, void* user_data) {
    (void)req;
    vox_coroutine_http_state_t* state = (vox_coroutine_http_state_t*)user_data;
    state->response->status_code = status_code;
    state->response->http_major = http_major;
    state->response->http_minor = http_minor;
}

static void http_on_header(vox_http_client_req_t* req, vox_strview_t name, vox_strview_t value, void* user_data) {
    (void)req;
    vox_coroutine_http_state_t* state = (vox_coroutine_http_state_t*)user_data;
    
    /* 扩展headers数组 */
    if (state->response->header_count >= state->header_capacity) {
        size_t new_capacity = state->header_capacity == 0 ? 8 : state->header_capacity * 2;
        vox_http_client_header_t* new_headers = (vox_http_client_header_t*)vox_mpool_realloc(
            state->mpool,
            state->response->headers,
            new_capacity * sizeof(vox_http_client_header_t)
        );
        if (!new_headers) {
            return;
        }
        state->response->headers = new_headers;
        state->header_capacity = new_capacity;
    }
    
    /* 复制header：name 与 value 都成功才计入，否则释放已分配避免泄漏 */
    size_t idx = state->response->header_count;
    
    char* name_copy = (char*)vox_mpool_alloc(state->mpool, name.len + 1);
    if (!name_copy) return;
    memcpy(name_copy, name.ptr, name.len);
    name_copy[name.len] = '\0';
    
    char* value_copy = (char*)vox_mpool_alloc(state->mpool, value.len + 1);
    if (!value_copy) {
        vox_mpool_free(state->mpool, name_copy);
        return;
    }
    memcpy(value_copy, value.ptr, value.len);
    value_copy[value.len] = '\0';
    
    state->response->headers[idx].name = name_copy;
    state->response->headers[idx].value = value_copy;
    state->response->header_count++;
}

static void http_on_body(vox_http_client_req_t* req, const void* data, size_t len, void* user_data) {
    (void)req;
    vox_coroutine_http_state_t* state = (vox_coroutine_http_state_t*)user_data;
    
    /* 累积body数据 */
    if (!state->body_buffer) {
        state->body_buffer = vox_string_create(state->mpool);
        if (!state->body_buffer) {
            return;
        }
    }
    vox_string_append_data(state->body_buffer, (const char*)data, len);
}

static void http_on_complete(vox_http_client_req_t* req, int status, void* user_data) {
    (void)req;
    vox_coroutine_http_state_t* state = (vox_coroutine_http_state_t*)user_data;
    
    /* 设置body */
    if (state->body_buffer) {
        size_t body_len = vox_string_length(state->body_buffer);
        const void* body_data = vox_string_data(state->body_buffer);
        
        state->response->body_len = body_len;
        state->response->body = vox_mpool_alloc(state->mpool, body_len);
        if (state->response->body) {
            memcpy(state->response->body, body_data, body_len);
        }
        vox_string_destroy(state->body_buffer);
        state->body_buffer = NULL;
    }
    
    vox_coroutine_promise_complete(state->promise, status, NULL);
}

static void http_on_error(vox_http_client_req_t* req, const char* message, void* user_data) {
    (void)req;
    vox_coroutine_http_state_t* state = (vox_coroutine_http_state_t*)user_data;
    
    if (message) {
        size_t len = strlen(message);
        state->response->error_message = (char*)vox_mpool_alloc(state->mpool, len + 1);
        if (state->response->error_message) {
            memcpy(state->response->error_message, message, len + 1);
        }
    }
    
    vox_coroutine_promise_complete(state->promise, -1, NULL);
}

/* ===== 协程适配实现 ===== */

int vox_coroutine_http_request_await(vox_coroutine_t* co,
                                      vox_http_client_t* client,
                                      const vox_http_client_request_t* request,
                                      vox_coroutine_http_response_t* out_response) {
    if (!co || !client || !request || !out_response) {
        return -1;
    }

    vox_loop_t* loop = vox_coroutine_get_loop(co);
    vox_coroutine_http_state_t state = {0};
    state.promise = vox_coroutine_promise_create(loop);
    state.response = out_response;
    state.mpool = vox_loop_get_mpool(loop);
    if (!state.promise) {
        return -1;
    }

    /* 清空响应结构 */
    memset(out_response, 0, sizeof(vox_coroutine_http_response_t));

    /* 设置回调 */
    vox_http_client_callbacks_t cbs = {0};
    cbs.on_status = http_on_status;
    cbs.on_header = http_on_header;
    cbs.on_body = http_on_body;
    cbs.on_complete = http_on_complete;
    cbs.on_error = http_on_error;

    /* 发起HTTP请求 */
    vox_http_client_req_t* req = NULL;
    if (vox_http_client_request(client, request, &cbs, &state, &req) < 0) {
        vox_coroutine_promise_destroy(state.promise);
        return -1;
    }

    /* 等待Promise完成 */
    int ret = vox_coroutine_await(co, state.promise);
    
    vox_coroutine_promise_destroy(state.promise);
    return ret;
}

void vox_coroutine_http_response_free(vox_coroutine_http_response_t* response) {
    if (!response) {
        return;
    }

    /* 注意：响应数据由loop的内存池管理，在协程销毁时统一释放 */
    /* 这里只是清空指针和计数器 */
    response->headers = NULL;
    response->header_count = 0;
    response->body = NULL;
    response->body_len = 0;
    response->error_message = NULL;

    memset(response, 0, sizeof(vox_coroutine_http_response_t));
}

/* ===== 便捷函数实现 ===== */

int vox_coroutine_http_get_await(vox_coroutine_t* co,
                                  vox_http_client_t* client,
                                  const char* url,
                                  vox_coroutine_http_response_t* out_response) {
    vox_http_client_request_t request = {0};
    request.method = VOX_HTTP_METHOD_GET;
    request.url = url;
    
    return vox_coroutine_http_request_await(co, client, &request, out_response);
}

int vox_coroutine_http_post_await(vox_coroutine_t* co,
                                   vox_http_client_t* client,
                                   const char* url,
                                   const void* body,
                                   size_t body_len,
                                   const char* content_type,
                                   vox_coroutine_http_response_t* out_response) {
    vox_http_client_header_t header = {
        .name = "Content-Type",
        .value = content_type ? content_type : "application/octet-stream"
    };
    
    vox_http_client_request_t request = {0};
    request.method = VOX_HTTP_METHOD_POST;
    request.url = url;
    request.headers = &header;
    request.header_count = 1;
    request.body = body;
    request.body_len = body_len;
    
    return vox_coroutine_http_request_await(co, client, &request, out_response);
}

int vox_coroutine_http_post_json_await(vox_coroutine_t* co,
                                        vox_http_client_t* client,
                                        const char* url,
                                        const char* json_body,
                                        vox_coroutine_http_response_t* out_response) {
    return vox_coroutine_http_post_await(co, client, url, 
                                         json_body, strlen(json_body),
                                         "application/json",
                                         out_response);
}

int vox_coroutine_http_put_await(vox_coroutine_t* co,
                                  vox_http_client_t* client,
                                  const char* url,
                                  const void* body,
                                  size_t body_len,
                                  const char* content_type,
                                  vox_coroutine_http_response_t* out_response) {
    vox_http_client_header_t header = {
        .name = "Content-Type",
        .value = content_type ? content_type : "application/octet-stream"
    };
    
    vox_http_client_request_t request = {0};
    request.method = VOX_HTTP_METHOD_PUT;
    request.url = url;
    request.headers = &header;
    request.header_count = 1;
    request.body = body;
    request.body_len = body_len;
    
    return vox_coroutine_http_request_await(co, client, &request, out_response);
}

int vox_coroutine_http_delete_await(vox_coroutine_t* co,
                                     vox_http_client_t* client,
                                     const char* url,
                                     vox_coroutine_http_response_t* out_response) {
    vox_http_client_request_t request = {0};
    request.method = VOX_HTTP_METHOD_DELETE;
    request.url = url;
    
    return vox_coroutine_http_request_await(co, client, &request, out_response);
}
