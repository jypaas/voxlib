/*
 * vox_tcp.c - TCP 异步操作实现
 */

#include "vox_tcp.h"
#include "vox_os.h"
#include "vox_loop.h"
#include "vox_backend.h"
#include "vox_mpool.h"
#include "vox_log.h"
#include <string.h>
#include <stdlib.h>

#ifdef VOX_OS_WINDOWS
    #include "vox_iocp.h"  /* 用于 vox_iocp_associate_socket */
    #include <mswsock.h>  /* 用于 AcceptEx, ConnectEx 等 */
#endif

/* 默认读取缓冲区大小 */
#define VOX_TCP_DEFAULT_READ_BUF_SIZE 4096

/* TCP 写入请求结构 */
typedef struct vox_tcp_write_req {
    const void* buf;                    /* 数据缓冲区 */
    size_t len;                         /* 总长度 */
    size_t offset;                      /* 已写入的偏移量 */
    vox_tcp_write_cb cb;                /* 写入完成回调 */
    struct vox_tcp_write_req* next;    /* 下一个请求（链表） */
} vox_tcp_write_req_t;

/* TCP 句柄内部数据 */
typedef struct {
    vox_tcp_t* tcp;
    void* user_data;
} vox_tcp_internal_data_t;

#ifdef VOX_OS_WINDOWS
/* VOX_CONTAINING_RECORD 宏：从结构成员指针获取包含该成员的结构指针 */
#ifndef VOX_CONTAINING_RECORD
#define VOX_CONTAINING_RECORD(address, type, field) \
    ((type *)((char *)(address) - (size_t)(&((type *)0)->field)))
#endif

/* AcceptEx 函数指针类型 */
typedef BOOL (WINAPI *LPFN_ACCEPTEX)(
    SOCKET sListenSocket,
    SOCKET sAcceptSocket,
    PVOID lpOutputBuffer,
    DWORD dwReceiveDataLength,
    DWORD dwLocalAddressLength,
    DWORD dwRemoteAddressLength,
    LPDWORD lpdwBytesReceived,
    LPOVERLAPPED lpOverlapped
);

typedef BOOL (WINAPI *LPFN_CONNECTEX)(
    SOCKET s,
    const struct sockaddr *name,
    int namelen,
    PVOID lpSendBuffer,
    DWORD dwSendDataLength,
    LPDWORD lpdwBytesSent,
    LPOVERLAPPED lpOverlapped
);

/* 获取 AcceptEx 函数指针 */
static LPFN_ACCEPTEX get_acceptex_function(SOCKET s) {
    static LPFN_ACCEPTEX fnAcceptEx = NULL;
    GUID guidAcceptEx = WSAID_ACCEPTEX;
    DWORD bytes;
    
    if (fnAcceptEx == NULL) {
        if (WSAIoctl(s, SIO_GET_EXTENSION_FUNCTION_POINTER,
                     &guidAcceptEx, sizeof(guidAcceptEx),
                     &fnAcceptEx, sizeof(fnAcceptEx),
                     &bytes, NULL, NULL) != 0) {
            return NULL;
        }
    }
    return fnAcceptEx;
}

/* 获取 ConnectEx 函数指针 */
static LPFN_CONNECTEX get_connectex_function(SOCKET s) {
    static LPFN_CONNECTEX fnConnectEx = NULL;
    GUID guidConnectEx = WSAID_CONNECTEX;
    DWORD bytes;
    
    if (fnConnectEx == NULL) {
        if (WSAIoctl(s, SIO_GET_EXTENSION_FUNCTION_POINTER,
                     &guidConnectEx, sizeof(guidConnectEx),
                     &fnConnectEx, sizeof(fnConnectEx),
                     &bytes, NULL, NULL) != 0) {
            return NULL;
        }
    }
    return fnConnectEx;
}
#endif

/* 前向声明 */
static void tcp_process_write_queue(vox_tcp_t* tcp);
static int tcp_register_backend(vox_tcp_t* tcp, uint32_t events);
static int tcp_update_backend(vox_tcp_t* tcp, uint32_t events);
static int tcp_unregister_backend(vox_tcp_t* tcp);

#ifdef VOX_OS_WINDOWS
/* Windows IOCP 异步 IO 操作 */
static int tcp_start_accept_async(vox_tcp_t* server);
static int tcp_start_recv_async(vox_tcp_t* tcp);
static int tcp_start_send_async(vox_tcp_t* tcp, const void* buf, size_t len);
static int tcp_start_connect_async(vox_tcp_t* tcp, const vox_socket_addr_t* addr);
#endif

/* 从 backend 事件回调中获取 TCP 句柄 */
static vox_tcp_t* get_tcp_from_backend_data(void* user_data) {
    if (!user_data) {
        return NULL;
    }
    vox_tcp_internal_data_t* data = (vox_tcp_internal_data_t*)user_data;
    return data->tcp;
}

