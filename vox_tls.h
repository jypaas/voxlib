/*
 * vox_tls.h - TLS 异步操作
 * 基于vox_tcp的TLS异步接口
 */

#ifndef VOX_TLS_H
#define VOX_TLS_H

#include "vox_handle.h"
#include "vox_tcp.h"
#include "ssl/vox_ssl.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 前向声明 */
typedef struct vox_tls vox_tls_t;

/* TLS 连接回调函数类型 */
typedef void (*vox_tls_connect_cb)(vox_tls_t* tls, int status, void* user_data);

/* TLS 连接接受回调函数类型 */
typedef void (*vox_tls_connection_cb)(vox_tls_t* server, int status, void* user_data);

/* TLS 握手回调函数类型 */
typedef void (*vox_tls_handshake_cb)(vox_tls_t* tls, int status, void* user_data);

/* 缓冲区分配回调函数类型 */
typedef void (*vox_tls_alloc_cb)(vox_tls_t* tls, size_t suggested_size, void* buf, size_t* len, void* user_data);

/* TLS 读取回调函数类型 */
typedef void (*vox_tls_read_cb)(vox_tls_t* tls, ssize_t nread, const void* buf, void* user_data);

/* TLS 写入回调函数类型 */
typedef void (*vox_tls_write_cb)(vox_tls_t* tls, int status, void* user_data);

/* TLS 关闭回调函数类型 */
typedef void (*vox_tls_shutdown_cb)(vox_tls_t* tls, int status, void* user_data);

/* TLS 句柄结构 */
struct vox_tls {
    /* 句柄基类（必须作为第一个成员） */
    vox_handle_t handle;

    /* 底层 TCP 句柄 */
    vox_tcp_t* tcp;

    /* SSL Context 和 Session */
    vox_ssl_context_t* ssl_ctx;
    vox_ssl_session_t* ssl_session;

    /* 回调函数 */
    vox_tls_connect_cb connect_cb;        /* 连接回调 */
    vox_tls_connection_cb connection_cb;  /* 接受连接回调（服务器端） */
    vox_tls_handshake_cb handshake_cb;    /* 握手回调 */
    vox_tls_alloc_cb alloc_cb;            /* 缓冲区分配回调 */
    vox_tls_read_cb read_cb;              /* 读取回调 */
    vox_tls_write_cb write_cb;           /* 写入回调 */
    vox_tls_shutdown_cb shutdown_cb;     /* 关闭回调 */

    /* 状态 */
    bool connected;                       /* TCP 是否已连接 */
    bool tls_connected;                  /* TLS 是否已连接（握手完成） */
    bool listening;                      /* 是否正在监听 */
    bool reading;                        /* 是否正在读取 */
    bool handshaking;                    /* 是否正在握手 */
    bool shutting_down;                  /* 是否正在关闭 */

    /* 内部状态 */
    void* read_buf;                      /* 读取缓冲区 */
    size_t read_buf_size;                /* 读取缓冲区大小 */
    void* write_queue;                   /* 写入请求队列（链表头） */
    void* write_queue_tail;              /* 写入请求队列（链表尾，用于 O(1) 添加） */

    /* Memory BIO 缓冲区 */
    void* rbio_buf;                      /* rbio 读取缓冲区（从 socket 读取的加密数据） */
    size_t rbio_buf_size;                /* rbio 缓冲区大小 */
    void* wbio_buf;                      /* wbio 读取缓冲区（要写入 socket 的加密数据） */
    size_t wbio_buf_size;                /* wbio 缓冲区大小 */
};

/**
 * 初始化 TLS 句柄
 * @param tls TLS 句柄指针
 * @param loop 事件循环指针
 * @param ssl_ctx SSL Context（如果为NULL，将创建默认的客户端context）
 * @return 成功返回0，失败返回-1
 */
int vox_tls_init(vox_tls_t* tls, vox_loop_t* loop, vox_ssl_context_t* ssl_ctx);

/**
 * 使用内存池创建 TLS 句柄
 * @param loop 事件循环指针
 * @param ssl_ctx SSL Context（如果为NULL，将创建默认的客户端context）
 * @return 成功返回 TLS 句柄指针，失败返回 NULL
 */
vox_tls_t* vox_tls_create(vox_loop_t* loop, vox_ssl_context_t* ssl_ctx);

