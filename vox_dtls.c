/*
 * vox_dtls.c - DTLS 异步操作实现
 * 基于 vox_udp 实现跨平台 DTLS
 */

#include "vox_dtls.h"
#include "vox_loop.h"
#include "vox_backend.h"
#include "vox_mpool.h"
#include "vox_log.h"
#include "vox_socket.h"
#include <string.h>

/* 默认读取缓冲区大小 */
#define VOX_DTLS_DEFAULT_READ_BUF_SIZE 4096
#define VOX_DTLS_DEFAULT_BIO_BUF_SIZE 16384  /* BIO 缓冲区大小 */

/* DTLS 写入请求结构 */
typedef struct vox_dtls_write_req {
    void* buf;                         /* 数据缓冲区（我们拥有的副本） */
    size_t len;                        /* 总长度 */
    size_t offset;                     /* 已写入的偏移量 */
    vox_socket_addr_t addr;            /* 目标地址 */
    vox_dtls_write_cb cb;               /* 写入完成回调 */
    struct vox_dtls_write_req* next;    /* 下一个请求（链表） */
} vox_dtls_write_req_t;

/* DTLS 句柄内部数据 */
typedef struct {
    vox_dtls_t* dtls;
    void* user_data;
} vox_dtls_internal_data_t;

/* 前向声明 */
static void dtls_process_write_queue(vox_dtls_t* dtls);
static int dtls_process_rbio_data(vox_dtls_t* dtls);
static int dtls_process_wbio_data(vox_dtls_t* dtls);
static void dtls_udp_recv_cb(vox_udp_t* udp, ssize_t nread, const void* buf, 
                             const vox_socket_addr_t* addr, unsigned int flags, void* user_data);
static void dtls_udp_send_cb(vox_udp_t* udp, int status, void* user_data);