/* Backend 事件处理回调（导出供 backend 使用） */
void vox_tcp_backend_event_cb(vox_backend_t* backend, int fd, uint32_t events, void* user_data, void* overlapped, size_t bytes_transferred);
void vox_tcp_backend_event_cb(vox_backend_t* backend, int fd, uint32_t events, void* user_data, void* overlapped, size_t bytes_transferred) {
    (void)fd;  /* IOCP 模式下 fd 可能不准确，使用 tcp->socket.fd */

#ifdef VOX_OS_WINDOWS
    /* Windows IOCP：通过 OVERLAPPED 指针获取 TCP 句柄和操作类型 */
    vox_backend_t* backend_check = backend;
    if (backend_check && vox_backend_get_type(backend_check) == VOX_BACKEND_TYPE_IOCP && overlapped) {
        /* 使用 VOX_CONTAINING_RECORD 宏从 OVERLAPPED 指针获取扩展结构 */
        vox_tcp_overlapped_ex_t* ov_ex = VOX_CONTAINING_RECORD(overlapped, vox_tcp_overlapped_ex_t, overlapped);
        vox_tcp_t* tcp = ov_ex->tcp;

        if (!tcp) {
            VOX_LOG_ERROR("IOCP event: overlapped=%p, but tcp pointer is NULL", overlapped);
            return;
        }
    
        /* 根据 IO 操作类型处理事件 */
        switch (ov_ex->io_type) {
            case VOX_TCP_IO_ACCEPT:
                /* AcceptEx 完成 */
                /* 从 OVERLAPPED 指针获取 accept 上下文 */
                {
                    vox_tcp_accept_ctx_t* ctx = vox_container_of(ov_ex, vox_tcp_accept_ctx_t, ov_ex);

                    /* 更新接受上下文 */
                    if (ctx->socket != INVALID_SOCKET) {
                        SOCKET listen_sock = (SOCKET)tcp->socket.fd;
                        if (setsockopt(ctx->socket, SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT,
                                      (char*)&listen_sock, sizeof(listen_sock)) == SOCKET_ERROR) {
                            DWORD error = WSAGetLastError();
                            VOX_LOG_ERROR("SO_UPDATE_ACCEPT_CONTEXT failed, error=%lu", error);
                        }
                    }

                    /* 将 accepted socket 保存到临时字段，供 vox_tcp_accept 使用 */
                    tcp->accept_socket = ctx->socket;

                    /* 清除上下文状态 */
                    ctx->socket = INVALID_SOCKET;
                    ctx->pending = false;
                    tcp->accept_pending_count--;

                    /* 调用连接回调 */
                    if (tcp->connection_cb) {
                        tcp->connection_cb(tcp, 0, vox_handle_get_data((vox_handle_t*)tcp));
                    } else {
                        /* 如果没有回调，关闭 socket */
                        if (tcp->accept_socket != INVALID_SOCKET) {
                            closesocket(tcp->accept_socket);
                            tcp->accept_socket = INVALID_SOCKET;
                        }
                    }

                    /* 立即发起新的 AcceptEx 操作以维护操作池 */
                    tcp_start_accept_async(tcp);
                }
                return;

            case VOX_TCP_IO_RECV:
                /* WSARecv 完成 */
                tcp->recv_pending = false;

                /* 检查 completion key 的 user_data 是否是 TLS 的内部数据
                 * 在 IOCP 模式下，如果 completion key 的 user_data 是 TLS 的内部数据，
                 * 事件应该已经通过 handle_backend_event 路由到 TLS 的回调了
                 * 但为了确保数据被处理，我们需要继续启动下一个 WSARecv（如果还在读取） */
                if (tcp->read_cb) {
                    void* buf = tcp->recv_bufs ? tcp->recv_bufs[0].buf : NULL;

                    if (bytes_transferred > 0) {
                        tcp->read_cb(tcp, (ssize_t)bytes_transferred, buf,
                                     vox_handle_get_data((vox_handle_t*)tcp));

                        /* 如果还在读取，启动下一个 WSARecv */
                        /* 添加连接状态检查，避免对已关闭的连接进行读取 */
                        if (tcp->reading && tcp->socket.fd != VOX_INVALID_SOCKET && tcp->connected) {
                            if (tcp_start_recv_async(tcp) != 0) {
                                /* WSARecv 失败（通常是连接被远端关闭），触发连接关闭 */
                                tcp->read_cb(tcp, 0, NULL, vox_handle_get_data((vox_handle_t*)tcp));
                                vox_tcp_read_stop(tcp);
                            }
                        }
                    } else {
                        /* 连接关闭 */
                        tcp->read_cb(tcp, 0, NULL, vox_handle_get_data((vox_handle_t*)tcp));
                        vox_tcp_read_stop(tcp);
                    }
                } else {
                    /* 没有 read_cb，可能是 TLS 模式
                     * 在 IOCP 模式下，如果 completion key 的 user_data 是 TLS 的内部数据，
                     * 事件应该已经通过 handle_backend_event 路由到 TLS 的回调了
                     * 但为了确保数据被 OpenSSL 的 BIO 读取，我们需要继续启动 WSARecv */
                    if (tcp->reading && bytes_transferred > 0) {
                        /* 继续启动下一个 WSARecv，以便继续读取数据
                         * TLS 的 backend 事件回调会处理这些数据 */
                        /* 添加连接状态检查，避免对已关闭的连接进行读取 */
                        if (tcp->socket.fd != VOX_INVALID_SOCKET && tcp->connected) {
                            if (tcp_start_recv_async(tcp) != 0) {
                                /* WSARecv 失败（通常是连接被远端关闭），停止读取 */
                                /* 对于 TLS 模式，backend 事件回调会处理连接关闭 */
                                vox_tcp_read_stop(tcp);
                            }
                        }
                    } else if (bytes_transferred == 0) {
                        /* 连接关闭，通知 TLS（通过 completion key 的 user_data） */
                        /* 事件应该已经通过 handle_backend_event 路由到 TLS 的回调了 */
                        vox_tcp_read_stop(tcp);
                    }
                }
                return;

            case VOX_TCP_IO_SEND:
                /* WSASend 完成 */
                tcp->send_pending = false;

                /* 处理写入队列中的回调 */
                if (tcp->write_queue) {
                    vox_tcp_write_req_t* req = (vox_tcp_write_req_t*)tcp->write_queue;
                    req->offset += bytes_transferred;

                    if (req->offset >= req->len) {
                        /* 写入完成 */
                        vox_tcp_write_cb cb = req->cb;
                        vox_mpool_t* mpool = vox_loop_get_mpool(tcp->handle.loop);
                        vox_tcp_write_req_t* next = req->next;

                        tcp->write_queue = next;

                        if (cb) {
                            cb(tcp, 0, vox_handle_get_data((vox_handle_t*)tcp));
                        }

                        vox_mpool_free(mpool, req);

                        /* 处理队列中的下一个请求 */
                        tcp_process_write_queue(tcp);
                    } else {
                        /* 继续发送剩余数据 */
                        if (tcp_start_send_async(tcp,
                                                (const char*)req->buf + req->offset,
                                                req->len - req->offset) != 0) {
                            /* WSASend 失败，通知写入错误 */
                            vox_tcp_write_cb cb = req->cb;
                            vox_mpool_t* mpool = vox_loop_get_mpool(tcp->handle.loop);
                            vox_tcp_write_req_t* next = req->next;

                            tcp->write_queue = next;

                            if (cb) {
                                cb(tcp, -1, vox_handle_get_data((vox_handle_t*)tcp));
                            }

                            vox_mpool_free(mpool, req);

                            /* 继续处理队列中的下一个请求 */
                            tcp_process_write_queue(tcp);
                        }
                    }
                }
                return;

            case VOX_TCP_IO_CONNECT:
                /* ConnectEx 完成 */
                tcp->connect_pending = false;
                
                SOCKET sock = (SOCKET)tcp->socket.fd;
                
                /* 更新连接上下文（ConnectEx 要求，必须在检查SO_ERROR之前调用） */
                if (setsockopt(sock, SOL_SOCKET, SO_UPDATE_CONNECT_CONTEXT,
                              NULL, 0) == SOCKET_ERROR) {
                    DWORD error = WSAGetLastError();
                    /* 10057 (WSAENOTCONN) 表示连接未成功，属预期情况，不按 ERROR 打日志 */
                    if (error != 10057) {
                        VOX_LOG_ERROR("SO_UPDATE_CONNECT_CONTEXT failed, error=%lu", error);
                    }
                    /* 如果SO_UPDATE_CONNECT_CONTEXT失败，连接肯定失败 */
                    tcp->connected = false;
                    if (tcp->connect_cb) {
                        vox_tcp_connect_cb saved_cb = tcp->connect_cb;
                        void* saved_user_data = vox_handle_get_data((vox_handle_t*)tcp);
                        tcp->connect_cb = NULL;
                        if (saved_cb) {
                            saved_cb(tcp, -1, saved_user_data);
                        }
                    }
                    /* 移除可写事件监听 */
                    uint32_t current_events = tcp->backend_events;
                    uint32_t new_events = current_events & ~VOX_BACKEND_WRITE;
                    if (new_events == 0) {
                        tcp_unregister_backend(tcp);
                    } else {
                        tcp_update_backend(tcp, new_events);
                    }
                    return;
                }
                
                /* 检查连接是否成功 */
                int connect_error = 0;
                socklen_t error_len = sizeof(connect_error);
                int status = 0;
                
                if (getsockopt(sock, SOL_SOCKET, SO_ERROR, (char*)&connect_error, &error_len) == 0) {
                    if (connect_error == 0) {
                        /* 连接成功 */
                        tcp->connected = true;
                        status = 0;
                    } else {
                        /* 连接失败 */
                        VOX_LOG_ERROR("ConnectEx failed, SO_ERROR=%d", connect_error);
                        tcp->connected = false;
                        status = -1;
                    }
                } else {
                    /* 获取错误失败，假设连接失败 */
                    DWORD getopt_error = WSAGetLastError();
                    VOX_LOG_ERROR("getsockopt(SO_ERROR) failed, WSAGetLastError=%lu", getopt_error);
                    tcp->connected = false;
                    status = -1;
                }

                if (tcp->connect_cb) {
                    vox_tcp_connect_cb saved_cb = tcp->connect_cb;
                    void* saved_user_data = vox_handle_get_data((vox_handle_t*)tcp);
                    tcp->connect_cb = NULL;

                    if (saved_cb) {
                        saved_cb(tcp, status, saved_user_data);
                    }
                }

                /* 移除可写事件监听 */
                uint32_t current_events = tcp->backend_events;
                uint32_t new_events = current_events & ~VOX_BACKEND_WRITE;
                if (new_events == 0) {
                    tcp_unregister_backend(tcp);
                } else {
                    tcp_update_backend(tcp, new_events);
                }
                return;

            default:
                VOX_LOG_ERROR("Unknown IO operation type: %d", ov_ex->io_type);
                return;
        }
    }
#endif

    /* 非 IOCP 模式：传统的事件处理 */
    vox_tcp_t* tcp = get_tcp_from_backend_data(user_data);
    if (!tcp) {
        return;
    }

    (void)backend;
    (void)overlapped;
    (void)bytes_transferred;

    /* 处理错误事件 */
    if (events & VOX_BACKEND_ERROR) {
        if (tcp->connect_cb) {
            tcp->connect_cb(tcp, -1, vox_handle_get_data((vox_handle_t*)tcp));
            tcp->connect_cb = NULL;
        }
        return;
    }
    
    /* 处理连接完成事件 */
    if (tcp->connect_cb && !tcp->connected) {
        /* 检查连接是否成功 */
        int error = 0;
        socklen_t len = sizeof(error);
        if (getsockopt((int)tcp->socket.fd, SOL_SOCKET, SO_ERROR, (char*)&error, &len) == 0) {
            if (error == 0) {
                tcp->connected = true;
                /* 不在这里调用回调，在移除事件后调用 */
            } else {
                /* 连接失败，立即调用回调 */
                VOX_LOG_ERROR("Non-IOCP connect: failed, SO_ERROR=%d", error);
                tcp->connected = false;
                vox_tcp_connect_cb saved_cb = tcp->connect_cb;
                void* saved_user_data = vox_handle_get_data((vox_handle_t*)tcp);
                tcp->connect_cb = NULL;
                if (saved_cb) {
                    saved_cb(tcp, -1, saved_user_data);
                }
                return;
            }
        } else {
            /* 获取错误失败，立即调用回调 */
            //DWORD getopt_error = WSAGetLastError();
            //VOX_LOG_ERROR("Non-IOCP connect: getsockopt(SO_ERROR) failed, WSAGetLastError=%lu", getopt_error);
            tcp->connected = false;
            vox_tcp_connect_cb saved_cb = tcp->connect_cb;
            void* saved_user_data = vox_handle_get_data((vox_handle_t*)tcp);
            tcp->connect_cb = NULL;
            if (saved_cb) {
                saved_cb(tcp, -1, saved_user_data);
            }
            return;
        }
        /* 保存回调，在移除事件后再调用，避免在回调中修改 backend 导致问题 */
        vox_tcp_connect_cb saved_cb = tcp->connect_cb;
        void* saved_user_data = vox_handle_get_data((vox_handle_t*)tcp);
        tcp->connect_cb = NULL;  /* 清除回调，避免重复调用 */
        
        /* 先调用回调，让回调有机会调用 vox_tcp_read_start 来添加 READ 事件 */
        if (saved_cb) {
            saved_cb(tcp, 0, saved_user_data);
        }
        
        /* 移除可写事件监听（连接已完成） */
        /* 注意：回调中可能已经调用了 vox_tcp_read_start 或 vox_tcp_write，
         * 所以需要重新获取当前的 backend_events 并保留必要的事件 */
        uint32_t current_events = tcp->backend_events;
        uint32_t new_events = current_events;
        
        /* 如果只有 WRITE 事件（连接事件），则移除它；否则保留其他事件 */
        if ((current_events == VOX_BACKEND_WRITE) || (current_events == (VOX_BACKEND_WRITE | VOX_BACKEND_ERROR))) {
            new_events = 0;
        } else {
            /* 保留其他事件（如 READ 事件），只移除 WRITE 事件 */
            new_events = current_events & ~VOX_BACKEND_WRITE;
        }
        
        /* 只有当事件确实发生变化时才更新 backend */
        if (new_events != current_events) {
            if (new_events == 0) {
                tcp_unregister_backend(tcp);
            } else {
                tcp_update_backend(tcp, new_events);
            }
        }
        return;
    }
    
    /* 处理可读事件 */
    if (events & VOX_BACKEND_READ) {
        if (tcp->listening) {
            /* 服务器端：接受连接（非 IOCP 模式或同步 IO） */
            /* 每次事件只接受一个连接，如果有多个连接等待，会在下一次事件中处理 */
            /* 这样可以避免复杂的循环逻辑，而且性能也不会太差 */
            if (tcp->connection_cb) {
                tcp->connection_cb(tcp, 0, vox_handle_get_data((vox_handle_t*)tcp));
            }
        } else if (tcp->reading) {
            /* 客户端：读取数据（非 IOCP 模式或同步 IO） */
            if (tcp->read_cb) {
                /* 分配缓冲区 */
                void* buf = NULL;
                size_t len = 0;
                
                if (tcp->alloc_cb) {
                    tcp->alloc_cb(tcp, VOX_TCP_DEFAULT_READ_BUF_SIZE, &buf, &len, 
                                  vox_handle_get_data((vox_handle_t*)tcp));
                } else {
                    /* 使用默认缓冲区 */
                    if (!tcp->read_buf || tcp->read_buf_size < VOX_TCP_DEFAULT_READ_BUF_SIZE) {
                        vox_mpool_t* mpool = vox_loop_get_mpool(tcp->handle.loop);
                        if (tcp->read_buf) {
                            vox_mpool_free(mpool, tcp->read_buf);
                        }
                        tcp->read_buf = vox_mpool_alloc(mpool, VOX_TCP_DEFAULT_READ_BUF_SIZE);
                        if (tcp->read_buf) {
                            tcp->read_buf_size = VOX_TCP_DEFAULT_READ_BUF_SIZE;
                        }
                    }
                    buf = tcp->read_buf;
                    len = tcp->read_buf_size;
                }
                
                if (buf && len > 0) {
                    /* 读取数据 */
                    int64_t nread = vox_socket_recv(&tcp->socket, buf, len);
                    if (nread > 0) {
                        tcp->read_cb(tcp, (ssize_t)nread, buf, 
                                     vox_handle_get_data((vox_handle_t*)tcp));
                    } else if (nread == 0) {
                        /* 连接关闭 */
                        tcp->read_cb(tcp, 0, NULL, vox_handle_get_data((vox_handle_t*)tcp));
                        vox_tcp_read_stop(tcp);
                    } else {
                        /* 读取错误 - 检查是否是 EAGAIN/EWOULDBLOCK */
#ifdef VOX_OS_WINDOWS
                        int err = WSAGetLastError();
                        if (err == WSAEWOULDBLOCK) {
                            /* 非阻塞 socket 暂时没有数据，这是正常的，不需要报告错误 */
                            return;
                        }
#else
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                            /* 非阻塞 socket 暂时没有数据，这是正常的，不需要报告错误 */
                            return;
                        }
#endif
                        /* 真正的读取错误 */
                        tcp->read_cb(tcp, -1, NULL, vox_handle_get_data((vox_handle_t*)tcp));
                    }
                }
            }
        }
    }
    
    /* 处理可写事件（非 IOCP 模式） */
    if (events & VOX_BACKEND_WRITE) {
        /* 处理写入请求队列 */
        tcp_process_write_queue(tcp);
    }
    
    /* 处理挂起事件 */
    if (events & VOX_BACKEND_HANGUP) {
        if (tcp->read_cb) {
            tcp->read_cb(tcp, 0, NULL, vox_handle_get_data((vox_handle_t*)tcp));
        }
        vox_tcp_read_stop(tcp);
    }
}

