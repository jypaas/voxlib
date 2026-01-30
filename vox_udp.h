/*
 * vox_udp.h - UDP 异步操作
 * 提供类似 libuv 的 UDP 异步接口
 */

#ifndef VOX_UDP_H
#define VOX_UDP_H

#include "vox_handle.h"
#include "vox_socket.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 前向声明 */
typedef struct vox_udp vox_udp_t;

/* UDP 接收回调函数类型 */
typedef void (*vox_udp_recv_cb)(vox_udp_t* udp, ssize_t nread, 
                                const void* buf, 
                                const vox_socket_addr_t* addr,
                                unsigned int flags,
                                void* user_data);

/* UDP 发送回调函数类型 */
typedef void (*vox_udp_send_cb)(vox_udp_t* udp, int status, void* user_data);

/* UDP 缓冲区分配回调函数类型 */
typedef void (*vox_udp_alloc_cb)(vox_udp_t* udp, size_t suggested_size,
                                 void* buf, size_t* len, void* user_data);

#ifdef VOX_OS_WINDOWS
/* UDP IO 操作类型（用于扩展 OVERLAPPED 结构） */
typedef enum {
    VOX_UDP_IO_RECV = 1,
    VOX_UDP_IO_SEND = 2
} vox_udp_io_type_t;

/* 扩展 OVERLAPPED 结构
 * OVERLAPPED 必须是第一个成员，以便使用 VOX_CONTAINING_RECORD 宏
 */
typedef struct {
    OVERLAPPED overlapped;          /* OVERLAPPED 结构（必须是第一个成员） */
    vox_udp_io_type_t io_type;      /* IO 操作类型 */
    struct vox_udp* udp;            /* 指向 UDP 句柄的指针 */
} vox_udp_overlapped_ex_t;
#endif

/* UDP 句柄结构 */
struct vox_udp {
    /* 句柄基类（必须作为第一个成员） */
    vox_handle_t handle;

    /* Socket */
    vox_socket_t socket;

    /* 回调函数 */
    vox_udp_alloc_cb alloc_cb;      /* 缓冲区分配回调 */
    vox_udp_recv_cb recv_cb;        /* 接收回调 */
    vox_udp_send_cb send_cb;        /* 发送回调 */

    /* 状态 */
    bool bound;                     /* 是否已绑定 */
    bool receiving;                 /* 是否正在接收 */
    bool closing;                   /* 是否正在关闭（用于防止 IOCP use-after-free） */

    /* 内部状态 */
    void* recv_buf;                 /* 接收缓冲区 */
    size_t recv_buf_size;            /* 接收缓冲区大小 */
    void* send_queue;                /* 发送请求队列（链表头） */

    /* Backend 注册状态 */
    bool backend_registered;         /* 是否已注册到 backend */
    uint32_t backend_events;        /* 注册的事件 */
    void* backend_data;             /* backend 内部数据指针（用于注销时释放） */

#ifdef VOX_OS_WINDOWS
    /* Windows IOCP 异步 IO 支持（使用扩展 OVERLAPPED 结构） */
    vox_udp_overlapped_ex_t recv_ov_ex;   /* 接收操作的扩展 OVERLAPPED */
    vox_udp_overlapped_ex_t send_ov_ex;   /* 发送操作的扩展 OVERLAPPED */

    /* WSARecvFrom 相关 */
    WSABUF* recv_bufs;                /* WSARecvFrom 缓冲区数组 */
    DWORD recv_buf_count;             /* 缓冲区数量 */
    DWORD recv_flags;                 /* 接收标志 */
    struct sockaddr_storage recv_from_addr;  /* 接收来源地址 */
    int recv_from_addr_len;          /* 地址长度 */
    bool recv_pending;                /* 是否有待处理的 WSARecvFrom 操作 */

    /* WSASendTo 相关 */
    WSABUF* send_bufs;                /* WSASendTo 缓冲区数组 */
    DWORD send_buf_count;             /* 缓冲区数量 */
    struct sockaddr_storage send_to_addr;    /* 发送目标地址 */
    int send_to_addr_len;            /* 地址长度 */
    bool send_pending;                /* 是否有待处理的 WSASendTo 操作 */
#endif
};

/**
 * 初始化 UDP 句柄
 * @param udp UDP 句柄指针
 * @param loop 事件循环指针
 * @return 成功返回0，失败返回-1
 */
int vox_udp_init(vox_udp_t* udp, vox_loop_t* loop);

/**
 * 使用内存池创建 UDP 句柄
 * @param loop 事件循环指针
 * @return 成功返回 UDP 句柄指针，失败返回 NULL
 */
vox_udp_t* vox_udp_create(vox_loop_t* loop);

/**
 * 销毁 UDP 句柄
 * @param udp UDP 句柄指针
 */
void vox_udp_destroy(vox_udp_t* udp);

/**
 * 绑定地址
 * @param udp UDP 句柄指针
 * @param addr 地址指针
 * @param flags 标志位（保留，传0）
 * @return 成功返回0，失败返回-1
 */
int vox_udp_bind(vox_udp_t* udp, const vox_socket_addr_t* addr, unsigned int flags);

/**
 * 开始异步接收数据
 * @param udp UDP 句柄指针
 * @param alloc_cb 缓冲区分配回调函数（可以为NULL，使用默认缓冲区）
 * @param recv_cb 接收回调函数
 * @return 成功返回0，失败返回-1
 */
int vox_udp_recv_start(vox_udp_t* udp, vox_udp_alloc_cb alloc_cb, vox_udp_recv_cb recv_cb);

/**
 * 停止异步接收数据
 * @param udp UDP 句柄指针
 * @return 成功返回0，失败返回-1
 */
int vox_udp_recv_stop(vox_udp_t* udp);

/**
 * 异步发送数据
 * @param udp UDP 句柄指针
 * @param buf 数据缓冲区
 * @param len 数据长度
 * @param addr 目标地址（可以为NULL，如果UDP已连接）
 * @param cb 发送回调函数（可以为NULL）
 * @return 成功返回0，失败返回-1
 */
int vox_udp_send(vox_udp_t* udp, const void* buf, size_t len, 
                 const vox_socket_addr_t* addr, vox_udp_send_cb cb);

/**
 * 获取本地地址
 * @param udp UDP 句柄指针
 * @param addr 地址结构指针
 * @return 成功返回0，失败返回-1
 */
int vox_udp_getsockname(vox_udp_t* udp, vox_socket_addr_t* addr);

/**
 * 设置广播选项
 * @param udp UDP 句柄指针
 * @param enable 是否启用
 * @return 成功返回0，失败返回-1
 */
int vox_udp_set_broadcast(vox_udp_t* udp, bool enable);

/**
 * 设置地址重用选项
 * @param udp UDP 句柄指针
 * @param enable 是否启用
 * @return 成功返回0，失败返回-1
 */
int vox_udp_set_reuseaddr(vox_udp_t* udp, bool enable);

/* 注意：多播相关功能暂未实现，可在后续版本中添加 */

#ifdef __cplusplus
}
#endif

#endif /* VOX_UDP_H */