/* 处理 rbio 数据：从 socket 读取后写入 rbio，然后尝试 SSL 操作 */
static int dtls_process_rbio_data(vox_dtls_t* dtls) {
    if (!dtls || !dtls->ssl_session || !dtls->udp) {
        return -1;
    }

    /* 如果是服务器端且未连接也未在握手，自动开始握手 */
    if (!dtls->handshaking && !dtls->dtls_connected && dtls->listening) {
        dtls->handshaking = true;
        //VOX_LOG_WARN("DTLS server: auto-starting handshake");
        /* 注意：handshake_cb 可能还没有设置，这会在 connection_cb 中设置 */
    }

    /* 如果正在握手，尝试继续握手 */
    if (dtls->handshaking) {
        int ret = vox_ssl_session_handshake(dtls->ssl_session);
        if (ret == 0) {
            /* 握手成功 */
            dtls->handshaking = false;
            dtls->dtls_connected = true;
            if (dtls->handshake_cb) {
                vox_dtls_handshake_cb saved_cb = dtls->handshake_cb;
                void* saved_user_data = vox_handle_get_data((vox_handle_t*)dtls);
                dtls->handshake_cb = NULL;
                if (saved_cb) {
                    saved_cb(dtls, 0, saved_user_data);
                }
            }
            /* 握手完成后，处理 wbio 数据和写队列 */
            dtls_process_wbio_data(dtls);
            dtls_process_write_queue(dtls);
        } else if (ret == VOX_SSL_ERROR_WANT_READ || ret == VOX_SSL_ERROR_WANT_WRITE) {
            /* 握手需要更多数据，这是正常的 */
            dtls_process_wbio_data(dtls);
            dtls_process_write_queue(dtls);
        } else {
            /* 握手返回错误 */
            dtls->handshaking = false;
            dtls->dtls_connected = false;
            if (dtls->handshake_cb) {
                vox_dtls_handshake_cb saved_cb = dtls->handshake_cb;
                void* saved_user_data = vox_handle_get_data((vox_handle_t*)dtls);
                dtls->handshake_cb = NULL;
                if (saved_cb) {
                    saved_cb(dtls, -1, saved_user_data);
                }
            }
        }
    }

    /* 检查是否正在关闭并尝试完成关闭 */
    if (dtls->shutting_down && dtls->shutdown_cb) {
        int ret = vox_ssl_session_shutdown(dtls->ssl_session);
        if (ret == 0) {
            /* 关闭完成 */
            dtls->shutting_down = false;
            vox_dtls_shutdown_cb saved_cb = dtls->shutdown_cb;
            void* saved_user_data = vox_handle_get_data((vox_handle_t*)dtls);
            dtls->shutdown_cb = NULL;
            if (saved_cb) {
                saved_cb(dtls, 0, saved_user_data);
            }
        } else if (ret != VOX_SSL_ERROR_WANT_READ && ret != VOX_SSL_ERROR_WANT_WRITE) {
            /* 关闭失败 */
            dtls->shutting_down = false;
            vox_dtls_shutdown_cb saved_cb = dtls->shutdown_cb;
            void* saved_user_data = vox_handle_get_data((vox_handle_t*)dtls);
            dtls->shutdown_cb = NULL;
            if (saved_cb) {
                saved_cb(dtls, -1, saved_user_data);
            }
        }
        /* 处理 wbio 数据 */
        dtls_process_wbio_data(dtls);
    }

    /* 如果握手完成且是客户端，调用 connect_cb */
    if (dtls->dtls_connected && dtls->connect_cb && !dtls->listening) {
        vox_dtls_connect_cb saved_cb = dtls->connect_cb;
        void* saved_user_data = vox_handle_get_data((vox_handle_t*)dtls);
        dtls->connect_cb = NULL;
        if (saved_cb) {
            saved_cb(dtls, 0, saved_user_data);
        }
    }

    /* 如果已连接，尝试读取解密后的数据 */
    if (dtls->dtls_connected && dtls->reading && dtls->read_cb) {
        /* 循环读取，直到没有更多数据 */
        /* 限制循环次数，避免无限循环 */
        int max_iterations = 100;
        while (max_iterations-- > 0) {
            /* 分配读取缓冲区 */
            void* buf = NULL;
            size_t len = 0;

            if (dtls->alloc_cb) {
                dtls->alloc_cb(dtls, VOX_DTLS_DEFAULT_READ_BUF_SIZE, &buf, &len,
                             vox_handle_get_data((vox_handle_t*)dtls));
            } else {
                /* 使用默认缓冲区 */
                vox_mpool_t* mpool = vox_loop_get_mpool(dtls->handle.loop);
                if (!dtls->read_buf || dtls->read_buf_size < VOX_DTLS_DEFAULT_READ_BUF_SIZE) {
                    if (dtls->read_buf) {
                        vox_mpool_free(mpool, dtls->read_buf);
                    }
                    dtls->read_buf = vox_mpool_alloc(mpool, VOX_DTLS_DEFAULT_READ_BUF_SIZE);
                    if (!dtls->read_buf) {
                        VOX_LOG_ERROR("Failed to allocate read buffer");
                        break;
                    }
                    dtls->read_buf_size = VOX_DTLS_DEFAULT_READ_BUF_SIZE;
                }
                buf = dtls->read_buf;
                len = dtls->read_buf_size;
            }
            
            if (!buf || len == 0) {
                break;
            }

            /* 尝试读取解密后的数据 */
            ssize_t nread = vox_ssl_session_read(dtls->ssl_session, buf, len);

            if (nread > 0) {
                /* 成功读取数据 */
                /* 注意：回调可能会修改 DTLS 状态（例如停止读取），所以先保存状态 */
                bool still_reading = dtls->reading;
                void* saved_user_data = vox_handle_get_data((vox_handle_t*)dtls);
                const vox_socket_addr_t* addr = dtls->peer_addr_set ? &dtls->peer_addr : NULL;
                dtls->read_cb(dtls, nread, buf, addr, saved_user_data);
                /* 如果回调后不再读取，退出循环 */
                if (!still_reading || !dtls->reading) {
                    break;
                }
                /* 检查 rbio 中是否还有更多数据 */
                /* 如果没有更多数据，退出循环，等待更多 UDP 数据 */
                size_t rbio_pending = vox_ssl_bio_pending(dtls->ssl_session, VOX_SSL_BIO_RBIO);
                if (rbio_pending == 0) {
                    /* 没有更多数据，退出循环，等待更多 UDP 数据 */
                    break;
                }
                /* 继续循环，尝试读取更多数据 */
            } else if (nread == 0) {
                /* 连接关闭 */
                const vox_socket_addr_t* addr = dtls->peer_addr_set ? &dtls->peer_addr : NULL;
                dtls->read_cb(dtls, 0, NULL, addr, vox_handle_get_data((vox_handle_t*)dtls));
                vox_dtls_read_stop(dtls);
                break;
            } else if (nread == VOX_SSL_ERROR_WANT_READ) {
                /* 需要更多数据，退出循环，等待更多 UDP 数据 */
                break;
            } else if (nread == VOX_SSL_ERROR_WANT_WRITE) {
                /* 需要写入，处理 wbio 数据 */
                dtls_process_wbio_data(dtls);
                /* 退出循环，避免无限循环，等待写操作完成后再继续 */
                break;
            } else {
                /* 读取错误 */
                const vox_socket_addr_t* addr = dtls->peer_addr_set ? &dtls->peer_addr : NULL;
                dtls->read_cb(dtls, -1, NULL, addr, vox_handle_get_data((vox_handle_t*)dtls));
                break;
            }
        }
    }

    /* 只有在未处于握手状态时才处理 wbio 数据，避免在握手期间递归调用握手 */
    if (!dtls->handshaking) {
        dtls_process_wbio_data(dtls);
    }

    return 0;
}

