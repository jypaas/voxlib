/*
 * vox_tcp.h - TCP 异步操作
 * 提供类似 libuv 的 TCP 异步接口
 */

#ifndef VOX_TCP_H
#define VOX_TCP_H

#include "vox_handle.h"
#include "vox_socket.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 前向声明 */
typedef struct vox_tcp vox_tcp_t;

/* TCP 连接回调函数类型 */
typedef void (*vox_tcp_connect_cb)(vox_tcp_t* tcp, int status, void* user_data);

/* TCP 连接接受回调函数类型 */
typedef void (*vox_tcp_connection_cb)(vox_tcp_t* server, int status, void* user_data);

/* 缓冲区分配回调函数类型 */
typedef void (*vox_tcp_alloc_cb)(vox_tcp_t* tcp, size_t suggested_size, void* buf, size_t* len, void* user_data);

/* TCP 读取回调函数类型 */
typedef void (*vox_tcp_read_cb)(vox_tcp_t* tcp, ssize_t nread, const void* buf, void* user_data);

/* TCP 写入回调函数类型 */
typedef void (*vox_tcp_write_cb)(vox_tcp_t* tcp, int status, void* user_data);

/* TCP 关闭回调函数类型 */
typedef void (*vox_tcp_shutdown_cb)(vox_tcp_t* tcp, int status, void* user_data);

#ifdef VOX_OS_WINDOWS
/* TCP IO 操作类型（用于扩展 OVERLAPPED 结构） */
typedef enum {
    VOX_TCP_IO_ACCEPT = 1,
    VOX_TCP_IO_RECV = 2,
    VOX_TCP_IO_SEND = 3,
    VOX_TCP_IO_CONNECT = 4
} vox_tcp_io_type_t;

/* 扩展 OVERLAPPED 结构
 * 这是标准的 Windows 异步 IO 模式：OVERLAPPED 必须是第一个成员，
 * 这样我们可以通过 VOX_CONTAINING_RECORD 宏从 OVERLAPPED 指针获取扩展结构
 */
typedef struct {
    OVERLAPPED overlapped;          /* OVERLAPPED 结构（必须是第一个成员） */
    vox_tcp_io_type_t io_type;      /* IO 操作类型 */
    struct vox_tcp* tcp;            /* 指向 TCP 句柄的指针 */
} vox_tcp_overlapped_ex_t;

/* AcceptEx 操作池大小（同时发起的 AcceptEx 操作数量）
 * 512 个并发操作可以处理极高并发的连接请求（如 wrk -c1000+）
 * 这是一个比较激进的设置，适用于需要极致性能的场景
 * 注意：每个操作会占用约 256 字节内存（socket + buffer + OVERLAPPED），512 个约占用 128KB */
#define VOX_TCP_ACCEPT_POOL_SIZE 512

/* AcceptEx 操作上下文（用于支持多个并发 AcceptEx 操作）
 * 每个上下文包含独立的 socket、buffer 和 OVERLAPPED 结构 */
typedef struct {
    vox_tcp_overlapped_ex_t ov_ex;  /* 扩展 OVERLAPPED 结构 */
    SOCKET socket;                   /* AcceptEx 使用的客户端 socket */
    void* buffer;                    /* AcceptEx 缓冲区（包含地址信息） */
    size_t buffer_size;              /* 缓冲区大小 */
    bool pending;                    /* 是否有待处理的操作 */
    int index;                       /* 在池中的索引 */
} vox_tcp_accept_ctx_t;
#endif

/* TCP 句柄结构 */
struct vox_tcp {
    /* 句柄基类（必须作为第一个成员） */
    vox_handle_t handle;

    /* Socket */
    vox_socket_t socket;

    /* 回调函数 */
    vox_tcp_connect_cb connect_cb;        /* 连接回调 */
    vox_tcp_connection_cb connection_cb;   /* 接受连接回调（服务器端） */
    vox_tcp_alloc_cb alloc_cb;            /* 缓冲区分配回调 */
    vox_tcp_read_cb read_cb;              /* 读取回调 */
    vox_tcp_write_cb write_cb;            /* 写入回调 */
    vox_tcp_shutdown_cb shutdown_cb;       /* 关闭回调 */

    /* 状态 */
    bool connected;                       /* 是否已连接 */
    bool listening;                       /* 是否正在监听 */
    bool reading;                         /* 是否正在读取 */

    /* 内部状态 */
    void* read_buf;                       /* 读取缓冲区 */
    size_t read_buf_size;                 /* 读取缓冲区大小 */
    void* write_queue;                    /* 写入请求队列（链表头） */

    /* Backend 注册状态 */
    bool backend_registered;               /* 是否已注册到 backend */
    uint32_t backend_events;              /* 注册的事件 */

#ifdef VOX_OS_WINDOWS
    /* Windows IOCP 异步 IO 支持（使用扩展 OVERLAPPED 结构） */
    vox_tcp_overlapped_ex_t read_ov_ex;    /* 读取操作的扩展 OVERLAPPED */
    vox_tcp_overlapped_ex_t write_ov_ex;   /* 写入操作的扩展 OVERLAPPED */
    vox_tcp_overlapped_ex_t connect_ov_ex; /* 连接操作的扩展 OVERLAPPED */

    /* AcceptEx 操作池（支持多个并发 AcceptEx 操作以处理高并发连接）
     * 使用操作池可以同时发起多个 AcceptEx 操作，显著提升高并发场景下的连接接受能力 */
    vox_tcp_accept_ctx_t* accept_pool;     /* AcceptEx 操作池数组 */
    int accept_pool_size;                  /* 操作池大小 */
    int accept_pending_count;              /* 当前待处理的 AcceptEx 操作数量 */
    SOCKET accept_socket;                  /* 临时字段：当前接受的 socket（用于传递给 vox_tcp_accept） */

