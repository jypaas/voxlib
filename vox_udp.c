/*
 * vox_udp.c - UDP 异步操作实现
 */

#include "vox_udp.h"
#include "vox_os.h"
#include "vox_loop.h"
#include "vox_backend.h"
#include "vox_mpool.h"
#include "vox_log.h"
#include <string.h>
#include <stdlib.h>

/* 默认接收缓冲区大小 */
#define VOX_UDP_DEFAULT_RECV_BUF_SIZE 65536

/* UDP 发送请求结构 */
typedef struct vox_udp_send_req {
    const void* buf;                    /* 数据缓冲区 */
    size_t len;                         /* 总长度 */
    vox_socket_addr_t addr;             /* 目标地址 */
    vox_udp_send_cb cb;                 /* 发送完成回调 */
    struct vox_udp_send_req* next;     /* 下一个请求（链表） */
} vox_udp_send_req_t;

/* UDP 句柄内部数据（用于 backend 事件回调） */
typedef struct {
    vox_udp_t* udp;
    void* user_data;
} vox_udp_internal_data_t;

#ifdef VOX_OS_WINDOWS
/* Windows IOCP 异步 IO 操作 */
static int udp_start_recv_async(vox_udp_t* udp);
static int udp_start_send_async(vox_udp_t* udp, const void* buf, size_t len, const vox_socket_addr_t* addr);
#endif

/* 前向声明 */
static void udp_process_send_queue(vox_udp_t* udp);
static int udp_register_backend(vox_udp_t* udp, uint32_t events);
static int udp_update_backend(vox_udp_t* udp, uint32_t events);
static int udp_unregister_backend(vox_udp_t* udp);

/* 从 backend 事件回调中获取 UDP 句柄 */
static vox_udp_t* get_udp_from_backend_data(void* user_data) {
    if (!user_data) {
        return NULL;
    }
    vox_udp_internal_data_t* data = (vox_udp_internal_data_t*)user_data;
    return data->udp;
}