/* 处理 wbio 数据：从 wbio 读取加密数据，写入 socket */
static int dtls_process_wbio_data(vox_dtls_t* dtls) {
    if (!dtls || !dtls->ssl_session || !dtls->udp) {
        return -1;
    }

    /* 循环处理，直到 wbio 中没有数据，带安全限制 */
    int max_iterations = 100;
    while (max_iterations-- > 0) {
        /* 检查 wbio 中是否有数据 */
        size_t pending = vox_ssl_bio_pending(dtls->ssl_session, VOX_SSL_BIO_WBIO);
        if (pending == 0) {
            return 0;  /* 没有数据需要写入 */
        }
        
        /* 调试：记录 wbio 中有数据需要发送 */
        //if (dtls->handshaking) {
        //    VOX_LOG_WARN("DTLS handshaking: wbio has %zu bytes to send", pending);
        //}

        /* 分配缓冲区 */
        vox_mpool_t* mpool = vox_loop_get_mpool(dtls->handle.loop);
        if (!dtls->wbio_buf || dtls->wbio_buf_size < pending) {
            if (dtls->wbio_buf) {
                vox_mpool_free(mpool, dtls->wbio_buf);
            }
            dtls->wbio_buf = vox_mpool_alloc(mpool, pending);
            if (!dtls->wbio_buf) {
                VOX_LOG_ERROR("Failed to allocate wbio buffer");
                return -1;
            }
            dtls->wbio_buf_size = pending;
        }

        /* 从 wbio 读取数据 */
        ssize_t nread = vox_ssl_bio_read(dtls->ssl_session, VOX_SSL_BIO_WBIO, dtls->wbio_buf, pending);
        if (nread <= 0) {
            /* 没有数据或错误，退出循环 */
            if (dtls->handshaking) {
                VOX_LOG_WARN("DTLS handshaking: wbio read returned %zd", nread);
            }
            return 0;
        }
        
        /* 调试：记录准备发送的数据 */
        //if (dtls->handshaking) {
        //    VOX_LOG_WARN("DTLS handshaking: preparing to send %zd bytes", nread);
        //}

        /* 写入 UDP socket */
        const vox_socket_addr_t* addr = dtls->peer_addr_set ? &dtls->peer_addr : NULL;
        if (!addr) {
            VOX_LOG_ERROR("No peer address set for DTLS write");
            return -1;
        }
        
        /* 在 Linux 下，确保 peer_addr 已正确设置 */
        if (dtls->listening && !dtls->peer_addr_set) {
            VOX_LOG_ERROR("Server DTLS: peer_addr not set before sending handshake message");
            return -1;
        }
        
        /* 调试：记录发送的目标地址 */
        if (dtls->handshaking) {
            char addr_str[64];
            if (vox_socket_address_to_string(addr, addr_str, sizeof(addr_str)) == 0) {
                uint16_t port = vox_socket_get_port(addr);
                VOX_LOG_WARN("DTLS handshaking: sending %zd bytes to %s:%d", nread, addr_str, port);
            }
        }
        
        int write_result = vox_udp_send(dtls->udp, dtls->wbio_buf, (size_t)nread, addr, dtls_udp_send_cb);
        if (write_result != 0) {
            /* 写入失败，可能是 socket 缓冲区满，等待可写事件 */
            VOX_LOG_ERROR("Failed to write to UDP socket, pending=%zu, nread=%zd, result=%d", pending, nread, write_result);
            /* 不返回错误，等待可写事件后继续处理 */
            return 0;
        }
        
        /* 调试：记录成功发送的数据 */
        //if (dtls->handshaking) {
        //    VOX_LOG_WARN("DTLS handshaking: successfully queued %zd bytes for sending", nread);
        //}

        /* 继续检查是否还有数据需要写入 */
    }

    if (max_iterations <= 0) {
        VOX_LOG_WARN("wbio processing reached iteration limit, possible SSL layer issue");
    }

    return 0;
}

/* UDP 读取回调：从 socket 读取数据后，写入 rbio，然后处理 SSL 操作 */
static void dtls_udp_recv_cb(vox_udp_t* udp, ssize_t nread, const void* buf, 
                             const vox_socket_addr_t* addr, unsigned int flags, void* user_data) {
    (void)udp;
    (void)flags;
    vox_dtls_t* dtls = (vox_dtls_t*)user_data;
    if (!dtls) {
        return;
    }
    
    if (nread < 0) {
        /* 读取错误 */
        VOX_LOG_WARN("DTLS UDP recv error: nread=%zd", nread);
        if (dtls->read_cb) {
            const vox_socket_addr_t* peer_addr = dtls->peer_addr_set ? &dtls->peer_addr : NULL;
            dtls->read_cb(dtls, -1, NULL, peer_addr, vox_handle_get_data((vox_handle_t*)dtls));
        }
        return;
    }

    if (nread == 0) {
        /* 连接关闭 */
        if (dtls->handshaking) {
            /* 握手过程中连接关闭，握手失败 */
            dtls->handshaking = false;
            if (dtls->handshake_cb) {
                vox_dtls_handshake_cb saved_cb = dtls->handshake_cb;
                void* saved_user_data = vox_handle_get_data((vox_handle_t*)dtls);
                dtls->handshake_cb = NULL;
                if (saved_cb) {
                    saved_cb(dtls, -1, saved_user_data);
                }
            }
        }
        if (dtls->read_cb) {
            const vox_socket_addr_t* peer_addr = dtls->peer_addr_set ? &dtls->peer_addr : NULL;
            dtls->read_cb(dtls, 0, NULL, peer_addr, vox_handle_get_data((vox_handle_t*)dtls));
        }
        return;
    }

    /* 如果是对端地址，检查是否是新的客户端 */
    bool is_new_client = false;
    if (addr) {
        if (dtls->peer_addr_set) {
            /* 检查地址是否相同 */
            bool addr_match = false;
            if (dtls->peer_addr.family == addr->family) {
                if (addr->family == VOX_AF_INET) {
                    addr_match = (dtls->peer_addr.u.ipv4.addr == addr->u.ipv4.addr &&
                                  dtls->peer_addr.u.ipv4.port == addr->u.ipv4.port);
                } else if (addr->family == VOX_AF_INET6) {
                    addr_match = (memcmp(dtls->peer_addr.u.ipv6.addr, addr->u.ipv6.addr, 16) == 0 &&
                                  dtls->peer_addr.u.ipv6.port == addr->u.ipv6.port);
                }
            }
            
            /* 如果是不同的客户端地址，需要重置状态 */
            if (!addr_match && dtls->listening) {
                /* 销毁旧的 SSL Session */
                if (dtls->ssl_session) {
                    vox_ssl_session_destroy(dtls->ssl_session);
                    dtls->ssl_session = NULL;
                }
                /* 重置状态 */
                dtls->dtls_connected = false;
                dtls->handshaking = false;
                dtls->reading = false;
                dtls->handshake_cb = NULL;
                dtls->read_cb = NULL;
                dtls->alloc_cb = NULL;
                is_new_client = true;
            }
        } else {
            is_new_client = true;
        }
        
        /* 更新对端地址 */
        dtls->peer_addr = *addr;
        dtls->peer_addr_set = true;
    }

    /* 如果是服务器端且还没有 SSL Session，创建它 */
    bool is_new_server_session = false;
    if (!dtls->ssl_session && dtls->listening && dtls->ssl_ctx) {
        vox_mpool_t* mpool = vox_loop_get_mpool(dtls->handle.loop);
        dtls->ssl_session = vox_ssl_session_create(dtls->ssl_ctx, mpool);
        if (!dtls->ssl_session) {
            VOX_LOG_ERROR("Failed to create SSL session for server");
            return;
        }
        is_new_server_session = true;
    }
    
    if (!dtls->ssl_session) {
        return;
    }

    /* 调试：记录接收到的数据 */
    //if (dtls->handshaking) {
    //    VOX_LOG_WARN("DTLS handshaking: received %zd bytes from UDP", nread);
    //}

    /* 将数据写入 rbio，处理部分写入 */
    size_t total_written = 0;
    while (total_written < (size_t)nread) {
        ssize_t written = vox_ssl_bio_write(dtls->ssl_session, VOX_SSL_BIO_RBIO,
                                            (const char*)buf + total_written,
                                            (size_t)nread - total_written);
        if (written < 0) {
            VOX_LOG_ERROR("Failed to write to rbio");
            if (dtls->read_cb) {
                const vox_socket_addr_t* peer_addr = dtls->peer_addr_set ? &dtls->peer_addr : NULL;
                dtls->read_cb(dtls, -1, NULL, peer_addr, vox_handle_get_data((vox_handle_t*)dtls));
            }
            return;
        }
        if (written == 0) {
            /* BIO 已满，这在 Memory BIO 中不应该发生，但仍需处理 */
            VOX_LOG_ERROR("rbio write returned 0, possible BIO full");
            if (dtls->read_cb) {
                const vox_socket_addr_t* peer_addr = dtls->peer_addr_set ? &dtls->peer_addr : NULL;
                dtls->read_cb(dtls, -1, NULL, peer_addr, vox_handle_get_data((vox_handle_t*)dtls));
            }
            return;
        }
        total_written += (size_t)written;
    }

    /* 先处理 rbio 数据（握手或读取），这样在 connection_cb 中调用 vox_dtls_handshake 时，
     * rbio 中的数据已经被处理，握手可以继续 */
    dtls_process_rbio_data(dtls);

    /* 如果是新的服务器 Session 或新客户端，在数据写入 rbio 并处理后调用 connection_cb */
    if ((is_new_server_session || is_new_client) && dtls->connection_cb && addr) {
        dtls->connection_cb(dtls, 0, vox_handle_get_data((vox_handle_t*)dtls));
    }
}