/* 注册 TCP 句柄到 backend */
static int tcp_register_backend(vox_tcp_t* tcp, uint32_t events) {
    if (!tcp || !tcp->handle.loop) {
        return -1;
    }
    
    vox_backend_t* backend = vox_loop_get_backend(tcp->handle.loop);
    if (!backend) {
        return -1;
    }
    
    if (tcp->socket.fd == VOX_INVALID_SOCKET) {
        return -1;
    }
    
    vox_loop_t* loop = tcp->handle.loop;
    
    /* 在 IOCP 模式下，如果 socket 已经关联到了 IOCP（例如通过 AcceptEx），
     * 我们仍然需要为它创建自己的 completion key。
     * 即使 socket 已经通过 AcceptEx 关联到了 IOCP（使用 listen_key），
     * 我们也需要为它创建自己的 key，这样 WSARecv/WSASend 完成事件才能正确路由。
     * 
     * 注意：vox_iocp_add 会处理已关联的 socket，它会更新 completion key。
     */
#ifdef VOX_OS_WINDOWS
    vox_backend_type_t backend_type = vox_backend_get_type(backend);
    if (backend_type == VOX_BACKEND_TYPE_IOCP && tcp->backend_registered) {
        /* Socket 已经标记为已注册，但可能还没有自己的 completion key
         * 检查是否在 key_map 中 */
        vox_iocp_t* iocp = (vox_iocp_t*)vox_backend_get_iocp_impl(backend);
        if (iocp) {
            int fd = (int)tcp->socket.fd;
            ULONG_PTR existing_key = vox_iocp_get_completion_key(iocp, fd);
            if (existing_key != 0) {
                /* 已经有 completion key，只需更新事件状态 */
                tcp->backend_events = events;
                return 0;
            }
        }
    }
#endif
    
    /* 创建内部数据 */
    vox_mpool_t* mpool = vox_loop_get_mpool(loop);
    vox_tcp_internal_data_t* data = (vox_tcp_internal_data_t*)vox_mpool_alloc(
        mpool, sizeof(vox_tcp_internal_data_t));
    if (!data) {
        return -1;
    }
    
    data->tcp = tcp;
    data->user_data = vox_handle_get_data((vox_handle_t*)tcp);
    
    /* 注册到 backend，使用 TCP 的事件回调 */
    int fd = (int)tcp->socket.fd;
    int ret = vox_backend_add(backend, fd, events, data);
    if (ret != 0) {
        VOX_LOG_ERROR("tcp_register_backend: vox_backend_add failed (ret=%d)", ret);
        vox_mpool_free(mpool, data);
        return -1;
    }
    
    /* 注意：backend 的事件回调需要设置为 vox_tcp_backend_event_cb */
    /* 这需要在 backend 层面支持，或者通过 backend 的回调机制实现 */
    
    tcp->backend_registered = true;
    tcp->backend_events = events;
    
    return 0;
}

/* 更新 backend 注册的事件 */
static int tcp_update_backend(vox_tcp_t* tcp, uint32_t events) {
    if (!tcp || !tcp->handle.loop) {
        return -1;
    }
    
    vox_backend_t* backend = vox_loop_get_backend(tcp->handle.loop);
    if (!backend) {
        return -1;
    }
    
    if (!tcp->backend_registered) {
        return tcp_register_backend(tcp, events);
    }
    
    int fd = (int)tcp->socket.fd;
    if (vox_backend_modify(backend, fd, events) != 0) {
        VOX_LOG_ERROR("tcp_update_backend: modify failed");
        return -1;
    }
    
    tcp->backend_events = events;
    return 0;
}

/* 从 backend 注销 */
static int tcp_unregister_backend(vox_tcp_t* tcp) {
    if (!tcp || !tcp->handle.loop) {
        return -1;
    }
    
    vox_backend_t* backend = vox_loop_get_backend(tcp->handle.loop);
    if (!backend) {
        return -1;
    }
    
    if (!tcp->backend_registered) {
        return 0;
    }
    
    int fd = (int)tcp->socket.fd;
    vox_backend_remove(backend, fd);
    
    tcp->backend_registered = false;
    tcp->backend_events = 0;
    
    return 0;
}

/* 处理写入请求队列 */
static void tcp_process_write_queue(vox_tcp_t* tcp) {
    if (!tcp || !tcp->write_queue) {
        return;
    }

    vox_mpool_t* mpool = vox_loop_get_mpool(tcp->handle.loop);

#ifdef VOX_OS_WINDOWS
    /* Windows IOCP: 检查是否使用 IOCP backend，如果是则使用异步 WSASend */
    vox_backend_t* backend = vox_loop_get_backend(tcp->handle.loop);
    if (backend && vox_backend_get_type(backend) == VOX_BACKEND_TYPE_IOCP) {
        /* IOCP 模式：一次只处理队列头的一个请求（使用异步 WSASend） */
        vox_tcp_write_req_t* req = (vox_tcp_write_req_t*)tcp->write_queue;

        /* 计算剩余需要写入的数据 */
        const void* buf = (const char*)req->buf + req->offset;
        size_t remaining = req->len - req->offset;

        if (remaining == 0) {
            /* 当前请求已完成（这不应该发生，但防御性处理） */
            vox_tcp_write_cb cb = req->cb;
            vox_tcp_write_req_t* next = req->next;

            tcp->write_queue = (void*)next;

            if (cb) {
                cb(tcp, 0, vox_handle_get_data((vox_handle_t*)tcp));
            }

            vox_mpool_free(mpool, req);

            /* 递归处理下一个请求 */
            if (next) {
                tcp_process_write_queue(tcp);
            }
            return;
        }

        /* 如果没有待处理的发送操作，启动异步 WSASend */
        if (!tcp->send_pending) {
            if (tcp_start_send_async(tcp, buf, remaining) != 0) {
                /* WSASend 失败（通常是连接被关闭） */
                vox_tcp_write_cb cb = req->cb;
                vox_tcp_write_req_t* next = req->next;

                tcp->write_queue = (void*)next;

                if (cb) {
                    cb(tcp, -1, vox_handle_get_data((vox_handle_t*)tcp));
                }

                vox_mpool_free(mpool, req);

                /* 继续处理下一个请求（虽然可能也会失败） */
                if (next) {
                    tcp_process_write_queue(tcp);
                }
            }
            /* WSASend 成功提交，等待 IOCP 完成通知 */
        }
        /* 如果已有待处理的发送操作，等待其完成后会再次调用本函数 */
        return;
    }
#endif

    /* 非 IOCP 模式：使用同步 send + 可写事件通知 */
    vox_tcp_write_req_t* req = (vox_tcp_write_req_t*)tcp->write_queue;

    while (req) {
        /* 计算剩余需要写入的数据 */
        const void* buf = (const char*)req->buf + req->offset;
        size_t remaining = req->len - req->offset;

        if (remaining == 0) {
            /* 当前请求已完成 */
            vox_tcp_write_cb cb = req->cb;
            vox_tcp_write_req_t* next = req->next;

            /* 从链表中移除当前请求 */
            tcp->write_queue = (void*)next;

            /* 调用回调 */
            if (cb) {
                cb(tcp, 0, vox_handle_get_data((vox_handle_t*)tcp));
            }

            /* 释放当前请求 */
            vox_mpool_free(mpool, req);

            /* 处理下一个请求 */
            req = next;
            continue;
        }

        /* 尝试写入剩余数据 */
        int64_t nwritten = vox_socket_send(&tcp->socket, buf, remaining);

        if (nwritten < 0) {
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
            /* 真正的写入错误 */
            vox_tcp_write_cb cb = req->cb;
            vox_tcp_write_req_t* next = req->next;

            /* 从链表中移除当前请求 */
            tcp->write_queue = (void*)next;

            /* 调用回调（错误） */
            if (cb) {
                cb(tcp, -1, vox_handle_get_data((vox_handle_t*)tcp));
            }

            /* 释放当前请求 */
            vox_mpool_free(mpool, req);

            /* 处理下一个请求 */
            req = next;
            continue;
        }

        /* 更新偏移量 */
        req->offset += (size_t)nwritten;

        if (req->offset >= req->len) {
            /* 当前请求已完成 */
            vox_tcp_write_cb cb = req->cb;
            vox_tcp_write_req_t* next = req->next;

            /* 从链表中移除当前请求 */
            tcp->write_queue = (void*)next;

            /* 调用回调 */
            if (cb) {
                cb(tcp, 0, vox_handle_get_data((vox_handle_t*)tcp));
            }

            /* 释放当前请求 */
            vox_mpool_free(mpool, req);

            /* 处理下一个请求 */
            req = next;
        } else {
            /* 部分写入，需要继续等待可写事件 */
            break;
        }
    }

    /* 如果队列为空，移除可写事件监听，但要小心处理连接完成后的状态 */
    if (!tcp->write_queue) {
        uint32_t new_events = tcp->backend_events & ~VOX_BACKEND_WRITE;
        /* 如果新事件集合与当前事件相同，则无需更新 */
        if (new_events != tcp->backend_events) {
            if (new_events == 0) {
                tcp_unregister_backend(tcp);
            } else {
                tcp_update_backend(tcp, new_events);
            }
        }
    }
}