/* Backend 事件处理回调（导出供 backend 使用） */
void vox_udp_backend_event_cb(vox_backend_t* backend, int fd, uint32_t events, void* user_data, void* overlapped, size_t bytes_transferred);
void vox_udp_backend_event_cb(vox_backend_t* backend, int fd, uint32_t events, void* user_data, void* overlapped, size_t bytes_transferred) {
    (void)fd;

#ifdef VOX_OS_WINDOWS
    /* Windows IOCP：通过 OVERLAPPED 指针获取 UDP 句柄和操作类型 */
    vox_backend_t* backend_check = backend;
    if (backend_check && vox_backend_get_type(backend_check) == VOX_BACKEND_TYPE_IOCP && overlapped) {
        /* 使用 VOX_CONTAINING_RECORD 宏从 OVERLAPPED 指针获取扩展结构 */
        vox_udp_overlapped_ex_t* ov_ex = VOX_CONTAINING_RECORD(overlapped, vox_udp_overlapped_ex_t, overlapped);
        vox_udp_t* udp = ov_ex->udp;

        if (!udp) {
            return;
        }

        /* 检查句柄是否正在关闭，防止 use-after-free */
        if (udp->closing) {
            return;
        }

        /* 根据 IO 操作类型处理事件 */
        switch (ov_ex->io_type) {
            case VOX_UDP_IO_RECV:
                /* WSARecvFrom 完成 */
                udp->recv_pending = false;

                if (bytes_transferred > 0) {
                    if (udp->recv_cb) {
                        void* buf = udp->recv_bufs ? udp->recv_bufs[0].buf : NULL;

                        /* 转换地址格式 */
                        vox_socket_addr_t from_addr;
                        memset(&from_addr, 0, sizeof(from_addr));
                        if (udp->recv_from_addr_len > 0) {
                            struct sockaddr* sa = (struct sockaddr*)&udp->recv_from_addr;
                            if (sa->sa_family == AF_INET) {
                                struct sockaddr_in* sin = (struct sockaddr_in*)sa;
                                from_addr.family = VOX_AF_INET;
                                from_addr.u.ipv4.addr = sin->sin_addr.s_addr;
                                from_addr.u.ipv4.port = sin->sin_port;
                            } else if (sa->sa_family == AF_INET6) {
                                struct sockaddr_in6* sin6 = (struct sockaddr_in6*)sa;
                                from_addr.family = VOX_AF_INET6;
                                memcpy(from_addr.u.ipv6.addr, &sin6->sin6_addr, 16);
                                from_addr.u.ipv6.port = sin6->sin6_port;
                            }
                        }

                        udp->recv_cb(udp, (ssize_t)bytes_transferred, buf, &from_addr, 0,
                                     vox_handle_get_data((vox_handle_t*)udp));
                    }

                    /* 如果还在接收，启动下一个 WSARecvFrom */
                    if (udp->receiving) {
                        if (udp_start_recv_async(udp) != 0) {
                            /* 启动下一个接收失败，调用错误回调 */
                            if (udp->recv_cb) {
                                udp->recv_cb(udp, -1, NULL, NULL, 0,
                                             vox_handle_get_data((vox_handle_t*)udp));
                            }
                            udp->receiving = false;
                        }
                    }
                } else {
                    /* 接收错误或关闭 */
                    if (udp->recv_cb) {
                        udp->recv_cb(udp, -1, NULL, NULL, 0,
                                     vox_handle_get_data((vox_handle_t*)udp));
                    }
                }
                return;

            case VOX_UDP_IO_SEND:
                /* WSASendTo 完成 */
                udp->send_pending = false;

                /* 处理发送请求队列中的回调 */
                if (udp->send_queue) {
                    vox_udp_send_req_t* req = (vox_udp_send_req_t*)udp->send_queue;
                    vox_udp_send_cb cb = req->cb;
                    vox_mpool_t* mpool = vox_loop_get_mpool(udp->handle.loop);
                    vox_udp_send_req_t* next = req->next;

                    udp->send_queue = next;

                    if (cb) {
                        cb(udp, 0, vox_handle_get_data((vox_handle_t*)udp));
                    }

                    vox_mpool_free(mpool, req);

                    /* 在 IOCP 模式下，处理队列中的下一个请求（使用异步 WSASendTo） */
                    if (next && !udp->send_pending) {
                        /* 启动下一个异步发送 */
                        if (udp_start_send_async(udp, next->buf, next->len, &next->addr) != 0) {
                            /* 启动失败，从队列中移除并调用错误回调 */
                            vox_udp_send_req_t* failed_req = next;
                            next = failed_req->next;
                            udp->send_queue = (void*)next;
                            
                            if (failed_req->cb) {
                                failed_req->cb(udp, -1, vox_handle_get_data((vox_handle_t*)udp));
                            }
                            vox_mpool_free(mpool, failed_req);
                        }
                    }
                }
                return;

            default:
                return;
        }
    }
#endif

    /* 非 IOCP 模式：传统的事件处理 */
    vox_udp_t* udp = get_udp_from_backend_data(user_data);
    if (!udp) {
        VOX_LOG_WARN("UDP event handler: udp is NULL from user_data");
        return;
    }

    (void)backend;
    (void)overlapped;
    (void)bytes_transferred;

    /* 处理错误事件 */
    if (events & VOX_BACKEND_ERROR) {
        if (udp->recv_cb) {
            udp->recv_cb(udp, -1, NULL, NULL, 0, vox_handle_get_data((vox_handle_t*)udp));
        }
        return;
    }

    /* 处理可读事件 */
    if (events & VOX_BACKEND_READ) {
        if (udp->receiving && udp->recv_cb) {
            /* 分配缓冲区 */
            void* buf = NULL;
            size_t len = 0;
            
            if (udp->alloc_cb) {
                udp->alloc_cb(udp, VOX_UDP_DEFAULT_RECV_BUF_SIZE, &buf, &len, 
                              vox_handle_get_data((vox_handle_t*)udp));
            } else {
                /* 使用默认缓冲区 */
                if (!udp->recv_buf || udp->recv_buf_size < VOX_UDP_DEFAULT_RECV_BUF_SIZE) {
                    vox_mpool_t* mpool = vox_loop_get_mpool(udp->handle.loop);
                    if (udp->recv_buf) {
                        vox_mpool_free(mpool, udp->recv_buf);
                    }
                    udp->recv_buf = vox_mpool_alloc(mpool, VOX_UDP_DEFAULT_RECV_BUF_SIZE);
                    if (udp->recv_buf) {
                        udp->recv_buf_size = VOX_UDP_DEFAULT_RECV_BUF_SIZE;
                    }
                }
                buf = udp->recv_buf;
                len = udp->recv_buf_size;
            }
            
            if (buf && len > 0) {
                /* 接收数据（非 IOCP 模式或同步 IO） */
                vox_socket_addr_t from_addr;
                int64_t nread = vox_socket_recvfrom(&udp->socket, buf, len, &from_addr);
                if (nread > 0) {
                    udp->recv_cb(udp, (ssize_t)nread, buf, &from_addr, 0, 
                                 vox_handle_get_data((vox_handle_t*)udp));
                } else if (nread < 0) {
                    /* 接收错误 */
                    udp->recv_cb(udp, -1, NULL, NULL, 0, 
                                 vox_handle_get_data((vox_handle_t*)udp));
                }
            }
        }
    }
    
    /* 处理可写事件（非 IOCP 模式） */
    if (events & VOX_BACKEND_WRITE) {
        /* 处理发送请求队列 */
        udp_process_send_queue(udp);
    }
    
    /* 处理挂起事件 */
    if (events & VOX_BACKEND_HANGUP) {
        if (udp->recv_cb) {
            udp->recv_cb(udp, 0, NULL, NULL, 0, vox_handle_get_data((vox_handle_t*)udp));
        }
        vox_udp_recv_stop(udp);
    }
}

