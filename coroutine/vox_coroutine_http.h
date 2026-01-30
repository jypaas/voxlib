/*
 * vox_coroutine_http.h - HTTP协程适配器
 * 提供async/await风格的协程API，避免回调地狱
 */

#ifndef VOX_COROUTINE_HTTP_H
#define VOX_COROUTINE_HTTP_H

#include "../http/vox_http_client.h"
#include "vox_coroutine.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ===== HTTP响应数据结构 ===== */

typedef struct {
    int status_code;                    /* HTTP状态码 */
    int http_major;                     /* HTTP主版本号 */
    int http_minor;                     /* HTTP次版本号 */
    
    /* 响应头（键值对数组） */
    vox_http_client_header_t* headers;
    size_t header_count;
    
    /* 响应体 */
    void* body;
    size_t body_len;
    
    /* 错误信息（如果有） */
    char* error_message;
} vox_coroutine_http_response_t;

/* ===== 协程适配接口 ===== */

/**
 * 在协程中发起HTTP请求
 * @param co 协程指针
 * @param client HTTP客户端指针
 * @param request 请求参数
 * @param out_response 输出响应数据（需要调用者使用vox_coroutine_http_response_free释放）
 * @return 成功返回0，失败返回-1
 */
int vox_coroutine_http_request_await(vox_coroutine_t* co,
                                      vox_http_client_t* client,
                                      const vox_http_client_request_t* request,
                                      vox_coroutine_http_response_t* out_response);

/**
 * 释放HTTP响应数据
 * @param response 响应数据指针
 */
void vox_coroutine_http_response_free(vox_coroutine_http_response_t* response);

/* ===== 便捷函数 ===== */

/**
 * 在协程中发起GET请求
 * @param co 协程指针
 * @param client HTTP客户端指针
 * @param url URL地址
 * @param out_response 输出响应数据
 * @return 成功返回0，失败返回-1
 */
int vox_coroutine_http_get_await(vox_coroutine_t* co,
                                  vox_http_client_t* client,
                                  const char* url,
                                  vox_coroutine_http_response_t* out_response);

/**
 * 在协程中发起POST请求
 * @param co 协程指针
 * @param client HTTP客户端指针
 * @param url URL地址
 * @param body 请求体数据
 * @param body_len 请求体长度
 * @param content_type Content-Type头（可为NULL，默认"application/octet-stream"）
 * @param out_response 输出响应数据
 * @return 成功返回0，失败返回-1
 */
int vox_coroutine_http_post_await(vox_coroutine_t* co,
                                   vox_http_client_t* client,
                                   const char* url,
                                   const void* body,
                                   size_t body_len,
                                   const char* content_type,
                                   vox_coroutine_http_response_t* out_response);

/**
 * 在协程中发起JSON POST请求
 * @param co 协程指针
 * @param client HTTP客户端指针
 * @param url URL地址
 * @param json_body JSON字符串
 * @param out_response 输出响应数据
 * @return 成功返回0，失败返回-1
 */
int vox_coroutine_http_post_json_await(vox_coroutine_t* co,
                                        vox_http_client_t* client,
                                        const char* url,
                                        const char* json_body,
                                        vox_coroutine_http_response_t* out_response);

/**
 * 在协程中发起PUT请求
 * @param co 协程指针
 * @param client HTTP客户端指针
 * @param url URL地址
 * @param body 请求体数据
 * @param body_len 请求体长度
 * @param content_type Content-Type头（可为NULL）
 * @param out_response 输出响应数据
 * @return 成功返回0，失败返回-1
 */
int vox_coroutine_http_put_await(vox_coroutine_t* co,
                                  vox_http_client_t* client,
                                  const char* url,
                                  const void* body,
                                  size_t body_len,
                                  const char* content_type,
                                  vox_coroutine_http_response_t* out_response);

/**
 * 在协程中发起DELETE请求
 * @param co 协程指针
 * @param client HTTP客户端指针
 * @param url URL地址
 * @param out_response 输出响应数据
 * @return 成功返回0，失败返回-1
 */
int vox_coroutine_http_delete_await(vox_coroutine_t* co,
                                     vox_http_client_t* client,
                                     const char* url,
                                     vox_coroutine_http_response_t* out_response);

#ifdef __cplusplus
}
#endif

#endif /* VOX_COROUTINE_HTTP_H */