/* 初始化 TCP 句柄 */
int vox_tcp_init(vox_tcp_t* tcp, vox_loop_t* loop) {
    if (!tcp || !loop) {
        return -1;
    }
    
    memset(tcp, 0, sizeof(vox_tcp_t));
    
    /* 初始化句柄基类 */
    if (vox_handle_init((vox_handle_t*)tcp, VOX_HANDLE_TCP, loop) != 0) {
        return -1;
    }
    
    /* 初始化 socket */
    tcp->socket.fd = VOX_INVALID_SOCKET;
    tcp->socket.type = VOX_SOCKET_TCP;
    tcp->socket.family = VOX_AF_INET;
    tcp->socket.nonblock = false;
    
    tcp->connected = false;
    tcp->listening = false;
    tcp->reading = false;
    tcp->backend_registered = false;
    tcp->backend_events = 0;
    
    /* 初始化写入队列头指针 */
    tcp->write_queue = NULL;
    
#ifdef VOX_OS_WINDOWS
    /* 初始化 Windows IOCP 异步 IO 相关字段（使用扩展 OVERLAPPED 结构） */
    memset(&tcp->read_ov_ex, 0, sizeof(vox_tcp_overlapped_ex_t));
    tcp->read_ov_ex.io_type = VOX_TCP_IO_RECV;
    tcp->read_ov_ex.tcp = tcp;

    memset(&tcp->write_ov_ex, 0, sizeof(vox_tcp_overlapped_ex_t));
    tcp->write_ov_ex.io_type = VOX_TCP_IO_SEND;
    tcp->write_ov_ex.tcp = tcp;

    memset(&tcp->connect_ov_ex, 0, sizeof(vox_tcp_overlapped_ex_t));
    tcp->connect_ov_ex.io_type = VOX_TCP_IO_CONNECT;
    tcp->connect_ov_ex.tcp = tcp;

    /* 初始化 AcceptEx 操作池（在 listen 时分配） */
    tcp->accept_pool = NULL;
    tcp->accept_pool_size = 0;
    tcp->accept_pending_count = 0;
    tcp->accept_socket = INVALID_SOCKET;

    tcp->recv_bufs = NULL;
    tcp->recv_buf_count = 0;
    tcp->recv_flags = 0;
    tcp->recv_pending = false;

    tcp->send_bufs = NULL;
    tcp->send_buf_count = 0;
    tcp->send_pending = false;

    tcp->connect_pending = false;
#endif
    
    return 0;
}

/* 创建 TCP 句柄 */
vox_tcp_t* vox_tcp_create(vox_loop_t* loop) {
    if (!loop) {
        return NULL;
    }
    
    vox_mpool_t* mpool = vox_loop_get_mpool(loop);
    vox_tcp_t* tcp = (vox_tcp_t*)vox_mpool_alloc(mpool, sizeof(vox_tcp_t));
    if (!tcp) {
        return NULL;
    }
    
    if (vox_tcp_init(tcp, loop) != 0) {
        vox_mpool_free(mpool, tcp);
        return NULL;
    }
    
    return tcp;
}

/* 销毁 TCP 句柄 */
void vox_tcp_destroy(vox_tcp_t* tcp) {
    if (!tcp) {
        return;
    }
    
    /* 停止读取 */
    if (tcp->reading) {
        vox_tcp_read_stop(tcp);
    }
    
    /* 从 backend 注销 */
    tcp_unregister_backend(tcp);
    
    /* 关闭 socket */
    vox_socket_destroy(&tcp->socket);
    
    /* 释放读取缓冲区 */
    if (tcp->read_buf) {
        vox_mpool_t* mpool = vox_loop_get_mpool(tcp->handle.loop);
        vox_mpool_free(mpool, tcp->read_buf);
        tcp->read_buf = NULL;
        tcp->read_buf_size = 0;
    }
    
    /* 清理写入队列 */
    if (tcp->write_queue) {
        vox_mpool_t* mpool = vox_loop_get_mpool(tcp->handle.loop);
        vox_tcp_write_req_t* req = (vox_tcp_write_req_t*)tcp->write_queue;
        while (req) {
            vox_tcp_write_req_t* next = req->next;
            /* 调用回调（错误） */
            if (req->cb) {
                req->cb(tcp, -1, vox_handle_get_data((vox_handle_t*)tcp));
            }
            vox_mpool_free(mpool, req);
            req = next;
        }
        tcp->write_queue = NULL;
    }
    
#ifdef VOX_OS_WINDOWS
    /* 清理 Windows IOCP 异步 IO 相关资源 */
    vox_mpool_t* mpool = vox_loop_get_mpool(tcp->handle.loop);

    /* 取消待处理的异步操作 */
    /* 取消 AcceptEx 操作池中的所有待处理操作 */
    if (tcp->accept_pool && tcp->socket.fd != VOX_INVALID_SOCKET) {
        for (int i = 0; i < tcp->accept_pool_size; i++) {
            vox_tcp_accept_ctx_t* ctx = &tcp->accept_pool[i];
            if (ctx->pending) {
                CancelIoEx((HANDLE)(ULONG_PTR)tcp->socket.fd, &ctx->ov_ex.overlapped);
            }
        }
    }
    if (tcp->recv_pending && tcp->socket.fd != VOX_INVALID_SOCKET) {
        CancelIoEx((HANDLE)(ULONG_PTR)tcp->socket.fd, &tcp->read_ov_ex.overlapped);
    }
    if (tcp->send_pending && tcp->socket.fd != VOX_INVALID_SOCKET) {
        CancelIoEx((HANDLE)(ULONG_PTR)tcp->socket.fd, &tcp->write_ov_ex.overlapped);
    }
    if (tcp->connect_pending && tcp->socket.fd != VOX_INVALID_SOCKET) {
        CancelIoEx((HANDLE)(ULONG_PTR)tcp->socket.fd, &tcp->connect_ov_ex.overlapped);
    }
    
    /* 清理 AcceptEx 操作池 */
    if (tcp->accept_pool) {
        for (int i = 0; i < tcp->accept_pool_size; i++) {
            vox_tcp_accept_ctx_t* ctx = &tcp->accept_pool[i];

            /* 关闭待处理的 accept socket */
            if (ctx->socket != INVALID_SOCKET) {
                closesocket(ctx->socket);
                ctx->socket = INVALID_SOCKET;
            }

            /* 释放缓冲区 */
            if (ctx->buffer) {
                vox_mpool_free(mpool, ctx->buffer);
                ctx->buffer = NULL;
            }
        }

        /* 释放操作池数组 */
        vox_mpool_free(mpool, tcp->accept_pool);
        tcp->accept_pool = NULL;
        tcp->accept_pool_size = 0;
        tcp->accept_pending_count = 0;
    }

    /* 清理临时 accept socket */
    if (tcp->accept_socket != INVALID_SOCKET) {
        closesocket(tcp->accept_socket);
        tcp->accept_socket = INVALID_SOCKET;
    }
    
    /* 释放 WSARecv 缓冲区 */
    if (tcp->recv_bufs) {
        vox_mpool_free(mpool, tcp->recv_bufs);
        tcp->recv_bufs = NULL;
        tcp->recv_buf_count = 0;
    }
    
    /* 释放 WSASend 缓冲区 */
    if (tcp->send_bufs) {
        vox_mpool_free(mpool, tcp->send_bufs);
        tcp->send_bufs = NULL;
        tcp->send_buf_count = 0;
    }
#endif
    
    /* 关闭句柄 */
    vox_handle_close((vox_handle_t*)tcp, NULL);
}

/* 绑定地址 */
int vox_tcp_bind(vox_tcp_t* tcp, const vox_socket_addr_t* addr, unsigned int flags) {
    if (!tcp || !addr) {
        return -1;
    }
    
    if (tcp->socket.fd != VOX_INVALID_SOCKET) {
        return -1;  /* 已经创建了 socket */
    }
    
    /* 创建 socket */
    if (vox_socket_create(&tcp->socket, VOX_SOCKET_TCP, addr->family) != 0) {
        return -1;
    }
    
    /* 设置非阻塞 */
    if (vox_socket_set_nonblock(&tcp->socket, true) != 0) {
        vox_socket_destroy(&tcp->socket);
        return -1;
    }
    
    /* 设置地址重用 */
    vox_socket_set_reuseaddr(&tcp->socket, true);
    
    /* 如果设置了 REUSEPORT 标志，启用端口重用 */
    if (flags & VOX_PORT_REUSE_FLAG) {
        vox_socket_set_reuseport(&tcp->socket, true);
    }
    
    /* 绑定地址 */
    if (vox_socket_bind(&tcp->socket, addr) != 0) {
        vox_socket_destroy(&tcp->socket);
        return -1;
    }
    
    (void)flags;  /* 暂时未使用 */
    
    return 0;
}

/* 开始监听 */
int vox_tcp_listen(vox_tcp_t* tcp, int backlog, vox_tcp_connection_cb cb) {
    if (!tcp) {
        return -1;
    }
    
    if (tcp->socket.fd == VOX_INVALID_SOCKET) {
        return -1;  /* 需要先绑定 */
    }
    
    if (tcp->listening) {
        return -1;  /* 已经在监听 */
    }
    
    /* 开始监听 */
    if (vox_socket_listen(&tcp->socket, backlog) != 0) {
        return -1;
    }
    
    tcp->listening = true;
    tcp->connection_cb = cb;
    
    /* 注册到 backend，监听可读事件（接受连接） */
    if (tcp_register_backend(tcp, VOX_BACKEND_READ) != 0) {
        tcp->listening = false;
        return -1;
    }
    
#ifdef VOX_OS_WINDOWS
    /* Windows IOCP: 检查是否使用 IOCP backend，如果是则启动异步 AcceptEx */
    vox_backend_t* backend = vox_loop_get_backend(tcp->handle.loop);
    if (backend) {
        vox_backend_type_t backend_type = vox_backend_get_type(backend);
        if (backend_type == VOX_BACKEND_TYPE_IOCP) {
            /* 启动异步 AcceptEx */
            if (tcp_start_accept_async(tcp) != 0) {
                tcp_unregister_backend(tcp);
                tcp->listening = false;
                return -1;
            }
        }
    }
#endif
    
    /* 激活句柄 */
    vox_handle_activate((vox_handle_t*)tcp);
    
    return 0;
}