/* 注册 UDP 句柄到 backend */
static int udp_register_backend(vox_udp_t* udp, uint32_t events) {
    if (!udp || !udp->handle.loop) {
        return -1;
    }

    vox_backend_t* backend = vox_loop_get_backend(udp->handle.loop);
    if (!backend) {
        return -1;
    }

    if (udp->socket.fd == VOX_INVALID_SOCKET) {
        return -1;
    }

    vox_loop_t* loop = udp->handle.loop;

    /* 创建内部数据 */
    vox_mpool_t* mpool = vox_loop_get_mpool(loop);
    vox_udp_internal_data_t* data = (vox_udp_internal_data_t*)vox_mpool_alloc(
        mpool, sizeof(vox_udp_internal_data_t));
    if (!data) {
        return -1;
    }

    data->udp = udp;
    data->user_data = vox_handle_get_data((vox_handle_t*)udp);

    /* 注册到 backend */
    int fd = (int)udp->socket.fd;
    if (vox_backend_add(backend, fd, events, data) != 0) {
        vox_mpool_free(mpool, data);
        return -1;
    }

    /* 保存内部数据指针以便后续释放 */
    udp->backend_data = data;
    udp->backend_registered = true;
    udp->backend_events = events;

    return 0;
}

/* 更新 backend 注册的事件 */
static int udp_update_backend(vox_udp_t* udp, uint32_t events) {
    if (!udp || !udp->handle.loop) {
        return -1;
    }
    
    vox_backend_t* backend = vox_loop_get_backend(udp->handle.loop);
    if (!backend) {
        return -1;
    }
    
    if (!udp->backend_registered) {
        return udp_register_backend(udp, events);
    }
    
    int fd = (int)udp->socket.fd;
    if (vox_backend_modify(backend, fd, events) != 0) {
        return -1;
    }
    
    udp->backend_events = events;
    return 0;
}

/* 从 backend 注销 */
static int udp_unregister_backend(vox_udp_t* udp) {
    if (!udp || !udp->handle.loop) {
        return -1;
    }

    vox_backend_t* backend = vox_loop_get_backend(udp->handle.loop);
    if (!backend) {
        return -1;
    }

    if (!udp->backend_registered) {
        return 0;
    }

    int fd = (int)udp->socket.fd;
    vox_backend_remove(backend, fd);

    /* 释放内部数据 */
    if (udp->backend_data) {
        vox_mpool_t* mpool = vox_loop_get_mpool(udp->handle.loop);
        vox_mpool_free(mpool, udp->backend_data);
        udp->backend_data = NULL;
    }

    udp->backend_registered = false;
    udp->backend_events = 0;

    return 0;
}

/* 处理发送请求队列 */
static void udp_process_send_queue(vox_udp_t* udp) {
    if (!udp || !udp->send_queue) {
        return;
    }

    vox_mpool_t* mpool = vox_loop_get_mpool(udp->handle.loop);

#ifdef VOX_OS_WINDOWS
    /* Windows IOCP: 检查是否使用 IOCP backend，如果是则使用异步 WSASendTo */
    vox_backend_t* backend = vox_loop_get_backend(udp->handle.loop);
    if (backend && vox_backend_get_type(backend) == VOX_BACKEND_TYPE_IOCP) {
        /* IOCP 模式：一次只处理队列头的一个请求（使用异步 WSASendTo） */
        vox_udp_send_req_t* req = (vox_udp_send_req_t*)udp->send_queue;

        /* 如果没有待处理的发送操作，启动异步 WSASendTo */
        if (!udp->send_pending) {
            if (udp_start_send_async(udp, req->buf, req->len, &req->addr) != 0) {
                /* WSASendTo 失败（通常是连接被关闭或网络错误） */
                vox_udp_send_cb cb = req->cb;
                vox_udp_send_req_t* next = req->next;

                udp->send_queue = (void*)next;

                if (cb) {
                    cb(udp, -1, vox_handle_get_data((vox_handle_t*)udp));
                }

                vox_mpool_free(mpool, req);

                /* 继续处理下一个请求（虽然可能也会失败） */
                if (next) {
                    udp_process_send_queue(udp);
                }
            }
            /* WSASendTo 成功提交，等待 IOCP 完成通知 */
        }
        /* 如果已有待处理的发送操作，等待其完成后会再次调用本函数 */
        return;
    }
#endif

    /* 非 IOCP 模式：使用同步 sendto + 可写事件通知 */
    vox_udp_send_req_t* req = (vox_udp_send_req_t*)udp->send_queue;

    while (req) {
        /* 尝试发送数据 */
        int64_t nsent = vox_socket_sendto(&udp->socket, req->buf, req->len, &req->addr);
        
        if (nsent < 0) {
            /* 检查是否是 EAGAIN/EWOULDBLOCK（非阻塞 socket 缓冲区满） */
#ifdef VOX_OS_WINDOWS
            int err = WSAGetLastError();
            if (err == WSAEWOULDBLOCK) {
                /* 非阻塞 socket 缓冲区满，需要等待可写事件，退出循环 */
                break;
            }
#else
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                /* 非阻塞 socket 缓冲区满，需要等待可写事件，退出循环 */
                break;
            }
#endif
            /* 真正的发送错误 */
            vox_udp_send_cb cb = req->cb;
            vox_udp_send_req_t* next = req->next;
            
            /* 从链表中移除当前请求 */
            udp->send_queue = (void*)next;
            
            /* 调用回调（错误） */
            if (cb) {
                cb(udp, -1, vox_handle_get_data((vox_handle_t*)udp));
            }
            
            /* 释放当前请求 */
            vox_mpool_free(mpool, req);
            
            /* 处理下一个请求 */
            req = next;
            continue;
        }
        
        if ((size_t)nsent == req->len) {
            /* 发送完成 */
            vox_udp_send_cb cb = req->cb;
            vox_udp_send_req_t* next = req->next;
            
            /* 从链表中移除当前请求 */
            udp->send_queue = (void*)next;
            
            /* 调用回调 */
            if (cb) {
                cb(udp, 0, vox_handle_get_data((vox_handle_t*)udp));
            }
            
            /* 释放当前请求 */
            vox_mpool_free(mpool, req);
            
            /* 处理下一个请求 */
            req = next;
        } else {
            /* UDP通常不会部分发送，但如果发生，继续尝试 */
            /* 更新缓冲区指针和长度 */
            req->buf = (const char*)req->buf + nsent;
            req->len -= (size_t)nsent;
            /* 继续等待可写事件 */
            break;
        }
    }
    
    /* 如果队列为空，移除可写事件监听，但要确保事件状态一致性 */
    if (!udp->send_queue) {
        uint32_t new_events = udp->backend_events & ~VOX_BACKEND_WRITE;
        /* 只有当事件确实发生变化时才更新 backend */
        if (new_events != udp->backend_events) {
            if (new_events == 0) {
                udp_unregister_backend(udp);
            } else {
                udp_update_backend(udp, new_events);
            }
        }
    }
}