    /* WSARecv 相关 */
    WSABUF* recv_bufs;                     /* WSARecv 缓冲区数组 */
    DWORD recv_buf_count;                  /* 缓冲区数量 */
    DWORD recv_flags;                      /* 接收标志 */
    bool recv_pending;                    /* 是否有待处理的 WSARecv 操作 */

    /* WSASend 相关 */
    WSABUF* send_bufs;                     /* WSASend 缓冲区数组 */
    DWORD send_buf_count;                  /* 缓冲区数量 */
    bool send_pending;                     /* 是否有待处理的 WSASend 操作 */

    /* ConnectEx 相关 */
    bool connect_pending;                  /* 是否有待处理的 ConnectEx 操作 */
#endif
};

/**
 * 初始化 TCP 句柄
 * @param tcp TCP 句柄指针
 * @param loop 事件循环指针
 * @return 成功返回0，失败返回-1
 */
int vox_tcp_init(vox_tcp_t* tcp, vox_loop_t* loop);

/**
 * 使用内存池创建 TCP 句柄
 * @param loop 事件循环指针
 * @return 成功返回 TCP 句柄指针，失败返回 NULL
 */
vox_tcp_t* vox_tcp_create(vox_loop_t* loop);

/**
 * 销毁 TCP 句柄
 * @param tcp TCP 句柄指针
 */
void vox_tcp_destroy(vox_tcp_t* tcp);

/**
 * 绑定地址
 * @param tcp TCP 句柄指针
 * @param addr 地址指针
 * @param flags 标志位（保留，传0）
 * @return 成功返回0，失败返回-1
 */
int vox_tcp_bind(vox_tcp_t* tcp, const vox_socket_addr_t* addr, unsigned int flags);

/**
 * 开始监听连接
 * @param tcp TCP 句柄指针
 * @param backlog 监听队列长度
 * @param cb 连接接受回调函数
 * @return 成功返回0，失败返回-1
 */
int vox_tcp_listen(vox_tcp_t* tcp, int backlog, vox_tcp_connection_cb cb);

/**
 * 接受连接（在 connection_cb 中调用）
 * @param server 服务器 TCP 句柄指针
 * @param client 客户端 TCP 句柄指针（必须已初始化）
 * @return 成功返回0，失败返回-1
 */
int vox_tcp_accept(vox_tcp_t* server, vox_tcp_t* client);

/**
 * 异步连接
 * @param tcp TCP 句柄指针
 * @param addr 目标地址
 * @param cb 连接回调函数
 * @return 成功返回0，失败返回-1
 */
int vox_tcp_connect(vox_tcp_t* tcp, const vox_socket_addr_t* addr, vox_tcp_connect_cb cb);

/**
 * 开始异步读取
 * @param tcp TCP 句柄指针
 * @param alloc_cb 缓冲区分配回调函数（可以为NULL，使用默认缓冲区）
 * @param read_cb 读取回调函数
 * @return 成功返回0，失败返回-1
 */
int vox_tcp_read_start(vox_tcp_t* tcp, vox_tcp_alloc_cb alloc_cb, vox_tcp_read_cb read_cb);

/**
 * 停止异步读取
 * @param tcp TCP 句柄指针
 * @return 成功返回0，失败返回-1
 */
int vox_tcp_read_stop(vox_tcp_t* tcp);

/**
 * 异步写入
 * @param tcp TCP 句柄指针
 * @param buf 数据缓冲区
 * @param len 数据长度
 * @param cb 写入回调函数（可以为NULL）
 * @return 成功返回0，失败返回-1
 */
int vox_tcp_write(vox_tcp_t* tcp, const void* buf, size_t len, vox_tcp_write_cb cb);

/**
 * 关闭写入端（shutdown）
 * @param tcp TCP 句柄指针
 * @param cb 关闭回调函数（可以为NULL）
 * @return 成功返回0，失败返回-1
 */
int vox_tcp_shutdown(vox_tcp_t* tcp, vox_tcp_shutdown_cb cb);

/**
 * 获取本地地址
 * @param tcp TCP 句柄指针
 * @param addr 地址结构指针
 * @return 成功返回0，失败返回-1
 */
int vox_tcp_getsockname(vox_tcp_t* tcp, vox_socket_addr_t* addr);

/**
 * 获取对端地址
 * @param tcp TCP 句柄指针
 * @param addr 地址结构指针
 * @return 成功返回0，失败返回-1
 */
int vox_tcp_getpeername(vox_tcp_t* tcp, vox_socket_addr_t* addr);

/**
 * 设置 TCP 无延迟（禁用 Nagle 算法）
 * @param tcp TCP 句柄指针
 * @param enable 是否启用
 * @return 成功返回0，失败返回-1
 */
int vox_tcp_nodelay(vox_tcp_t* tcp, bool enable);

/**
 * 设置保持连接选项
 * @param tcp TCP 句柄指针
 * @param enable 是否启用
 * @return 成功返回0，失败返回-1
 */
int vox_tcp_keepalive(vox_tcp_t* tcp, bool enable);

/**
 * 设置地址重用选项
 * @param tcp TCP 句柄指针
 * @param enable 是否启用
 * @return 成功返回0，失败返回-1
 */
int vox_tcp_reuseaddr(vox_tcp_t* tcp, bool enable);

#ifdef __cplusplus
}
#endif

#endif /* VOX_TCP_H */