/* 接受连接 */
int vox_tcp_accept(vox_tcp_t* server, vox_tcp_t* client) {
    if (!server || !client) {
        return -1;
    }
    
    if (!server->listening) {
        return -1;
    }
    
    if (client->socket.fd != VOX_INVALID_SOCKET) {
        return -1;  /* 客户端 socket 已经存在 */
    }
    
#ifdef VOX_OS_WINDOWS
    /* Windows IOCP: 检查是否使用 IOCP backend 和 AcceptEx */
    vox_backend_t* backend = vox_loop_get_backend(server->handle.loop);
    if (backend && vox_backend_get_type(backend) == VOX_BACKEND_TYPE_IOCP &&
        server->accept_socket != INVALID_SOCKET) {
        /* AcceptEx 已经完成，使用预先创建的 socket */
        SOCKET accept_sock = server->accept_socket;
        client->socket.fd = (vox_socket_fd_t)accept_sock;
        client->socket.type = server->socket.type;
        client->socket.family = server->socket.family;
        client->socket.nonblock = true;
        client->connected = true;
        
        /* 从 AcceptEx 缓冲区提取地址信息（可选） */
        /* 注意：地址信息已经在 AcceptEx 操作池的缓冲区中，如果需要可以解析 */

        /* 清除 accept_socket，准备下一个 AcceptEx */
        server->accept_socket = INVALID_SOCKET;
        
        /* 在 IOCP 模式下，接受的 socket 已经通过 AcceptEx 关联到了 IOCP，
         * 但使用的是监听 socket 的 completion key。
         * 
         * 重要：虽然 socket 已经关联到 IOCP（使用 listen_key），但我们需要为客户端 socket
         * 创建自己的 completion key，这样 WSARecv 完成事件才能正确路由。
         * 
         * 但是，Windows 不允许更改已关联 socket 的 completion key。
         * 所以，我们只能接受这个限制：WSARecv 完成事件会使用 listen_key，路由到 server 句柄。
         * 我们需要在 server 句柄的事件处理中，通过 overlapped 指针来查找对应的客户端 TCP 句柄。
         * 
         * 当前实现：我们标记客户端 socket 为已注册，但实际上 completion key 仍然是 listen_key。
         * 在事件处理中，我们需要通过 overlapped 指针来识别这是客户端 socket 的操作。
         */
        client->backend_registered = true;  /* 标记为已注册，但实际上 completion key 仍然是 listen_key */
        client->backend_events = 0;         /* 初始事件为空 */
        
        /* 对于通过 AcceptEx 接受的 socket，需要更新接受上下文 */
        /* 注意：SO_UPDATE_ACCEPT_CONTEXT 需要传递监听 socket 的 SOCKET 句柄 */
        SOCKET listen_sock = (SOCKET)server->socket.fd;
        if (setsockopt(accept_sock, SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT, 
                      (char*)&listen_sock, sizeof(listen_sock)) == SOCKET_ERROR) {
            /* 不返回错误，继续处理 */
        }
        
        /* 重要：在 AcceptEx 完成后，我们需要确保 socket 已经正确关联到 IOCP
         * 虽然 socket 已经通过 AcceptEx 关联到 IOCP（使用 listen_key），
         * 但我们需要为客户端 socket 创建自己的 completion key。
         * 
         * 但是，Windows 不允许更改已关联 socket 的 completion key。
         * 所以，我们需要在调用 WSARecv 之前，确保 socket 已经正确关联。
         * 
         * 实际上，socket 已经关联到 IOCP（使用 listen_key），这是 AcceptEx 的要求。
         * 我们无法更改 completion key，所以 WSARecv 完成事件会使用 listen_key。
         * 我们需要在事件处理中正确处理这种情况。
         * 
         * 但是，WSAEFAULT 错误通常意味着参数无效，而不是 completion key 问题。
         * 让我们检查一下是否是因为 socket 还没有完全准备好。
         * 
         * 修复：添加更详细的错误检查和日志
         */
        DWORD update_error = WSAGetLastError();
        if (update_error != 0) {
            VOX_LOG_WARN("SO_UPDATE_ACCEPT_CONTEXT failed, error=%lu. This may cause WSARecv issues later.", update_error);
        }
        
        return 0;
    }
#endif
    
    /* 非 IOCP 模式或同步 IO：使用传统的 accept */
    if (vox_socket_accept(&server->socket, &client->socket, NULL) != 0) {
        /* 检查是否是 EAGAIN/EWOULDBLOCK（非阻塞模式下暂时没有连接） */
#ifdef VOX_OS_WINDOWS
        int err = WSAGetLastError();
        if (err == WSAEWOULDBLOCK || err == WSAEINPROGRESS) {
            /* 暂时没有连接，这是正常的，返回错误但不记录日志 */
            return -1;
        }
#else
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            /* 暂时没有连接，这是正常的，返回错误但不记录日志 */
            return -1;
        }
#endif
        /* 其他错误 */
        return -1;
    }
    
    /* 设置非阻塞 */
    vox_socket_set_nonblock(&client->socket, true);
    
    client->connected = true;
    
    return 0;
}

/* 异步连接 */
int vox_tcp_connect(vox_tcp_t* tcp, const vox_socket_addr_t* addr, vox_tcp_connect_cb cb) {
    if (!tcp || !addr) {
        return -1;
    }
    
    if (tcp->socket.fd != VOX_INVALID_SOCKET) {
        return -1;  /* socket 已经创建 */
    }
    
    if (tcp->connected) {
        return -1;  /* 已经连接 */
    }
    
    /* 创建 socket */
    if (vox_socket_create(&tcp->socket, VOX_SOCKET_TCP, addr->family) != 0) {
        return -1;
    }
    
    /* 设置非阻塞 */
    if (vox_socket_set_nonblock(&tcp->socket, true) != 0) {
        vox_socket_destroy(&tcp->socket);
        return -1;
    }
    
    /* 尝试连接 */
    tcp->connect_cb = cb;
    
#ifdef VOX_OS_WINDOWS
    /* Windows IOCP: 检查是否使用 IOCP backend，如果是则使用异步 ConnectEx */
    vox_backend_t* backend = vox_loop_get_backend(tcp->handle.loop);
    if (backend && vox_backend_get_type(backend) == VOX_BACKEND_TYPE_IOCP) {
        /* 先绑定到任意地址（ConnectEx 要求） */
        struct sockaddr_storage bind_addr;
        socklen_t bind_len;
        if (addr->family == VOX_AF_INET) {
            struct sockaddr_in* sin = (struct sockaddr_in*)&bind_addr;
            memset(sin, 0, sizeof(*sin));
            sin->sin_family = AF_INET;
            sin->sin_addr.s_addr = INADDR_ANY;
            sin->sin_port = 0;
            bind_len = sizeof(*sin);
        } else {
            struct sockaddr_in6* sin6 = (struct sockaddr_in6*)&bind_addr;
            memset(sin6, 0, sizeof(*sin6));
            sin6->sin6_family = AF_INET6;
            sin6->sin6_addr = in6addr_any;
            sin6->sin6_port = 0;
            bind_len = sizeof(*sin6);
        }
        
        if (bind((SOCKET)tcp->socket.fd, (struct sockaddr*)&bind_addr, bind_len) != 0) {
            vox_socket_destroy(&tcp->socket);
            return -1;
        }
        
        /* 注册到 backend（ConnectEx 需要 socket 关联到 IOCP） */
        if (tcp_register_backend(tcp, VOX_BACKEND_WRITE | VOX_BACKEND_ERROR) != 0) {
            vox_socket_destroy(&tcp->socket);
            return -1;
        }
        
        /* 启动异步 ConnectEx */
        if (tcp_start_connect_async(tcp, addr) != 0) {
            tcp_unregister_backend(tcp);
            vox_socket_destroy(&tcp->socket);
            return -1;
        }
        
        /* 激活句柄 */
        vox_handle_activate((vox_handle_t*)tcp);
        return 0;
    }
#endif
    
    /* 非 IOCP 模式：使用传统的非阻塞 connect */
    if (vox_socket_connect(&tcp->socket, addr) != 0) {
        /* 非阻塞连接，检查错误 */
#ifdef VOX_OS_WINDOWS
        int err = WSAGetLastError();
        if (err != WSAEWOULDBLOCK) {
            vox_socket_destroy(&tcp->socket);
            return -1;
        }
#else
        if (errno != EINPROGRESS) {
            vox_socket_destroy(&tcp->socket);
            return -1;
        }
#endif
    } else {
        /* connect() 返回0，但在Windows上，即使connect()返回0，连接也可能还没有完成 */
        /* 为了安全起见，在Windows上我们总是等待可写事件来确认连接完成 */
        /* 这样可以避免在连接还没有真正建立时就认为连接成功 */
#ifdef VOX_OS_WINDOWS
        /* 在Windows上，即使connect()返回0，也等待可写事件来确认连接完成 */
        /* 继续下面的代码，注册可写事件等待连接完成 */
#else
        /* 在Unix系统上，如果connect()返回0，通常表示连接立即成功 */
        /* 但为了安全，仍然检查SO_ERROR */
        int connect_error = 0;
        socklen_t error_len = sizeof(connect_error);
        
        if (getsockopt((int)tcp->socket.fd, SOL_SOCKET, SO_ERROR, (char*)&connect_error, &error_len) == 0) {
            if (connect_error == 0) {
                /* 连接确实成功 */
                tcp->connected = true;
                
                /* 即使连接立即成功，也要注册到 backend，因为回调中可能会添加读写事件 */
                /* 使用 READ 事件，因为我们跳过了连接事件，现在准备好处理读写 */
                if (tcp_register_backend(tcp, VOX_BACKEND_READ) != 0) {
                    vox_socket_destroy(&tcp->socket);
                    return -1;
                }
                
                /* 激活句柄 */
                vox_handle_activate((vox_handle_t*)tcp);
                
                /* 调用回调函数 - 此时backend已经注册，可以安全地添加读写事件 */
                if (cb) {
                    cb(tcp, 0, vox_handle_get_data((vox_handle_t*)tcp));
                }
                return 0;
            } else {
                /* 连接失败，即使connect()返回0 */
                VOX_LOG_ERROR("vox_tcp_connect: connect() returned 0 but SO_ERROR=%d, connection failed", connect_error);
                vox_socket_destroy(&tcp->socket);
                return -1;
            }
        } else {
            /* 无法获取SO_ERROR，可能是连接还在进行中，需要等待 */
            /* 继续下面的代码，注册可写事件等待连接完成 */
        }
#endif
    }
    
    /* 注册到 backend，监听可写事件（连接完成） */
    if (tcp_register_backend(tcp, VOX_BACKEND_WRITE | VOX_BACKEND_ERROR) != 0) {
        vox_socket_destroy(&tcp->socket);
        return -1;
    }
    
    /* 激活句柄 */
    vox_handle_activate((vox_handle_t*)tcp);
    
    return 0;
}