/* 初始化 UDP 句柄 */
int vox_udp_init(vox_udp_t* udp, vox_loop_t* loop) {
    if (!udp || !loop) {
        return -1;
    }
    
    memset(udp, 0, sizeof(vox_udp_t));
    
    /* 初始化句柄基类 */
    if (vox_handle_init((vox_handle_t*)udp, VOX_HANDLE_UDP, loop) != 0) {
        return -1;
    }
    
    /* 初始化 socket */
    udp->socket.fd = VOX_INVALID_SOCKET;
    udp->socket.type = VOX_SOCKET_UDP;
    udp->socket.family = VOX_AF_INET;
    udp->socket.nonblock = false;
    
    udp->bound = false;
    udp->receiving = false;
    udp->closing = false;
    udp->backend_registered = false;
    udp->backend_events = 0;
    udp->backend_data = NULL;
    
    /* 初始化发送队列头指针 */
    udp->send_queue = NULL;
    
#ifdef VOX_OS_WINDOWS
    /* 初始化 Windows IOCP 异步 IO 相关字段（使用扩展 OVERLAPPED 结构） */
    memset(&udp->recv_ov_ex, 0, sizeof(vox_udp_overlapped_ex_t));
    udp->recv_ov_ex.io_type = VOX_UDP_IO_RECV;
    udp->recv_ov_ex.udp = udp;

    memset(&udp->send_ov_ex, 0, sizeof(vox_udp_overlapped_ex_t));
    udp->send_ov_ex.io_type = VOX_UDP_IO_SEND;
    udp->send_ov_ex.udp = udp;

    udp->recv_bufs = NULL;
    udp->recv_buf_count = 0;
    udp->recv_flags = 0;
    udp->recv_from_addr_len = 0;
    udp->recv_pending = false;

    udp->send_bufs = NULL;
    udp->send_buf_count = 0;
    udp->send_to_addr_len = 0;
    udp->send_pending = false;
#endif
    
    return 0;
}

/* 创建 UDP 句柄 */
vox_udp_t* vox_udp_create(vox_loop_t* loop) {
    if (!loop) {
        return NULL;
    }
    
    vox_mpool_t* mpool = vox_loop_get_mpool(loop);
    vox_udp_t* udp = (vox_udp_t*)vox_mpool_alloc(mpool, sizeof(vox_udp_t));
    if (!udp) {
        return NULL;
    }
    
    if (vox_udp_init(udp, loop) != 0) {
        vox_mpool_free(mpool, udp);
        return NULL;
    }
    
    return udp;
}