/* UDP 发送回调 */
static void dtls_udp_send_cb(vox_udp_t* udp, int status, void* user_data) {
    (void)udp;
    vox_dtls_t* dtls = (vox_dtls_t*)user_data;

    if (!dtls) {
        return;
    }

    if (status != 0) {
        VOX_LOG_ERROR("DTLS UDP send failed with status %d", status);
        return;
    }

    //if (dtls->handshaking) {
    //    VOX_LOG_WARN("DTLS handshaking: UDP send completed successfully");
    //}

    /* 继续处理 wbio 数据（可能有更多数据需要写入） */
    dtls_process_wbio_data(dtls);

    /* 如果写入队列中有请求，继续处理 */
    dtls_process_write_queue(dtls);
}

/* 处理写入请求队列 */
static void dtls_process_write_queue(vox_dtls_t* dtls) {
    if (!dtls || !dtls->write_queue) {
        return;
    }
    
    /* 在握手期间或DTLS已连接时处理写队列 */
    if (!dtls->dtls_connected && !dtls->handshaking) {
        return;
    }

    vox_mpool_t* mpool = vox_loop_get_mpool(dtls->handle.loop);
    vox_dtls_write_req_t* req = (vox_dtls_write_req_t*)dtls->write_queue;

    while (req) {
        /* 计算剩余需要写入的数据 */
        const void* buf = (const char*)req->buf + req->offset;
        size_t remaining = req->len - req->offset;

        if (remaining == 0) {
            /* 当前请求已完成 */
            vox_dtls_write_cb cb = req->cb;
            vox_dtls_write_req_t* next = req->next;

            /* 从链表中移除当前请求 */
            dtls->write_queue = (void*)next;
            
            /* 更新尾指针：如果移除的是尾节点，需要更新尾指针 */
            if (dtls->write_queue_tail == (void*)req) {
                dtls->write_queue_tail = NULL;
            }

            /* 调用回调 */
            if (cb) {
                cb(dtls, 0, vox_handle_get_data((vox_handle_t*)dtls));
            }

            /* 释放复制的缓冲区 */
            if (req->buf) {
                vox_mpool_free(mpool, req->buf);
            }

            /* 释放当前请求 */
            vox_mpool_free(mpool, req);

            /* 处理下一个请求 */
            req = next;
            continue;
        }

        /* 尝试写入到 SSL */
        ssize_t nwritten = vox_ssl_session_write(dtls->ssl_session, buf, remaining);

        if (nwritten < 0) {
            if (nwritten == VOX_SSL_ERROR_WANT_WRITE) {
                /* 需要写入更多数据，处理 wbio 数据 */
                dtls_process_wbio_data(dtls);
                /* 等待可写事件 */
                break;
            } else if (nwritten == VOX_SSL_ERROR_WANT_READ) {
                /* 需要读取更多数据，这是不正常的，但继续处理 */
                break;
            } else {
                /* 写入错误 */
                vox_dtls_write_cb cb = req->cb;
                vox_dtls_write_req_t* next = req->next;

                /* 从链表中移除当前请求 */
                dtls->write_queue = (void*)next;
                
                /* 更新尾指针：如果移除的是尾节点，需要更新尾指针 */
                if (dtls->write_queue_tail == (void*)req) {
                    dtls->write_queue_tail = NULL;
                }

                /* 调用回调（错误） */
                if (cb) {
                    cb(dtls, -1, vox_handle_get_data((vox_handle_t*)dtls));
                }

                /* 释放当前请求 */
                vox_mpool_free(mpool, req);

                /* 处理下一个请求 */
                req = next;
                continue;
            }
        }

        /* 更新偏移量 */
        req->offset += (size_t)nwritten;

        /* 处理 wbio 数据（写入 SSL 后可能产生加密数据） */
        dtls_process_wbio_data(dtls);

        if (req->offset >= req->len) {
            /* 当前请求已完成 */
            vox_dtls_write_cb cb = req->cb;
            vox_dtls_write_req_t* next = req->next;

            /* 从链表中移除当前请求 */
            dtls->write_queue = (void*)next;
            
            /* 更新尾指针：如果移除的是尾节点，需要更新尾指针 */
            if (dtls->write_queue_tail == (void*)req) {
                dtls->write_queue_tail = NULL;
            }

            /* 调用回调 */
            if (cb) {
                cb(dtls, 0, vox_handle_get_data((vox_handle_t*)dtls));
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

    /* 如果队列为空，清理尾指针 */
    if (!dtls->write_queue) {
        dtls->write_queue_tail = NULL;
    }
}

/* 初始化 DTLS 句柄 */
int vox_dtls_init(vox_dtls_t* dtls, vox_loop_t* loop, vox_ssl_context_t* ssl_ctx) {
    if (!dtls || !loop) {
        return -1;
    }

    memset(dtls, 0, sizeof(vox_dtls_t));

    /* 初始化句柄基类 */
    if (vox_handle_init((vox_handle_t*)dtls, VOX_HANDLE_DTLS, loop) != 0) {
        return -1;
    }

    /* 创建底层 UDP 句柄 */
    dtls->udp = vox_udp_create(loop);
    if (!dtls->udp) {
        return -1;
    }

    /* 设置 UDP 回调 */
    vox_handle_set_data((vox_handle_t*)dtls->udp, dtls);

    /* 创建或使用提供的 SSL Context */
    vox_mpool_t* mpool = vox_loop_get_mpool(loop);
    if (!ssl_ctx) {
        /* 创建默认的客户端 context（使用 DTLS） */
        dtls->ssl_ctx = vox_ssl_context_create(mpool, VOX_SSL_MODE_CLIENT);
        if (!dtls->ssl_ctx) {
            vox_udp_destroy(dtls->udp);
            return -1;
        }
        /* 配置为 DTLS */
        vox_ssl_config_t ssl_config = {0};
        ssl_config.protocols = "DTLS";
        if (vox_ssl_context_configure(dtls->ssl_ctx, &ssl_config) != 0) {
            VOX_LOG_ERROR("Failed to configure DTLS context");
            vox_ssl_context_destroy(dtls->ssl_ctx);
            vox_udp_destroy(dtls->udp);
            return -1;
        }
    } else {
        dtls->ssl_ctx = ssl_ctx;
    }

    dtls->bound = false;
    dtls->dtls_connected = false;
    dtls->listening = false;
    dtls->reading = false;
    dtls->handshaking = false;
    dtls->shutting_down = false;
    dtls->peer_addr_set = false;

    return 0;
}

/* 创建 DTLS 句柄 */
vox_dtls_t* vox_dtls_create(vox_loop_t* loop, vox_ssl_context_t* ssl_ctx) {
    if (!loop) {
        return NULL;
    }

    vox_mpool_t* mpool = vox_loop_get_mpool(loop);
    vox_dtls_t* dtls = (vox_dtls_t*)vox_mpool_alloc(mpool, sizeof(vox_dtls_t));
    if (!dtls) {
        return NULL;
    }

    if (vox_dtls_init(dtls, loop, ssl_ctx) != 0) {
        vox_mpool_free(mpool, dtls);
        return NULL;
    }

    return dtls;
}

/* 销毁 DTLS 句柄 */
void vox_dtls_destroy(vox_dtls_t* dtls) {
    if (!dtls) {
        return;
    }

    /* 停止读取 */
    if (dtls->reading) {
        vox_dtls_read_stop(dtls);
    }

    /* 销毁 SSL Session */
    if (dtls->ssl_session) {
        vox_ssl_session_destroy(dtls->ssl_session);
        dtls->ssl_session = NULL;
    }

    /* 销毁底层 UDP 句柄 */
    if (dtls->udp) {
        /* 清除 user_data 以防止回调访问正在销毁的 DTLS 句柄 */
        vox_handle_set_data((vox_handle_t*)dtls->udp, NULL);
        vox_udp_destroy(dtls->udp);
        dtls->udp = NULL;
    }

    /* 释放读取缓冲区 */
    if (dtls->read_buf) {
        vox_mpool_t* mpool = vox_loop_get_mpool(dtls->handle.loop);
        vox_mpool_free(mpool, dtls->read_buf);
        dtls->read_buf = NULL;
        dtls->read_buf_size = 0;
    }

    /* 释放 BIO 缓冲区 */
    vox_mpool_t* mpool = vox_loop_get_mpool(dtls->handle.loop);
    if (dtls->rbio_buf) {
        vox_mpool_free(mpool, dtls->rbio_buf);
        dtls->rbio_buf = NULL;
        dtls->rbio_buf_size = 0;
    }
    if (dtls->wbio_buf) {
        vox_mpool_free(mpool, dtls->wbio_buf);
        dtls->wbio_buf = NULL;
        dtls->wbio_buf_size = 0;
    }

    /* Clean up write queue without invoking callbacks.
     * Applications should ensure operations complete before destroying handles.
     * Invoking callbacks during destroy can cause use-after-free issues. */
    if (dtls->write_queue) {
        vox_dtls_write_req_t* req = (vox_dtls_write_req_t*)dtls->write_queue;
        while (req) {
            vox_dtls_write_req_t* next = req->next;
            /* Don't invoke callbacks during destroy - just free resources */
            /* 释放复制的缓冲区 */
            if (req->buf) {
                vox_mpool_free(mpool, req->buf);
            }
            /* 释放请求结构 */
            vox_mpool_free(mpool, req);
            req = next;
        }
        dtls->write_queue = NULL;
        dtls->write_queue_tail = NULL;
    }

    /* 销毁 SSL Context（如果是自己创建的） */
    /* 注意：如果 ssl_ctx 是外部提供的，不应该在这里销毁 */
    /* 这里我们假设如果 ssl_ctx 是外部提供的，调用者会负责销毁 */
}

/* 绑定地址 */
int vox_dtls_bind(vox_dtls_t* dtls, const vox_socket_addr_t* addr, unsigned int flags) {
    if (!dtls || !dtls->udp || !addr) {
        return -1;
    }

    return vox_udp_bind(dtls->udp, addr, flags);
}

/* 开始监听连接 */
int vox_dtls_listen(vox_dtls_t* dtls, int backlog, vox_dtls_connection_cb cb) {
    (void)backlog;  /* UDP 中此参数被忽略 */
    if (!dtls || !dtls->udp) {
        return -1;
    }

    dtls->connection_cb = cb;
    dtls->listening = true;

    /* 开始接收数据 */
    return vox_udp_recv_start(dtls->udp, NULL, dtls_udp_recv_cb);
}

/* 接受连接 */
int vox_dtls_accept(vox_dtls_t* server, vox_dtls_t* client, const vox_socket_addr_t* addr) {
    if (!server || !client || !server->udp || !client->udp || !addr) {
        return -1;
    }

    /* 对于 UDP，accept 主要是设置对端地址和创建 SSL Session */
    client->peer_addr = *addr;
    client->peer_addr_set = true;

    /* 创建 SSL Session */
    vox_mpool_t* mpool = vox_loop_get_mpool(client->handle.loop);
    client->ssl_session = vox_ssl_session_create(server->ssl_ctx, mpool);
    if (!client->ssl_session) {
        return -1;
    }

    /* 确保客户端的 SSL Context 指向服务器的 SSL Context */
    client->ssl_ctx = server->ssl_ctx;

    return 0;
}

/* 异步连接 */
int vox_dtls_connect(vox_dtls_t* dtls, const vox_socket_addr_t* addr, vox_dtls_connect_cb cb) {
    if (!dtls || !dtls->udp || !addr) {
        return -1;
    }

    dtls->connect_cb = cb;
    dtls->peer_addr = *addr;
    dtls->peer_addr_set = true;

    /* 如果 UDP socket 还没有创建（未绑定），自动绑定到 0.0.0.0:0（让系统自动分配端口） */
    if (dtls->udp->socket.fd == VOX_INVALID_SOCKET) {
        vox_socket_addr_t bind_addr;
        /* 使用与目标地址相同的地址族 */
        bind_addr.family = addr->family;
        if (addr->family == VOX_AF_INET) {
            bind_addr.u.ipv4.addr = 0;  /* INADDR_ANY */
            bind_addr.u.ipv4.port = 0;  /* 让系统自动分配端口 */
        } else if (addr->family == VOX_AF_INET6) {
            memset(bind_addr.u.ipv6.addr, 0, 16);  /* in6addr_any */
            bind_addr.u.ipv6.port = 0;  /* 让系统自动分配端口 */
        } else {
            return -1;
        }
        
        if (vox_dtls_bind(dtls, &bind_addr, 0) != 0) {
            return -1;
        }
    }

    /* 创建 SSL Session（如果还没有创建） */
    if (!dtls->ssl_session) {
        vox_mpool_t* mpool = vox_loop_get_mpool(dtls->handle.loop);
        dtls->ssl_session = vox_ssl_session_create(dtls->ssl_ctx, mpool);
        if (!dtls->ssl_session) {
            return -1;
        }
    }

    /* 开始接收数据（用于接收握手数据） */
    if (!dtls->udp->receiving) {
        if (vox_udp_recv_start(dtls->udp, NULL, dtls_udp_recv_cb) != 0) {
            return -1;
        }
    }

    /* 开始 DTLS 握手 */
    if (vox_dtls_handshake(dtls, NULL) != 0) {
        /* 握手启动失败 */
        VOX_LOG_ERROR("Failed to start DTLS handshake");
        if (dtls->connect_cb) {
            vox_dtls_connect_cb saved_cb = dtls->connect_cb;
            void* saved_user_data = vox_handle_get_data((vox_handle_t*)dtls);
            dtls->connect_cb = NULL;
            if (saved_cb) {
                saved_cb(dtls, -1, saved_user_data);
            }
        }
        return -1;
    }

    /* 握手回调会在握手完成后调用 connect_cb */
    
    /* 处理写队列以确保握手消息可以发送 */
    dtls_process_write_queue(dtls);

    return 0;
}

/* 开始 DTLS 握手 */
int vox_dtls_handshake(vox_dtls_t* dtls, vox_dtls_handshake_cb cb) {
    if (!dtls || !dtls->ssl_session) {
        return -1;
    }

    if (dtls->handshaking) {
        /* 如果已经在握手，但 handshake_cb 还没有设置，现在设置它 */
        if (cb && !dtls->handshake_cb) {
            dtls->handshake_cb = cb;
        }
        return 0;  /* 已经在握手 */
    }

    dtls->handshaking = true;
    dtls->handshake_cb = cb;

    /* 开始读取（用于接收握手数据） */
    if (!dtls->udp->receiving) {
        if (vox_udp_recv_start(dtls->udp, NULL, dtls_udp_recv_cb) != 0) {
            VOX_LOG_ERROR("Failed to start UDP read for DTLS handshake");
            dtls->handshaking = false;
            return -1;
        }
    }

    /* 尝试开始握手 */
    int ret = vox_ssl_session_handshake(dtls->ssl_session);
    if (ret == 0) {
        /* 握手立即成功 */
        dtls->handshaking = false;
        dtls->dtls_connected = true;
        /* 处理 wbio 数据（发送握手消息） */
        dtls_process_wbio_data(dtls);
        /* 处理写队列以确保握手消息可以发送 */
        dtls_process_write_queue(dtls);
        if (cb) {
            cb(dtls, 0, vox_handle_get_data((vox_handle_t*)dtls));
        }
    } else if (ret == VOX_SSL_ERROR_WANT_READ || ret == VOX_SSL_ERROR_WANT_WRITE) {
        /* 需要更多数据，这是正常的 */
        /* 处理 wbio 数据（发送握手消息到对端） */
        dtls_process_wbio_data(dtls);
        /* 处理写队列以确保握手消息可以发送 */
        dtls_process_write_queue(dtls);
        /* 握手会继续，等待更多数据 */
    } else {
        /* 握手失败 */
        char err_buf[256];
        vox_ssl_session_get_error_string(dtls->ssl_session, err_buf, sizeof(err_buf));
        VOX_LOG_ERROR("DTLS handshake failed: ret=%d, error=%s", ret, err_buf);
        dtls->handshaking = false;
        if (cb) {
            cb(dtls, -1, vox_handle_get_data((vox_handle_t*)dtls));
        }
        return -1;
    }

    return 0;
}

/* 开始异步读取 */
int vox_dtls_read_start(vox_dtls_t* dtls, vox_dtls_alloc_cb alloc_cb, vox_dtls_read_cb read_cb) {
    if (!dtls || !dtls->udp) {
        return -1;
    }

    if (dtls->udp->socket.fd == VOX_INVALID_SOCKET) {
        return -1;
    }

    if (!dtls->dtls_connected) {
        return -1;  /* DTLS 未连接 */
    }

    if (dtls->reading) {
        return 0;  /* 已经在读取 */
    }

    dtls->reading = true;
    dtls->alloc_cb = alloc_cb;
    dtls->read_cb = read_cb;

    /* 开始 UDP 读取（如果还没有开始） */
    if (!dtls->udp->receiving) {
        if (vox_udp_recv_start(dtls->udp, NULL, dtls_udp_recv_cb) != 0) {
            dtls->reading = false;
            return -1;
        }
    }

    /* 尝试读取已有数据 */
    dtls_process_rbio_data(dtls);

    return 0;
}

/* 停止异步读取 */
int vox_dtls_read_stop(vox_dtls_t* dtls) {
    if (!dtls || !dtls->udp) {
        return -1;
    }

    if (!dtls->reading) {
        return 0;
    }

    dtls->reading = false;
    dtls->read_cb = NULL;
    dtls->alloc_cb = NULL;

    return 0;
}

/* 异步写入 */
int vox_dtls_write(vox_dtls_t* dtls, const void* buf, size_t len, 
                   const vox_socket_addr_t* addr, vox_dtls_write_cb cb) {
    if (!dtls || !buf || len == 0) {
        return -1;
    }

    /* 检查 UDP 句柄是否有效 */
    if (!dtls->udp) {
        return -1;
    }

    if (dtls->udp->socket.fd == VOX_INVALID_SOCKET) {
        return -1;
    }

    if (!dtls->dtls_connected) {
        return -1;  /* DTLS 未连接 */
    }

    /* 确定目标地址 */
    const vox_socket_addr_t* target_addr = addr;
    if (!target_addr && dtls->peer_addr_set) {
        target_addr = &dtls->peer_addr;
    }
    if (!target_addr) {
        return -1;  /* 没有目标地址 */
    }

    vox_mpool_t* mpool = vox_loop_get_mpool(dtls->handle.loop);

    /* 如果有待处理的写入请求，直接加入队列 */
    if (dtls->write_queue) {
        /* 创建新的写入请求 */
        vox_dtls_write_req_t* req = (vox_dtls_write_req_t*)vox_mpool_alloc(
            mpool, sizeof(vox_dtls_write_req_t));
        if (!req) {
            return -1;
        }

        /* 分配并复制数据（避免缓冲区重用问题） */
        void* buf_copy = vox_mpool_alloc(mpool, len);
        if (!buf_copy) {
            vox_mpool_free(mpool, req);
            return -1;
        }
        memcpy(buf_copy, buf, len);

        req->buf = buf_copy;
        req->len = len;
        req->offset = 0;
        req->addr = *target_addr;
        req->cb = cb;
        req->next = NULL;

        /* 添加到队列末尾（使用尾指针，O(1) 操作） */
        vox_dtls_write_req_t* old_tail = (vox_dtls_write_req_t*)dtls->write_queue_tail;
        if (old_tail) {
            old_tail->next = req;
        } else {
            /* 队列为空，这不应该发生（因为已经检查了 write_queue） */
            dtls->write_queue = (void*)req;
        }
        dtls->write_queue_tail = (void*)req;

        return 0;
    }

    /* 尝试立即写入 */
    ssize_t nwritten = vox_ssl_session_write(dtls->ssl_session, buf, len);

    if (nwritten < 0) {
        if (nwritten == VOX_SSL_ERROR_WANT_WRITE) {
            /* 需要写入更多数据，处理 wbio 数据（除非正在进行握手） */
            if (!dtls->handshaking) {
                dtls_process_wbio_data(dtls);
            }
            /* 加入队列 */
            nwritten = 0;
        } else if (nwritten == VOX_SSL_ERROR_WANT_READ) {
            /* 需要读取更多数据，这是不正常的，但继续处理 */
            nwritten = 0;
        } else {
            /* 写入错误 */
            return -1;
        }
    }

    /* 只有在未处于握手状态时才处理 wbio 数据，避免在握手期间递归调用握手 */
    if (!dtls->handshaking) {
        dtls_process_wbio_data(dtls);
    }

    if (nwritten >= 0 && (size_t)nwritten == len) {
        /* 全部写入完成 */
        if (cb) {
            cb(dtls, 0, vox_handle_get_data((vox_handle_t*)dtls));
        }
        return 0;
    }

    /* 部分写入或需要等待，创建写入请求并加入队列 */
    vox_dtls_write_req_t* req = (vox_dtls_write_req_t*)vox_mpool_alloc(
        mpool, sizeof(vox_dtls_write_req_t));
    if (!req) {
        return -1;
    }

    /* 分配并复制数据（避免缓冲区重用问题） */
    void* buf_copy = vox_mpool_alloc(mpool, len);
    if (!buf_copy) {
        vox_mpool_free(mpool, req);
        return -1;
    }
    memcpy(buf_copy, buf, len);

    req->buf = buf_copy;
    req->len = len;
    req->offset = (size_t)nwritten;
    req->addr = *target_addr;
    req->cb = cb;
    req->next = NULL;

    /* 添加到队列（使用尾指针，O(1) 操作） */
    vox_dtls_write_req_t* old_tail = (vox_dtls_write_req_t*)dtls->write_queue_tail;
    if (old_tail) {
        old_tail->next = req;
    } else {
        /* 队列为空 */
        dtls->write_queue = (void*)req;
    }
    dtls->write_queue_tail = (void*)req;

    return 0;
}

/* 关闭写入端 */
int vox_dtls_shutdown(vox_dtls_t* dtls, vox_dtls_shutdown_cb cb) {
    if (!dtls || !dtls->ssl_session) {
        return -1;
    }

    dtls->shutdown_cb = cb;
    dtls->shutting_down = true;

    /* 执行 SSL shutdown */
    int ret = vox_ssl_session_shutdown(dtls->ssl_session);
    if (ret == 0) {
        /* 关闭完成 */
        dtls->shutting_down = false;
        if (cb) {
            cb(dtls, 0, vox_handle_get_data((vox_handle_t*)dtls));
        }
        dtls->shutdown_cb = NULL;
        return 0;
    } else if (ret == VOX_SSL_ERROR_WANT_READ || ret == VOX_SSL_ERROR_WANT_WRITE) {
        /* 需要更多数据来完成关闭 */
        /* 处理 wbio 数据 */
        dtls_process_wbio_data(dtls);
        /* 关闭是异步的，回调会在完成后调用 */
        return 0;
    } else {
        /* 关闭失败 */
        dtls->shutting_down = false;
        if (cb) {
            cb(dtls, -1, vox_handle_get_data((vox_handle_t*)dtls));
        }
        dtls->shutdown_cb = NULL;
        return -1;
    }
}

/* 获取本地地址 */
int vox_dtls_getsockname(vox_dtls_t* dtls, vox_socket_addr_t* addr) {
    if (!dtls || !dtls->udp || !addr) {
        return -1;
    }

    return vox_udp_getsockname(dtls->udp, addr);
}

/* 获取对端地址 */
int vox_dtls_getpeername(vox_dtls_t* dtls, vox_socket_addr_t* addr) {
    if (!dtls || !addr) {
        return -1;
    }

    if (!dtls->peer_addr_set) {
        return -1;
    }

    *addr = dtls->peer_addr;
    return 0;
}

/* 设置广播选项 */
int vox_dtls_set_broadcast(vox_dtls_t* dtls, bool enable) {
    if (!dtls || !dtls->udp) {
        return -1;
    }

    return vox_udp_set_broadcast(dtls->udp, enable);
}

/* 设置地址重用选项 */
int vox_dtls_set_reuseaddr(vox_dtls_t* dtls, bool enable) {
    if (!dtls || !dtls->udp) {
        return -1;
    }

    return vox_udp_set_reuseaddr(dtls->udp, enable);
}