/* 开始异步读取 */
int vox_tcp_read_start(vox_tcp_t* tcp, vox_tcp_alloc_cb alloc_cb, vox_tcp_read_cb read_cb) {
    if (!tcp) {
        return -1;
    }
    
    if (tcp->socket.fd == VOX_INVALID_SOCKET) {
        return -1;
    }
    
    if (tcp->reading) {
        return 0;  /* 已经在读取 */
    }
    
    tcp->reading = true;
    tcp->alloc_cb = alloc_cb;
    tcp->read_cb = read_cb;
    
#ifdef VOX_OS_WINDOWS
    /* Windows IOCP: 检查是否使用 IOCP backend，如果是则启动异步 WSARecv */
    vox_backend_t* backend = vox_loop_get_backend(tcp->handle.loop);
    if (backend && vox_backend_get_type(backend) == VOX_BACKEND_TYPE_IOCP) {
        /* 在 IOCP 模式下，客户端 socket 需要先注册到 backend
         * 这样它才能有正确的 completion key，并且事件能够被正确处理
         * 
         * 注意：即使 socket 已经通过 AcceptEx 关联到了 IOCP（backend_registered = true），
         * 我们也需要为它创建自己的 completion key，这样 WSARecv 完成事件才能正确路由。
         */
        /* 注册到 backend（这会为 socket 创建自己的 completion key）
         * 如果 socket 已经关联到 IOCP（通过 AcceptEx），vox_iocp_add 会更新 completion key */
        if (tcp_register_backend(tcp, VOX_BACKEND_READ) != 0) {
            tcp->reading = false;
            return -1;
        }
        
        /* 启动异步 WSARecv */
        if (tcp_start_recv_async(tcp) != 0) {
            tcp->reading = false;
            return -1;
        }
        /* IOCP 模式下不需要注册 READ 事件，因为 WSARecv 完成时会通过 IOCP 通知 */
        return 0;
    }
#endif
    
    /* 更新 backend 事件，添加可读事件 */
    uint32_t events = tcp->backend_events | VOX_BACKEND_READ;
    if (tcp_update_backend(tcp, events) != 0) {
        tcp->reading = false;
        return -1;
    }
    
    return 0;
}

/* 停止异步读取 */
int vox_tcp_read_stop(vox_tcp_t* tcp) {
    if (!tcp) {
        return -1;
    }
    
    if (!tcp->reading) {
        return 0;
    }
    
    tcp->reading = false;
    tcp->read_cb = NULL;
    tcp->alloc_cb = NULL;
    
#ifdef VOX_OS_WINDOWS
    /* 在 Windows IOCP 模式下，如果有待处理的 WSARecv 操作，需要取消它 */
    if (tcp->recv_pending && tcp->socket.fd != VOX_INVALID_SOCKET) {
        /* 取消待处理的 WSARecv 操作 */
        if (CancelIoEx((HANDLE)(ULONG_PTR)tcp->socket.fd, &tcp->read_ov_ex.overlapped)) {
            VOX_LOG_DEBUG("Cancelled pending WSARecv operation for socket %d", (int)tcp->socket.fd);
        } else {
            DWORD error = GetLastError();
            if (error != ERROR_NOT_FOUND) {
                VOX_LOG_WARN("Failed to cancel WSARecv operation, error=%lu", error);
            }
        }
        tcp->recv_pending = false;
    }
#endif
    
    /* 更新 backend 事件，移除可读事件 */
    uint32_t events = tcp->backend_events & ~VOX_BACKEND_READ;
    if (events == 0) {
        /* 如果没有其他事件，从 backend 注销 */
        tcp_unregister_backend(tcp);
    } else {
        tcp_update_backend(tcp, events);
    }
    
    return 0;
}