/* 销毁 UDP 句柄 */
void vox_udp_destroy(vox_udp_t* udp) {
    if (!udp) {
        return;
    }

    /* 标记为正在关闭，防止 IOCP 完成事件处理中的 use-after-free */
    udp->closing = true;

    /* 停止接收 */
    if (udp->receiving) {
        vox_udp_recv_stop(udp);
    }
    
#ifdef VOX_OS_WINDOWS
    /* 取消待处理的异步操作（在关闭 socket 之前） */
    if (udp->socket.fd != VOX_INVALID_SOCKET) {
        if (udp->recv_pending) {
            CancelIoEx((HANDLE)(ULONG_PTR)udp->socket.fd, &udp->recv_ov_ex.overlapped);
        }
        if (udp->send_pending) {
            CancelIoEx((HANDLE)(ULONG_PTR)udp->socket.fd, &udp->send_ov_ex.overlapped);
        }
        
        /* 设置 SO_LINGER 为 0，禁用延迟关闭，确保 socket 立即关闭 */
        struct linger linger_opt;
        linger_opt.l_onoff = 1;
        linger_opt.l_linger = 0;
        setsockopt((SOCKET)udp->socket.fd, SOL_SOCKET, SO_LINGER,
                   (const char*)&linger_opt, sizeof(linger_opt));
    }
#endif
    
    /* 从 backend 注销 */
    udp_unregister_backend(udp);
    
    /* 关闭 socket */
    vox_socket_destroy(&udp->socket);
    
    /* 释放接收缓冲区 */
    if (udp->recv_buf) {
        vox_mpool_t* mpool = vox_loop_get_mpool(udp->handle.loop);
        vox_mpool_free(mpool, udp->recv_buf);
        udp->recv_buf = NULL;
        udp->recv_buf_size = 0;
    }
    
    /* 清理发送队列 */
    if (udp->send_queue) {
        vox_mpool_t* mpool = vox_loop_get_mpool(udp->handle.loop);
        vox_udp_send_req_t* req = (vox_udp_send_req_t*)udp->send_queue;
        while (req) {
            vox_udp_send_req_t* next = req->next;
            /* 调用回调（错误） */
            if (req->cb) {
                req->cb(udp, -1, vox_handle_get_data((vox_handle_t*)udp));
            }
            vox_mpool_free(mpool, req);
            req = next;
        }
        udp->send_queue = NULL;
    }
    
#ifdef VOX_OS_WINDOWS
    /* 释放 IOCP 相关的缓冲区 */
    vox_mpool_t* mpool = vox_loop_get_mpool(udp->handle.loop);
    
    /* 释放 WSARecvFrom 缓冲区 */
    if (udp->recv_bufs) {
        vox_mpool_free(mpool, udp->recv_bufs);
        udp->recv_bufs = NULL;
        udp->recv_buf_count = 0;
    }
    
    /* 释放 WSASendTo 缓冲区 */
    if (udp->send_bufs) {
        vox_mpool_free(mpool, udp->send_bufs);
        udp->send_bufs = NULL;
        udp->send_buf_count = 0;
    }
#endif
    
    /* 关闭句柄 */
    vox_handle_close((vox_handle_t*)udp, NULL);
}

/* 绑定地址 */
int vox_udp_bind(vox_udp_t* udp, const vox_socket_addr_t* addr, unsigned int flags) {
    if (!udp || !addr) {
        return -1;
    }
    
    if (udp->socket.fd != VOX_INVALID_SOCKET) {
        return -1;  /* 已经创建了 socket */
    }
    
    /* 创建 socket */
    if (vox_socket_create(&udp->socket, VOX_SOCKET_UDP, addr->family) != 0) {
        return -1;
    }
    
    /* 设置非阻塞 */
    if (vox_socket_set_nonblock(&udp->socket, true) != 0) {
        vox_socket_destroy(&udp->socket);
        return -1;
    }
    
    /* 设置地址重用 */
    vox_socket_set_reuseaddr(&udp->socket, true);
    
    /* 如果设置了 REUSEPORT 标志，启用端口重用 */
    if (flags & VOX_PORT_REUSE_FLAG) {
        vox_socket_set_reuseport(&udp->socket, true);
    }
    
    /* 绑定地址 */
    if (vox_socket_bind(&udp->socket, addr) != 0) {
        vox_socket_destroy(&udp->socket);
        return -1;
    }
    
    udp->bound = true;
    
    return 0;
}

/* 开始异步接收 */
int vox_udp_recv_start(vox_udp_t* udp, vox_udp_alloc_cb alloc_cb, vox_udp_recv_cb recv_cb) {
    if (!udp) {
        return -1;
    }
    
    if (udp->socket.fd == VOX_INVALID_SOCKET) {
        return -1;
    }
    
    if (udp->receiving) {
        return 0;  /* 已经在接收 */
    }
    
    udp->receiving = true;
    udp->alloc_cb = alloc_cb;
    udp->recv_cb = recv_cb;
    
#ifdef VOX_OS_WINDOWS
    /* Windows IOCP: 检查是否使用 IOCP backend，如果是则启动异步 WSARecvFrom */
    vox_backend_t* backend = vox_loop_get_backend(udp->handle.loop);
    if (backend && vox_backend_get_type(backend) == VOX_BACKEND_TYPE_IOCP) {
        /* 在 IOCP 模式下，socket 必须先关联到 IOCP 才能使用 WSARecvFrom
         * 即使不需要注册 READ 事件，socket 仍然需要关联到 IOCP */
        if (!udp->backend_registered) {
            /* 注册 socket 到 backend（关联到 IOCP），事件可以为 0，因为 IOCP 不依赖事件 */
            if (udp_register_backend(udp, 0) != 0) {
                udp->receiving = false;
                return -1;
            }
        }
        
        /* 启动异步 WSARecvFrom */
        if (udp_start_recv_async(udp) != 0) {
            udp->receiving = false;
            return -1;
        }
        /* IOCP 模式下不需要注册 READ 事件，因为 WSARecvFrom 完成时会通过 IOCP 通知 */
        /* 激活句柄 */
        vox_handle_activate((vox_handle_t*)udp);
        return 0;
    }
#endif
    
    /* 更新 backend 事件，添加可读事件 */
    uint32_t events = udp->backend_events | VOX_BACKEND_READ;
    /* 只有当事件确实发生变化时才更新 backend */
    if (events != udp->backend_events) {
        if (udp_update_backend(udp, events) != 0) {
            udp->receiving = false;
            return -1;
        }
    }
    
    /* 激活句柄 */
    vox_handle_activate((vox_handle_t*)udp);
    
    return 0;
}

