/*
 * vox_dtls.h - DTLS 异步操作
 * 基于vox_udp的DTLS异步接口
 */

#ifndef VOX_DTLS_H
#define VOX_DTLS_H

#include "vox_handle.h"
#include "vox_udp.h"
#include "ssl/vox_ssl.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 前向声明 */
typedef struct vox_dtls vox_dtls_t;

/* DTLS 连接回调函数类型 */
typedef void (*vox_dtls_connect_cb)(vox_dtls_t* dtls, int status, void* user_data);

/* DTLS 连接接受回调函数类型 */
typedef void (*vox_dtls_connection_cb)(vox_dtls_t* server, int status, void* user_data);

/* DTLS 握手回调函数类型 */
typedef void (*vox_dtls_handshake_cb)(vox_dtls_t* dtls, int status, void* user_data);

/* 缓冲区分配回调函数类型 */
typedef void (*vox_dtls_alloc_cb)(vox_dtls_t* dtls, size_t suggested_size, void* buf, size_t* len, void* user_data);

/* DTLS 读取回调函数类型 */
typedef void (*vox_dtls_read_cb)(vox_dtls_t* dtls, ssize_t nread, const void* buf, 
                                 const vox_socket_addr_t* addr, void* user_data);

/* DTLS 写入回调函数类型 */
typedef void (*vox_dtls_write_cb)(vox_dtls_t* dtls, int status, void* user_data);

/* DTLS 关闭回调函数类型 */
typedef void (*vox_dtls_shutdown_cb)(vox_dtls_t* dtls, int status, void* user_data);

/* DTLS 句柄结构 */
struct vox_dtls {
    /* 句柄基类（必须作为第一个成员） */
    vox_handle_t handle;

    /* 底层 UDP 句柄 */
    vox_udp_t* udp;

    /* SSL Context 和 Session */
    vox_ssl_context_t* ssl_ctx;
    vox_ssl_session_t* ssl_session;

    /* 回调函数 */
    vox_dtls_connect_cb connect_cb;        /* 连接回调 */
    vox_dtls_connection_cb connection_cb;  /* 接受连接回调（服务器端） */
    vox_dtls_handshake_cb handshake_cb;    /* 握手回调 */
    vox_dtls_alloc_cb alloc_cb;            /* 缓冲区分配回调 */
    vox_dtls_read_cb read_cb;              /* 读取回调 */
    vox_dtls_write_cb write_cb;           /* 写入回调 */
    vox_dtls_shutdown_cb shutdown_cb;     /* 关闭回调 */

    /* 状态 */
    bool bound;                           /* 是否已绑定 */
    bool dtls_connected;                  /* DTLS 是否已连接（握手完成） */
    bool listening;                      /* 是否正在监听 */
    bool reading;                        /* 是否正在读取 */
    bool handshaking;                    /* 是否正在握手 */
    bool shutting_down;                  /* 是否正在关闭 */