/**
 * 销毁 TLS 句柄
 * @param tls TLS 句柄指针
 */
void vox_tls_destroy(vox_tls_t* tls);

/**
 * 绑定地址
 * @param tls TLS 句柄指针
 * @param addr 地址指针
 * @param flags 标志位（保留，传0）
 * @return 成功返回0，失败返回-1
 */
int vox_tls_bind(vox_tls_t* tls, const vox_socket_addr_t* addr, unsigned int flags);

/**
 * 开始监听连接
 * @param tls TLS 句柄指针
 * @param backlog 监听队列长度
 * @param cb 连接接受回调函数
 * @return 成功返回0，失败返回-1
 */
int vox_tls_listen(vox_tls_t* tls, int backlog, vox_tls_connection_cb cb);

/**
 * 接受连接（在 connection_cb 中调用）
 * @param server 服务器 TLS 句柄指针
 * @param client 客户端 TLS 句柄指针（必须已初始化）
 * @return 成功返回0，失败返回-1
 */
int vox_tls_accept(vox_tls_t* server, vox_tls_t* client);

/**
 * 异步连接
 * @param tls TLS 句柄指针
 * @param addr 目标地址
 * @param cb 连接回调函数（TCP连接成功后，会自动开始TLS握手）
 * @return 成功返回0，失败返回-1
 */
int vox_tls_connect(vox_tls_t* tls, const vox_socket_addr_t* addr, vox_tls_connect_cb cb);

/**
 * 开始 TLS 握手（服务器端在 accept 后调用，客户端在 connect 后自动调用）
 * @param tls TLS 句柄指针
 * @param cb 握手回调函数
 * @return 成功返回0，失败返回-1
 */
int vox_tls_handshake(vox_tls_t* tls, vox_tls_handshake_cb cb);

/**
 * 开始异步读取
 * @param tls TLS 句柄指针
 * @param alloc_cb 缓冲区分配回调函数（可以为NULL，使用默认缓冲区）
 * @param read_cb 读取回调函数
 * @return 成功返回0，失败返回-1
 */
int vox_tls_read_start(vox_tls_t* tls, vox_tls_alloc_cb alloc_cb, vox_tls_read_cb read_cb);

/**
 * 停止异步读取
 * @param tls TLS 句柄指针
 * @return 成功返回0，失败返回-1
 */
int vox_tls_read_stop(vox_tls_t* tls);

/**
 * 异步写入
 * @param tls TLS 句柄指针
 * @param buf 数据缓冲区
 * @param len 数据长度
 * @param cb 写入回调函数（可以为NULL）
 * @return 成功返回0，失败返回-1
 */
int vox_tls_write(vox_tls_t* tls, const void* buf, size_t len, vox_tls_write_cb cb);

/**
 * 关闭写入端（shutdown）
 * @param tls TLS 句柄指针
 * @param cb 关闭回调函数（可以为NULL）
 * @return 成功返回0，失败返回-1
 */
int vox_tls_shutdown(vox_tls_t* tls, vox_tls_shutdown_cb cb);

/**
 * 获取本地地址
 * @param tls TLS 句柄指针
 * @param addr 地址结构指针
 * @return 成功返回0，失败返回-1
 */
int vox_tls_getsockname(vox_tls_t* tls, vox_socket_addr_t* addr);

/**
 * 获取对端地址
 * @param tls TLS 句柄指针
 * @param addr 地址结构指针
 * @return 成功返回0，失败返回-1
 */
int vox_tls_getpeername(vox_tls_t* tls, vox_socket_addr_t* addr);

/**
 * 设置 TCP 无延迟（禁用 Nagle 算法）
 * @param tls TLS 句柄指针
 * @param enable 是否启用
 * @return 成功返回0，失败返回-1
 */
int vox_tls_nodelay(vox_tls_t* tls, bool enable);

/**
 * 设置保持连接选项
 * @param tls TLS 句柄指针
 * @param enable 是否启用
 * @return 成功返回0，失败返回-1
 */
int vox_tls_keepalive(vox_tls_t* tls, bool enable);

/**
 * 设置地址重用选项
 * @param tls TLS 句柄指针
 * @param enable 是否启用
 * @return 成功返回0，失败返回-1
 */
int vox_tls_reuseaddr(vox_tls_t* tls, bool enable);

#ifdef __cplusplus
}
#endif

#endif /* VOX_TLS_H */