/* 停止异步接收 */
int vox_udp_recv_stop(vox_udp_t* udp) {
    if (!udp) {
        return -1;
    }
    
    if (!udp->receiving) {
        return 0;
    }
    
    udp->receiving = false;
    udp->recv_cb = NULL;
    udp->alloc_cb = NULL;
    
#ifdef VOX_OS_WINDOWS
    /* 在 IOCP 模式下，即使停止接收，socket 也需要保持关联到 IOCP（发送操作需要）
     * 只有在 socket 完全没有用途时才注销 */
    vox_backend_t* backend = vox_loop_get_backend(udp->handle.loop);
    if (backend && vox_backend_get_type(backend) == VOX_BACKEND_TYPE_IOCP) {
        /* IOCP 模式下，如果还有发送队列，保持 socket 关联
         * 如果没有发送队列且没有其他用途，可以注销 */
        if (!udp->send_queue && udp->backend_registered) {
            /* 没有发送队列，可以注销 */
            udp_unregister_backend(udp);
        }
        /* 否则保持关联，因为发送操作需要 */
        return 0;
    }
#endif
    
    /* 更新 backend 事件，移除可读事件 */
    uint32_t events = udp->backend_events & ~VOX_BACKEND_READ;
    /* 如果新事件集合与当前事件相同，则无需更新 */
    if (events != udp->backend_events) {
        if (events == 0) {
            /* 如果没有其他事件，从 backend 注销 */
            udp_unregister_backend(udp);
        } else {
            udp_update_backend(udp, events);
        }
    }
    
    return 0;
}

/* 异步发送 */
int vox_udp_send(vox_udp_t* udp, const void* buf, size_t len, 
                 const vox_socket_addr_t* addr, vox_udp_send_cb cb) {
    if (!udp || !buf || len == 0) {
        return -1;
    }
    
    if (udp->socket.fd == VOX_INVALID_SOCKET) {
        return -1;
    }
    
    if (!addr) {
        return -1;  /* UDP 需要目标地址 */
    }
    
    vox_mpool_t* mpool = vox_loop_get_mpool(udp->handle.loop);
    
    /* 如果有待处理的发送请求，直接加入队列 */
    if (udp->send_queue) {
        /* 创建新的发送请求 */
        vox_udp_send_req_t* req = (vox_udp_send_req_t*)vox_mpool_alloc(
            mpool, sizeof(vox_udp_send_req_t));
        if (!req) {
            return -1;
        }
        
        req->buf = buf;
        req->len = len;
        req->addr = *addr;
        req->cb = cb;
        req->next = NULL;
        
        /* 添加到队列末尾 */
        vox_udp_send_req_t* last = (vox_udp_send_req_t*)udp->send_queue;
        while (last->next) {
            last = last->next;
        }
        last->next = req;
        
        /* 确保可写事件已注册 */
        if (!(udp->backend_events & VOX_BACKEND_WRITE)) {
            uint32_t events = udp->backend_events | VOX_BACKEND_WRITE;
            if (udp_update_backend(udp, events) != 0) {
                /* 失败，移除刚添加的请求 */
                last->next = NULL;
                vox_mpool_free(mpool, req);
                return -1;
            }
        }
        
        return 0;
    }
    
#ifdef VOX_OS_WINDOWS
    /* Windows IOCP: 检查是否使用 IOCP backend，如果是则使用异步 WSASendTo */
    vox_backend_t* backend = vox_loop_get_backend(udp->handle.loop);
    if (backend && vox_backend_get_type(backend) == VOX_BACKEND_TYPE_IOCP) {
        /* 在 IOCP 模式下，socket 必须先关联到 IOCP 才能使用 WSASendTo
         * 即使不需要注册 WRITE 事件，socket 仍然需要关联到 IOCP */
        if (!udp->backend_registered) {
            /* 注册 socket 到 backend（关联到 IOCP），事件可以为 0，因为 IOCP 不依赖事件 */
            if (udp_register_backend(udp, 0) != 0) {
                return -1;
            }
        }
        
        /* 创建发送请求 */
        vox_udp_send_req_t* req = (vox_udp_send_req_t*)vox_mpool_alloc(
            mpool, sizeof(vox_udp_send_req_t));
        if (!req) {
            return -1;
        }
        
        req->buf = buf;
        req->len = len;
        req->addr = *addr;
        req->cb = cb;
        req->next = NULL;
        
        /* 添加到队列 */
        if (udp->send_queue) {
            vox_udp_send_req_t* last = (vox_udp_send_req_t*)udp->send_queue;
            while (last->next) {
                last = last->next;
            }
            last->next = req;
        } else {
            udp->send_queue = (void*)req;
        }
        
        /* 如果没有待处理的发送操作，立即启动 */
        if (!udp->send_pending) {
            if (udp_start_send_async(udp, buf, len, addr) != 0) {
                /* 失败，从队列中移除 */
                if (udp->send_queue == (void*)req) {
                    udp->send_queue = NULL;
                } else {
                    vox_udp_send_req_t* prev = (vox_udp_send_req_t*)udp->send_queue;
                    while (prev->next != req) {
                        prev = prev->next;
                    }
                    prev->next = NULL;
                }
                vox_mpool_free(mpool, req);
                return -1;
            }
        }
        
        return 0;
    }
#endif
    
    /* 非 IOCP 模式：使用传统的非阻塞 sendto */
    /* 尝试立即发送 */
    int64_t nsent = vox_socket_sendto(&udp->socket, buf, len, addr);
    if (nsent < 0) {
        /* 检查是否是 EAGAIN/EWOULDBLOCK */
#ifdef VOX_OS_WINDOWS
        int err = WSAGetLastError();
        if (err != WSAEWOULDBLOCK) {
            return -1;
        }
#else
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            return -1;
        }
#endif
        /* 需要加入队列等待可写 */
    } else if ((size_t)nsent == len) {
        /* 全部发送完成 */
        if (cb) {
            cb(udp, 0, vox_handle_get_data((vox_handle_t*)udp));
        }
        return 0;
    } else {
        /* 部分发送（UDP通常不会发生，但处理一下） */
        /* 需要加入队列继续发送 */
    }
    
    /* 创建发送请求并加入队列 */
    vox_udp_send_req_t* req = (vox_udp_send_req_t*)vox_mpool_alloc(
        mpool, sizeof(vox_udp_send_req_t));
    if (!req) {
        return -1;
    }
    
    req->buf = buf;
    req->len = len;
    req->addr = *addr;
    req->cb = cb;
    req->next = NULL;
    
    udp->send_queue = (void*)req;
    
    /* 注册可写事件 */
    uint32_t events = udp->backend_events | VOX_BACKEND_WRITE;
    /* 只有当事件确实发生变化时才更新 backend */
    if (events != udp->backend_events) {
        if (udp_update_backend(udp, events) != 0) {
            vox_mpool_free(mpool, req);
            udp->send_queue = NULL;
            return -1;
        }
    }
    
    return 0;
}