    /* 对端地址（用于已连接的 DTLS） */
    vox_socket_addr_t peer_addr;          /* 对端地址 */
    bool peer_addr_set;                  /* 是否已设置对端地址 */

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
 * 初始化 DTLS 句柄
 * @param dtls DTLS 句柄指针
 * @param loop 事件循环指针
 * @param ssl_ctx SSL Context（如果为NULL，将创建默认的客户端context）
 * @return 成功返回0，失败返回-1
 */
int vox_dtls_init(vox_dtls_t* dtls, vox_loop_t* loop, vox_ssl_context_t* ssl_ctx);

/**
 * 使用内存池创建 DTLS 句柄
 * @param loop 事件循环指针
 * @param ssl_ctx SSL Context（如果为NULL，将创建默认的客户端context）
 * @return 成功返回 DTLS 句柄指针，失败返回 NULL
 */
vox_dtls_t* vox_dtls_create(vox_loop_t* loop, vox_ssl_context_t* ssl_ctx);

/**
 * 销毁 DTLS 句柄
 * @param dtls DTLS 句柄指针
 */
void vox_dtls_destroy(vox_dtls_t* dtls);

/**
 * 绑定地址
 * @param dtls DTLS 句柄指针
 * @param addr 地址指针
 * @param flags 标志位（保留，传0）
 * @return 成功返回0，失败返回-1
 */
int vox_dtls_bind(vox_dtls_t* dtls, const vox_socket_addr_t* addr, unsigned int flags);

/**
 * 开始监听连接
 * @param dtls DTLS 句柄指针
 * @param backlog 监听队列长度（UDP 中此参数被忽略）
 * @param cb 连接接受回调函数
 * @return 成功返回0，失败返回-1
 */
int vox_dtls_listen(vox_dtls_t* dtls, int backlog, vox_dtls_connection_cb cb);

/**
 * 接受连接（在 connection_cb 中调用）
 * @param server 服务器 DTLS 句柄指针
 * @param client 客户端 DTLS 句柄指针（必须已初始化）
 * @param addr 客户端地址（从 recv_cb 中获取）
 * @return 成功返回0，失败返回-1
 */
int vox_dtls_accept(vox_dtls_t* server, vox_dtls_t* client, const vox_socket_addr_t* addr);

/**
 * 异步连接
 * @param dtls DTLS 句柄指针
 * @param addr 目标地址
 * @param cb 连接回调函数（UDP连接成功后，会自动开始DTLS握手）
 * @return 成功返回0，失败返回-1
 */
int vox_dtls_connect(vox_dtls_t* dtls, const vox_socket_addr_t* addr, vox_dtls_connect_cb cb);

/**
 * 开始 DTLS 握手（服务器端在 accept 后调用，客户端在 connect 后自动调用）
 * @param dtls DTLS 句柄指针
 * @param cb 握手回调函数
 * @return 成功返回0，失败返回-1
 */
int vox_dtls_handshake(vox_dtls_t* dtls, vox_dtls_handshake_cb cb);

/**
 * 开始异步读取
 * @param dtls DTLS 句柄指针
 * @param alloc_cb 缓冲区分配回调函数（可以为NULL，使用默认缓冲区）
 * @param read_cb 读取回调函数
 * @return 成功返回0，失败返回-1
 */
int vox_dtls_read_start(vox_dtls_t* dtls, vox_dtls_alloc_cb alloc_cb, vox_dtls_read_cb read_cb);

/**
 * 停止异步读取
 * @param dtls DTLS 句柄指针
 * @return 成功返回0，失败返回-1
 */
int vox_dtls_read_stop(vox_dtls_t* dtls);

/**
 * 异步写入
 * @param dtls DTLS 句柄指针
 * @param buf 数据缓冲区
 * @param len 数据长度
 * @param addr 目标地址（可以为NULL，如果DTLS已连接）
 * @param cb 写入回调函数（可以为NULL）
 * @return 成功返回0，失败返回-1
 */
int vox_dtls_write(vox_dtls_t* dtls, const void* buf, size_t len, 
                   const vox_socket_addr_t* addr, vox_dtls_write_cb cb);

/**
 * 关闭写入端（shutdown）
 * @param dtls DTLS 句柄指针
 * @param cb 关闭回调函数（可以为NULL）
 * @return 成功返回0，失败返回-1
 */
int vox_dtls_shutdown(vox_dtls_t* dtls, vox_dtls_shutdown_cb cb);

/**
 * 获取本地地址
 * @param dtls DTLS 句柄指针
 * @param addr 地址结构指针
 * @return 成功返回0，失败返回-1
 */
int vox_dtls_getsockname(vox_dtls_t* dtls, vox_socket_addr_t* addr);

/**
 * 获取对端地址
 * @param dtls DTLS 句柄指针
 * @param addr 地址结构指针
 * @return 成功返回0，失败返回-1
 */
int vox_dtls_getpeername(vox_dtls_t* dtls, vox_socket_addr_t* addr);

/**
 * 设置广播选项
 * @param dtls DTLS 句柄指针
 * @param enable 是否启用
 * @return 成功返回0，失败返回-1
 */
int vox_dtls_set_broadcast(vox_dtls_t* dtls, bool enable);

/**
 * 设置地址重用选项
 * @param dtls DTLS 句柄指针
 * @param enable 是否启用
 * @return 成功返回0，失败返回-1
 */
int vox_dtls_set_reuseaddr(vox_dtls_t* dtls, bool enable);

#ifdef __cplusplus
}
#endif

#endif /* VOX_DTLS_H */