/* 异步写入 */
int vox_tcp_write(vox_tcp_t* tcp, const void* buf, size_t len, vox_tcp_write_cb cb) {
    if (!tcp || !buf || len == 0) {
        return -1;
    }

    if (tcp->socket.fd == VOX_INVALID_SOCKET) {
        return -1;
    }

    if (!tcp->connected) {
        return -1;
    }

    vox_mpool_t* mpool = vox_loop_get_mpool(tcp->handle.loop);
    
    /* 如果有待处理的写入请求，直接加入队列 */
    if (tcp->write_queue) {
        /* 创建新的写入请求 */
        vox_tcp_write_req_t* req = (vox_tcp_write_req_t*)vox_mpool_alloc(
            mpool, sizeof(vox_tcp_write_req_t));
        if (!req) {
            return -1;
        }
        
        req->buf = buf;
        req->len = len;
        req->offset = 0;
        req->cb = cb;
        req->next = NULL;
        
        /* 添加到队列末尾 */
        vox_tcp_write_req_t* last = (vox_tcp_write_req_t*)tcp->write_queue;
        while (last->next) {
            last = last->next;
        }
        last->next = req;
        
        /* 确保可写事件已注册 */
        if (!(tcp->backend_events & VOX_BACKEND_WRITE)) {
            uint32_t events = tcp->backend_events | VOX_BACKEND_WRITE;
            if (tcp_update_backend(tcp, events) != 0) {
                /* 失败，移除刚添加的请求 */
                last->next = NULL;
                vox_mpool_free(mpool, req);
                return -1;
            }
        }
        
        return 0;
    }
    
#ifdef VOX_OS_WINDOWS
    /* Windows IOCP: 检查是否使用 IOCP backend，如果是则使用异步 WSASend */
    vox_backend_t* backend = vox_loop_get_backend(tcp->handle.loop);
    if (backend && vox_backend_get_type(backend) == VOX_BACKEND_TYPE_IOCP) {
        /* 创建写入请求 */
        vox_tcp_write_req_t* req = (vox_tcp_write_req_t*)vox_mpool_alloc(
            mpool, sizeof(vox_tcp_write_req_t));
        if (!req) {
            return -1;
        }
        
        req->buf = buf;
        req->len = len;
        req->offset = 0;
        req->cb = cb;
        req->next = NULL;
        
        /* 添加到队列 */
        if (tcp->write_queue) {
            vox_tcp_write_req_t* last = (vox_tcp_write_req_t*)tcp->write_queue;
            while (last->next) {
                last = last->next;
            }
            last->next = req;
        } else {
            tcp->write_queue = (void*)req;
        }
        
        /* 如果没有待处理的发送操作，立即启动 */
        if (!tcp->send_pending) {
            if (tcp_start_send_async(tcp, buf, len) != 0) {
                /* 失败，从队列中移除 */
                if (tcp->write_queue == (void*)req) {
                    tcp->write_queue = NULL;
                } else {
                    vox_tcp_write_req_t* prev = (vox_tcp_write_req_t*)tcp->write_queue;
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
    
    /* 非 IOCP 模式：使用传统的非阻塞 send */
    /* 尝试立即写入 */
    int64_t nwritten = vox_socket_send(&tcp->socket, buf, len);
    if (nwritten < 0) {
        /* 检查是否是 EAGAIN/EWOULDBLOCK（非阻塞 socket 缓冲区满） */
#ifdef VOX_OS_WINDOWS
        int err = WSAGetLastError();
        if (err != WSAEWOULDBLOCK) {
            //return -1;  /* 真正的错误 */
        }
#else
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
           // return -1;  /* 真正的错误 */
        }
#endif
        /* EAGAIN/EWOULDBLOCK：需要等待可写事件，加入队列 */
        nwritten = 0;  /* 没有写入任何数据，需要加入队列 */
    }
    
    if (nwritten >= 0 && (size_t)nwritten == len) {
        /* 全部写入完成 */
        if (cb) {
            cb(tcp, 0, vox_handle_get_data((vox_handle_t*)tcp));
        }
        return 0;
    }
    
    /* 部分写入或 EAGAIN，创建写入请求并加入队列 */
    vox_tcp_write_req_t* req = (vox_tcp_write_req_t*)vox_mpool_alloc(
        mpool, sizeof(vox_tcp_write_req_t));
    if (!req) {
        return -1;
    }
    
    req->buf = buf;
    req->len = len;
    req->offset = (size_t)nwritten;
    req->cb = cb;
    req->next = NULL;
    
    tcp->write_queue = (void*)req;
    
    /* 注册可写事件 */
    uint32_t events = tcp->backend_events | VOX_BACKEND_WRITE;
    if (tcp_update_backend(tcp, events) != 0) {
        VOX_LOG_ERROR("vox_tcp_write: failed to update backend");
        vox_mpool_free(mpool, req);
        tcp->write_queue = NULL;
        return -1;
    }
    
    return 0;
}

/* 关闭写入端 */
int vox_tcp_shutdown(vox_tcp_t* tcp, vox_tcp_shutdown_cb cb) {
    if (!tcp) {
        return -1;
    }
    
    if (tcp->socket.fd == VOX_INVALID_SOCKET) {
        return -1;
    }
    
    if (!tcp->connected) {
        return -1;
    }
    
    /* 设置 shutdown 回调 */
    tcp->shutdown_cb = cb;
    
    /* 执行 shutdown（关闭写入端） */
#ifdef VOX_OS_WINDOWS
    if (shutdown((SOCKET)tcp->socket.fd, SD_SEND) != 0) {
        int err = WSAGetLastError();
        if (err != WSAENOTCONN && err != WSAEINVAL) {
            if (cb) {
                cb(tcp, -1, vox_handle_get_data((vox_handle_t*)tcp));
            }
            tcp->shutdown_cb = NULL;
            return -1;
        }
    }
#else
    if (shutdown((int)tcp->socket.fd, SHUT_WR) != 0) {
        if (errno != ENOTCONN && errno != EINVAL) {
            if (cb) {
                cb(tcp, -1, vox_handle_get_data((vox_handle_t*)tcp));
            }
            tcp->shutdown_cb = NULL;
            return -1;
        }
    }
#endif
    
    /* shutdown 是同步操作，立即调用回调 */
    if (cb) {
        cb(tcp, 0, vox_handle_get_data((vox_handle_t*)tcp));
    }
    tcp->shutdown_cb = NULL;
    
    return 0;
}

/* 获取本地地址 */
int vox_tcp_getsockname(vox_tcp_t* tcp, vox_socket_addr_t* addr) {
    if (!tcp || !addr) {
        return -1;
    }
    
    if (tcp->socket.fd == VOX_INVALID_SOCKET) {
        return -1;
    }
    
    struct sockaddr_storage sa;
    socklen_t len = sizeof(sa);
    
    if (getsockname(tcp->socket.fd, (struct sockaddr*)&sa, &len) != 0) {
        return -1;
    }
    
    /* 转换地址格式 */
    if (sa.ss_family == AF_INET) {
        struct sockaddr_in* sin = (struct sockaddr_in*)&sa;
        addr->family = VOX_AF_INET;
        addr->u.ipv4.addr = sin->sin_addr.s_addr;
        addr->u.ipv4.port = sin->sin_port;
    } else if (sa.ss_family == AF_INET6) {
        struct sockaddr_in6* sin6 = (struct sockaddr_in6*)&sa;
        addr->family = VOX_AF_INET6;
        memcpy(addr->u.ipv6.addr, &sin6->sin6_addr, 16);
        addr->u.ipv6.port = sin6->sin6_port;
    } else {
        return -1;
    }
    
    return 0;
}

/* 获取对端地址 */
int vox_tcp_getpeername(vox_tcp_t* tcp, vox_socket_addr_t* addr) {
    if (!tcp || !addr) {
        return -1;
    }
    
    if (tcp->socket.fd == VOX_INVALID_SOCKET) {
        return -1;
    }
    
    struct sockaddr_storage sa;
    socklen_t len = sizeof(sa);
    
    if (getpeername(tcp->socket.fd, (struct sockaddr*)&sa, &len) != 0) {
        return -1;
    }
    
    /* 转换地址格式 */
    if (sa.ss_family == AF_INET) {
        struct sockaddr_in* sin = (struct sockaddr_in*)&sa;
        addr->family = VOX_AF_INET;
        addr->u.ipv4.addr = sin->sin_addr.s_addr;
        addr->u.ipv4.port = sin->sin_port;
    } else if (sa.ss_family == AF_INET6) {
        struct sockaddr_in6* sin6 = (struct sockaddr_in6*)&sa;
        addr->family = VOX_AF_INET6;
        memcpy(addr->u.ipv6.addr, &sin6->sin6_addr, 16);
        addr->u.ipv6.port = sin6->sin6_port;
    } else {
        return -1;
    }
    
    return 0;
}

/* 设置 TCP 无延迟 */
int vox_tcp_nodelay(vox_tcp_t* tcp, bool enable) {
    if (!tcp) {
        return -1;
    }
    
    if (tcp->socket.fd == VOX_INVALID_SOCKET) {
        return -1;
    }
    
    return vox_socket_set_tcp_nodelay(&tcp->socket, enable);
}

/* 设置保持连接 */
int vox_tcp_keepalive(vox_tcp_t* tcp, bool enable) {
    if (!tcp) {
        return -1;
    }
    
    if (tcp->socket.fd == VOX_INVALID_SOCKET) {
        return -1;
    }
    
    return vox_socket_set_keepalive(&tcp->socket, enable);
}

/* 设置地址重用 */
int vox_tcp_reuseaddr(vox_tcp_t* tcp, bool enable) {
    if (!tcp) {
        return -1;
    }
    
    if (tcp->socket.fd == VOX_INVALID_SOCKET) {
        return -1;
    }
    
    return vox_socket_set_reuseaddr(&tcp->socket, enable);
}

#ifdef VOX_OS_WINDOWS
/* 启动异步 AcceptEx 操作 */
static int tcp_start_accept_async(vox_tcp_t* server) {
    if (!server || !server->listening) {
        return -1;
    }

    SOCKET listen_sock = (SOCKET)server->socket.fd;
    vox_mpool_t* mpool = vox_loop_get_mpool(server->handle.loop);

    /* 如果操作池未分配，分配操作池 */
    if (!server->accept_pool) {
        server->accept_pool = (vox_tcp_accept_ctx_t*)vox_mpool_alloc(
            mpool, sizeof(vox_tcp_accept_ctx_t) * VOX_TCP_ACCEPT_POOL_SIZE);
        if (!server->accept_pool) {
            VOX_LOG_ERROR("Failed to allocate accept pool");
            return -1;
        }

        /* 初始化操作池中的每个上下文 */
        size_t addr_len = (server->socket.family == VOX_AF_INET) ?
                          sizeof(struct sockaddr_in) : sizeof(struct sockaddr_in6);
        size_t buffer_size = 2 * (addr_len + 16);

        for (int i = 0; i < VOX_TCP_ACCEPT_POOL_SIZE; i++) {
            vox_tcp_accept_ctx_t* ctx = &server->accept_pool[i];
            memset(ctx, 0, sizeof(vox_tcp_accept_ctx_t));

            ctx->ov_ex.io_type = VOX_TCP_IO_ACCEPT;
            ctx->ov_ex.tcp = server;
            ctx->socket = INVALID_SOCKET;
            ctx->buffer_size = buffer_size;
            ctx->buffer = vox_mpool_alloc(mpool, buffer_size);
            ctx->pending = false;
            ctx->index = i;

            if (!ctx->buffer) {
                VOX_LOG_ERROR("Failed to allocate accept buffer for context %d", i);
                /* 清理已分配的资源 */
                for (int j = 0; j < i; j++) {
                    if (server->accept_pool[j].buffer) {
                        vox_mpool_free(mpool, server->accept_pool[j].buffer);
                    }
                }
                vox_mpool_free(mpool, server->accept_pool);
                server->accept_pool = NULL;
                return -1;
            }
        }

        server->accept_pool_size = VOX_TCP_ACCEPT_POOL_SIZE;
        server->accept_pending_count = 0;
    }

    /* 获取 AcceptEx 函数指针 */
    LPFN_ACCEPTEX fnAcceptEx = get_acceptex_function(listen_sock);
    if (!fnAcceptEx) {
        VOX_LOG_ERROR("Failed to get AcceptEx function pointer");
        return -1;
    }

    /* 获取 IOCP 实例和 listen_key */
    vox_backend_t* backend = vox_loop_get_backend(server->handle.loop);
    if (!backend || vox_backend_get_type(backend) != VOX_BACKEND_TYPE_IOCP) {
        VOX_LOG_ERROR("Not an IOCP backend");
        return -1;
    }

    vox_iocp_t* iocp = (vox_iocp_t*)vox_backend_get_iocp_impl(backend);
    if (!iocp) {
        VOX_LOG_ERROR("Failed to get IOCP instance");
        return -1;
    }

    int listen_fd = (int)listen_sock;
    ULONG_PTR listen_key = vox_iocp_get_completion_key(iocp, listen_fd);
    if (listen_key == 0) {
        VOX_LOG_ERROR("listen_socket has no completion key");
        return -1;
    }

    /* 遍历操作池，为每个空闲的上下文发起 AcceptEx 操作 */
    int started = 0;
    size_t addr_len = (server->socket.family == VOX_AF_INET) ?
                      sizeof(struct sockaddr_in) : sizeof(struct sockaddr_in6);
    int domain = (server->socket.family == VOX_AF_INET) ? AF_INET : AF_INET6;

    for (int i = 0; i < server->accept_pool_size; i++) {
        vox_tcp_accept_ctx_t* ctx = &server->accept_pool[i];

        /* 跳过已经有待处理操作的上下文 */
        if (ctx->pending) {
            continue;
        }

        /* 创建 accept socket */
        SOCKET accept_sock = WSASocket(domain, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED);
        if (accept_sock == INVALID_SOCKET) {
            DWORD error = WSAGetLastError();
            VOX_LOG_ERROR("WSASocket failed for context %d, error=%lu", i, error);
            continue;
        }

        /* 设置非阻塞 */
        u_long mode = 1;
        ioctlsocket(accept_sock, FIONBIO, &mode);

        /* 将 accept_socket 关联到 IOCP */
        if (vox_iocp_associate_socket(iocp, (int)accept_sock, listen_key) != 0) {
            DWORD error = WSAGetLastError();
            VOX_LOG_ERROR("Failed to associate accept_socket for context %d, error=%lu", i, error);
            closesocket(accept_sock);
            continue;
        }

        /* 保存 socket */
        ctx->socket = accept_sock;

        /* 重置 OVERLAPPED 结构 */
        memset(&ctx->ov_ex.overlapped, 0, sizeof(OVERLAPPED));

        /* 启动 AcceptEx */
        DWORD bytes_received = 0;
        BOOL result = fnAcceptEx(
            listen_sock,
            accept_sock,
            ctx->buffer,
            0,  /* 不接收额外数据 */
            (DWORD)(addr_len + 16),
            (DWORD)(addr_len + 16),
            &bytes_received,
            &ctx->ov_ex.overlapped
        );

        if (result == FALSE) {
            DWORD error = WSAGetLastError();
            if (error != ERROR_IO_PENDING) {
                VOX_LOG_ERROR("AcceptEx failed for context %d, error=%lu", i, error);
                closesocket(accept_sock);
                ctx->socket = INVALID_SOCKET;
                continue;
            }
        }

        /* 标记为待处理 */
        ctx->pending = true;
        server->accept_pending_count++;
        started++;
    }

    return started > 0 ? 0 : -1;
}

/* 启动异步 WSARecv 操作 */
static int tcp_start_recv_async(vox_tcp_t* tcp) {
    if (!tcp || !tcp->reading) {
        return -1;
    }
    
    if (tcp->recv_pending) {
        return 0;  /* 已经有待处理的 WSARecv 操作 */
    }
    
    SOCKET sock = (SOCKET)tcp->socket.fd;
    
    /* 在 IOCP 模式下，对于通过 AcceptEx 接受的 socket，需要先更新其接受上下文 */
#ifdef VOX_OS_WINDOWS
    vox_backend_t* backend = vox_loop_get_backend(tcp->handle.loop);
    if (backend && vox_backend_get_type(backend) == VOX_BACKEND_TYPE_IOCP) {
        /* 对于新接受的 socket，需要更新上下文 */
        /* 如果这个 socket 是通过 AcceptEx 接受的，则应该已经有正确的上下文 */
        /* 但为了安全起见，我们可以尝试更新上下文 */
        /* 对于客户端连接，我们不需要更新上下文，但对于服务器接受的 socket 可能需要 */
        
        /* 在 AcceptEx 后，socket 应该已经配置好，但我们仍然尝试更新上下文 */
        /* 这里我们尝试更新，但如果失败也不认为是致命错误 */
        /* 但要注意，SO_UPDATE_ACCEPT_CONTEXT 需要监听 socket 的句柄 */
        
        /* 如果我们知道这个 socket 是服务器接受的，我们需要获取监听 socket */
        /* 由于 AcceptEx 已经完成，我们可能需要在服务器端保持监听 socket 的引用 */
        /* 对于现在，我们跳过更新，因为已经在 vox_tcp_accept 中更新了 */
    }
#endif
    
    /* 分配或获取接收缓冲区 */
    void* buf = NULL;
    size_t len = 0;
    
    if (tcp->alloc_cb) {
        tcp->alloc_cb(tcp, VOX_TCP_DEFAULT_READ_BUF_SIZE, &buf, &len, 
                      vox_handle_get_data((vox_handle_t*)tcp));
    } else {
        /* 使用默认缓冲区 */
        vox_mpool_t* mpool = vox_loop_get_mpool(tcp->handle.loop);
        if (!tcp->read_buf || tcp->read_buf_size < VOX_TCP_DEFAULT_READ_BUF_SIZE) {
            if (tcp->read_buf) {
                vox_mpool_free(mpool, tcp->read_buf);
            }
            tcp->read_buf = vox_mpool_alloc(mpool, VOX_TCP_DEFAULT_READ_BUF_SIZE);
            if (tcp->read_buf) {
                tcp->read_buf_size = VOX_TCP_DEFAULT_READ_BUF_SIZE;
            }
        }
        buf = tcp->read_buf;
        len = tcp->read_buf_size;
    }
    
    if (!buf || len == 0) {
        return -1;
    }
    
    /* 准备 WSABUF 数组 */
    if (!tcp->recv_bufs || tcp->recv_buf_count == 0) {
        vox_mpool_t* mpool = vox_loop_get_mpool(tcp->handle.loop);
        tcp->recv_bufs = (WSABUF*)vox_mpool_alloc(mpool, sizeof(WSABUF));
        if (!tcp->recv_bufs) {
            return -1;
        }
        tcp->recv_buf_count = 1;
    }
    
    tcp->recv_bufs[0].buf = (CHAR*)buf;
    tcp->recv_bufs[0].len = (ULONG)len;
    tcp->recv_flags = 0;

    /* 设置扩展 OVERLAPPED 结构 */
    memset(&tcp->read_ov_ex.overlapped, 0, sizeof(OVERLAPPED));
    tcp->read_ov_ex.io_type = VOX_TCP_IO_RECV;
    tcp->read_ov_ex.tcp = tcp;
    
    /* 验证 socket 状态 */
    if (sock == INVALID_SOCKET) {
        return -1;
    }
    
    /* 验证缓冲区 */
    if (!tcp->recv_bufs || tcp->recv_buf_count == 0 || !tcp->recv_bufs[0].buf || tcp->recv_bufs[0].len == 0) {
        return -1;
    }
    
    /* 启动 WSARecv */
    /* 注意：在调用 WSARecv 之前，确保 socket 已经正确关联到 IOCP
     * 对于通过 AcceptEx 接受的 socket，它已经通过 AcceptEx 关联到 IOCP（使用 listen_key）
     * 但是，为了确保 WSARecv 正常工作，我们需要确保 socket 的 completion key 是正确的
     * 
     * 实际上，socket 的 completion key 在关联到 IOCP 时就固定了，无法更改
     * 所以，WSARecv 完成事件会使用 socket 的 completion key（可能是 listen_key）
     * 我们需要在事件处理中正确处理这种情况
     */
    
    /* 验证 socket 是否已关联到 IOCP */
    vox_backend_t* backend_check = vox_loop_get_backend(tcp->handle.loop);
    if (backend_check && vox_backend_get_type(backend_check) == VOX_BACKEND_TYPE_IOCP) {
        vox_iocp_t* iocp_check = (vox_iocp_t*)vox_backend_get_iocp_impl(backend_check);
        if (iocp_check) {
            ULONG_PTR completion_key = vox_iocp_get_completion_key(iocp_check, (int)sock);
            if (completion_key == 0) {
                /* Socket 没有 completion key，尝试关联到 IOCP */
                /* 注意：如果 socket 已经通过 AcceptEx 关联到 IOCP，这里会失败 */
                /* 但我们可以尝试，如果失败就继续 */
                vox_tcp_internal_data_t* data = (vox_tcp_internal_data_t*)vox_mpool_alloc(
                    vox_loop_get_mpool(tcp->handle.loop), sizeof(vox_tcp_internal_data_t));
                if (data) {
                    data->tcp = tcp;
                    data->user_data = vox_handle_get_data((vox_handle_t*)tcp);
                    if (vox_backend_add(backend_check, (int)sock, VOX_BACKEND_READ, data) == 0) {
                        /* Socket 已关联到 IOCP */
                    } else {
                        /* 关联失败，可能是 socket 已经关联到 IOCP（通过 AcceptEx） */
                        /* 这是可以接受的，因为事件会通过 OVERLAPPED 指针路由 */
                        vox_mpool_free(vox_loop_get_mpool(tcp->handle.loop), data);
                    }
                }
            } else {
                /* Socket 已经有 completion key，可能是通过 AcceptEx 关联的 */
                /* 这是可以接受的，因为事件会通过 OVERLAPPED 指针路由到正确的 TCP 句柄 */
            }
        }
    }
    
    /* 验证 socket 状态：检查是否已连接 */
    /* 注意：对于客户端 socket，在连接完成之前 getpeername 可能失败，这是正常的 */
    /* 对于服务器接受的 socket，在 AcceptEx 完成后应该已经连接 */
    struct sockaddr_storage peer_addr;
    socklen_t peer_len = sizeof(peer_addr);
    if (getpeername(sock, (struct sockaddr*)&peer_addr, &peer_len) != 0) {
        /* 这可能表示 socket 还没有完全准备好（客户端连接中） */
        /* 对于客户端，这是正常的，继续尝试 WSARecv */
        /* 对于服务器接受的 socket，如果失败可能是问题，但不一定是致命错误 */
        DWORD error = WSAGetLastError();
        if (error != WSAENOTCONN) {
            /* 如果不是 "未连接" 错误，记录日志 */
            VOX_LOG_ERROR("getpeername failed before WSARecv, error=%lu", error);
        }
        /* 继续尝试 WSARecv，如果 socket 确实没有准备好，WSARecv 会返回错误 */
    }
    
    int result = WSARecv(
        sock,
        tcp->recv_bufs,
        tcp->recv_buf_count,
        NULL,  /* 异步操作，使用NULL */
        &tcp->recv_flags,
        &tcp->read_ov_ex.overlapped,
        NULL
    );
    
    if (result == SOCKET_ERROR) {
        DWORD error = WSAGetLastError();
        if (error != WSA_IO_PENDING) {
            /* 错误 10054 (WSAECONNRESET) 表示连接被重置，可能是客户端关闭了连接 */
            /* 错误 10053 (WSAECONNABORTED) 表示软件导致连接中止 */
            /* 错误 10057 (WSAENOTCONN) 表示 socket 未连接 */
            if (error == WSAECONNRESET || error == WSAECONNABORTED || error == WSAENOTCONN) {
                /* 连接被重置、中止或未连接，这是可以接受的（客户端可能关闭了连接） */
                VOX_LOG_DEBUG("WSARecv failed, connection reset/abort or not connected, error=%lu", error);
            } else {
                VOX_LOG_ERROR("WSARecv failed, error=%lu", error);
            }
            return -1;
        }
    }
    
    tcp->recv_pending = true;
    return 0;
}

/* 启动异步 WSASend 操作 */
static int tcp_start_send_async(vox_tcp_t* tcp, const void* buf, size_t len) {
    if (!tcp || !buf || len == 0) {
        return -1;
    }

    /* 检查连接状态 */
    if (tcp->socket.fd == VOX_INVALID_SOCKET || !tcp->connected) {
        return -1;
    }

    if (tcp->send_pending) {
        return -1;  /* 已经有待处理的 WSASend 操作，应该加入队列 */
    }

    SOCKET sock = (SOCKET)tcp->socket.fd;
    
    /* 准备 WSABUF 数组 */
    if (!tcp->send_bufs || tcp->send_buf_count == 0) {
        vox_mpool_t* mpool = vox_loop_get_mpool(tcp->handle.loop);
        tcp->send_bufs = (WSABUF*)vox_mpool_alloc(mpool, sizeof(WSABUF));
        if (!tcp->send_bufs) {
            return -1;
        }
        tcp->send_buf_count = 1;
    }
    
    /* 注意：buf 必须是持久的，不能是栈上的临时缓冲区 */
    tcp->send_bufs[0].buf = (CHAR*)buf;
    tcp->send_bufs[0].len = (ULONG)len;
    
    /* 设置扩展 OVERLAPPED 结构 */
    memset(&tcp->write_ov_ex.overlapped, 0, sizeof(OVERLAPPED));
    tcp->write_ov_ex.io_type = VOX_TCP_IO_SEND;
    tcp->write_ov_ex.tcp = tcp;
    
    /* 启动 WSASend */
    /* 注意：对于异步操作，lpNumberOfBytesSent 应该为 NULL，实际传输的字节数通过 GetOverlappedResult 获取 */
    int result = WSASend(
        sock,
        tcp->send_bufs,
        tcp->send_buf_count,
        NULL,  /* 异步操作，使用 NULL */
        0,
        &tcp->write_ov_ex.overlapped,
        NULL
    );
    
    if (result == SOCKET_ERROR) {
        DWORD error = WSAGetLastError();
        if (error != WSA_IO_PENDING) {
            /* 错误 10054 (WSAECONNRESET) 表示连接被重置 */
            /* 错误 10053 (WSAECONNABORTED) 表示软件导致连接中止 */
            /* 错误 10057 (WSAENOTCONN) 表示 socket 未连接 */
            if (error == WSAECONNRESET || error == WSAECONNABORTED || error == WSAENOTCONN) {
                VOX_LOG_WARN("WSASend failed, connection reset/abort or not connected, error=%lu", error);
            } else {
                VOX_LOG_ERROR("WSASend failed, error=%lu", error);
            }
            return -1;
        }
    }

    tcp->send_pending = true;
    return 0;
}

/* 启动异步 ConnectEx 操作 */
static int tcp_start_connect_async(vox_tcp_t* tcp, const vox_socket_addr_t* addr) {
    if (!tcp || !addr) {
        return -1;
    }
    
    if (tcp->connect_pending) {
        return -1;
    }
    
    SOCKET sock = (SOCKET)tcp->socket.fd;
    LPFN_CONNECTEX fnConnectEx = get_connectex_function(sock);
    if (!fnConnectEx) {
        return -1;
    }
    
    /* 转换地址 */
    struct sockaddr_storage sa;
    socklen_t sa_len;
    
    if (addr->family == VOX_AF_INET) {
        struct sockaddr_in* sin = (struct sockaddr_in*)&sa;
        memset(sin, 0, sizeof(*sin));
        sin->sin_family = AF_INET;
        sin->sin_addr.s_addr = addr->u.ipv4.addr;
        sin->sin_port = addr->u.ipv4.port;
        sa_len = sizeof(*sin);
    } else {
        struct sockaddr_in6* sin6 = (struct sockaddr_in6*)&sa;
        memset(sin6, 0, sizeof(*sin6));
        sin6->sin6_family = AF_INET6;
        memcpy(&sin6->sin6_addr, addr->u.ipv6.addr, 16);
        sin6->sin6_port = addr->u.ipv6.port;
        sa_len = sizeof(*sin6);
    }
    
    /* 设置扩展 OVERLAPPED 结构 */
    memset(&tcp->connect_ov_ex.overlapped, 0, sizeof(OVERLAPPED));
    tcp->connect_ov_ex.io_type = VOX_TCP_IO_CONNECT;
    tcp->connect_ov_ex.tcp = tcp;
    
    /* 启动 ConnectEx */
    /* 注意：lpBytesSent 参数是可选的，如果不需要发送数据，可以传 NULL */
    BOOL result = fnConnectEx(
        sock,
        (struct sockaddr*)&sa,
        (int)sa_len,
        NULL,  /* 可选：发送缓冲区 */
        0,     /* 发送数据长度 */
        NULL,  /* 异步操作，使用 NULL */
        &tcp->connect_ov_ex.overlapped
    );
    
    if (result == FALSE) {
        DWORD error = WSAGetLastError();
        if (error != ERROR_IO_PENDING) {
            return -1;
        }
    }
    
    tcp->connect_pending = true;
    return 0;
}
#endif