/* 获取本地地址 */
int vox_udp_getsockname(vox_udp_t* udp, vox_socket_addr_t* addr) {
    if (!udp || !addr) {
        return -1;
    }
    
    if (udp->socket.fd == VOX_INVALID_SOCKET) {
        return -1;
    }
    
    return vox_socket_get_local_addr(&udp->socket, addr);
}

/* 设置广播选项 */
int vox_udp_set_broadcast(vox_udp_t* udp, bool enable) {
    if (!udp) {
        return -1;
    }
    
    if (udp->socket.fd == VOX_INVALID_SOCKET) {
        return -1;
    }
    
    return vox_socket_set_broadcast(&udp->socket, enable);
}

/* 设置地址重用选项 */
int vox_udp_set_reuseaddr(vox_udp_t* udp, bool enable) {
    if (!udp) {
        return -1;
    }
    
    if (udp->socket.fd == VOX_INVALID_SOCKET) {
        return -1;
    }
    
    return vox_socket_set_reuseaddr(&udp->socket, enable);
}

#ifdef VOX_OS_WINDOWS
/* 启动异步 WSARecvFrom 操作 */
static int udp_start_recv_async(vox_udp_t* udp) {
    if (!udp || !udp->receiving) {
        return -1;
    }
    
    if (udp->recv_pending) {
        return 0;  /* 已经有待处理的 WSARecvFrom 操作 */
    }
    
    SOCKET sock = (SOCKET)udp->socket.fd;
    
    /* 分配或获取接收缓冲区 */
    void* buf = NULL;
    size_t len = 0;
    
    if (udp->alloc_cb) {
        udp->alloc_cb(udp, VOX_UDP_DEFAULT_RECV_BUF_SIZE, &buf, &len, 
                      vox_handle_get_data((vox_handle_t*)udp));
    } else {
        /* 使用默认缓冲区 */
        vox_mpool_t* mpool = vox_loop_get_mpool(udp->handle.loop);
        if (!udp->recv_buf || udp->recv_buf_size < VOX_UDP_DEFAULT_RECV_BUF_SIZE) {
            if (udp->recv_buf) {
                vox_mpool_free(mpool, udp->recv_buf);
            }
            udp->recv_buf = vox_mpool_alloc(mpool, VOX_UDP_DEFAULT_RECV_BUF_SIZE);
            if (udp->recv_buf) {
                udp->recv_buf_size = VOX_UDP_DEFAULT_RECV_BUF_SIZE;
            }
        }
        buf = udp->recv_buf;
        len = udp->recv_buf_size;
    }
    
    if (!buf || len == 0) {
        return -1;
    }
    
    /* 准备 WSABUF 数组 */
    if (!udp->recv_bufs || udp->recv_buf_count == 0) {
        vox_mpool_t* mpool = vox_loop_get_mpool(udp->handle.loop);
        udp->recv_bufs = (WSABUF*)vox_mpool_alloc(mpool, sizeof(WSABUF));
        if (!udp->recv_bufs) {
            return -1;
        }
        udp->recv_buf_count = 1;
    }
    
    udp->recv_bufs[0].buf = (CHAR*)buf;
    udp->recv_bufs[0].len = (ULONG)len;
    udp->recv_flags = 0;
    
    /* 准备地址缓冲区 */
    udp->recv_from_addr_len = sizeof(udp->recv_from_addr);

    /* 重置 OVERLAPPED 结构（保留 io_type 和 udp 指针） */
    memset(&udp->recv_ov_ex.overlapped, 0, sizeof(OVERLAPPED));

    /* 启动 WSARecvFrom */
    /* 注意：对于异步操作，lpNumberOfBytesRecvd 应该为 NULL，实际传输的字节数通过 GetOverlappedResult 获取 */
    int result = WSARecvFrom(
        sock,
        udp->recv_bufs,
        udp->recv_buf_count,
        NULL,  /* 异步操作，使用 NULL */
        &udp->recv_flags,
        (struct sockaddr*)&udp->recv_from_addr,
        &udp->recv_from_addr_len,
        &udp->recv_ov_ex.overlapped,
        NULL
    );
    
    if (result == SOCKET_ERROR) {
        DWORD error = WSAGetLastError();
        if (error != WSA_IO_PENDING) {
            return -1;
        }
    }
    
    udp->recv_pending = true;
    return 0;
}

/* 启动异步 WSASendTo 操作 */
static int udp_start_send_async(vox_udp_t* udp, const void* buf, size_t len, const vox_socket_addr_t* addr) {
    if (!udp || !buf || len == 0 || !addr) {
        return -1;
    }
    
    if (udp->send_pending) {
        return -1;  /* 已经有待处理的 WSASendTo 操作，应该加入队列 */
    }
    
    SOCKET sock = (SOCKET)udp->socket.fd;
    
    /* 准备 WSABUF 数组 */
    if (!udp->send_bufs || udp->send_buf_count == 0) {
        vox_mpool_t* mpool = vox_loop_get_mpool(udp->handle.loop);
        udp->send_bufs = (WSABUF*)vox_mpool_alloc(mpool, sizeof(WSABUF));
        if (!udp->send_bufs) {
            return -1;
        }
        udp->send_buf_count = 1;
    }
    
    /* 注意：buf 必须是持久的，不能是栈上的临时缓冲区 */
    udp->send_bufs[0].buf = (CHAR*)buf;
    udp->send_bufs[0].len = (ULONG)len;
    
    /* 转换地址 */
    if (addr->family == VOX_AF_INET) {
        struct sockaddr_in* sin = (struct sockaddr_in*)&udp->send_to_addr;
        memset(sin, 0, sizeof(*sin));
        sin->sin_family = AF_INET;
        sin->sin_addr.s_addr = addr->u.ipv4.addr;
        sin->sin_port = addr->u.ipv4.port;
        udp->send_to_addr_len = sizeof(*sin);
    } else {
        struct sockaddr_in6* sin6 = (struct sockaddr_in6*)&udp->send_to_addr;
        memset(sin6, 0, sizeof(*sin6));
        sin6->sin6_family = AF_INET6;
        memcpy(&sin6->sin6_addr, addr->u.ipv6.addr, 16);
        sin6->sin6_port = addr->u.ipv6.port;
        udp->send_to_addr_len = sizeof(*sin6);
    }

    /* 重置 OVERLAPPED 结构（保留 io_type 和 udp 指针） */
    memset(&udp->send_ov_ex.overlapped, 0, sizeof(OVERLAPPED));

    /* 启动 WSASendTo */
    /* 注意：对于异步操作，lpNumberOfBytesSent 应该为 NULL，实际传输的字节数通过 GetOverlappedResult 获取 */
    int result = WSASendTo(
        sock,
        udp->send_bufs,
        udp->send_buf_count,
        NULL,  /* 异步操作，使用 NULL */
        0,
        (struct sockaddr*)&udp->send_to_addr,
        udp->send_to_addr_len,
        &udp->send_ov_ex.overlapped,
        NULL
    );

    if (result == SOCKET_ERROR) {
        DWORD error = WSAGetLastError();
        if (error != WSA_IO_PENDING) {
            /* 错误 10054 (WSAECONNRESET) 表示连接被重置 */
            /* 错误 10053 (WSAECONNABORTED) 表示软件导致连接中止 */
            /* 错误 10057 (WSAENOTCONN) 表示 socket 未连接 */
            if (error == WSAECONNRESET || error == WSAECONNABORTED || error == WSAENOTCONN) {
                VOX_LOG_WARN("WSASendTo failed, connection reset/abort or not connected, error=%lu", error);
            } else {
                VOX_LOG_ERROR("WSASendTo failed, error=%lu", error);
            }
            return -1;
        }
    }

    udp->send_pending = true;
    return 0;
}
#endif

/* 注意：多播相关功能暂未实现 */
