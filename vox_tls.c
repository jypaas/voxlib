/*
 * vox_tls.c - TLS 异步操作实现
 * 基于 vox_tcp 实现跨平台 TLS
 */

#include "vox_tls.h"
#include "vox_loop.h"
#include "vox_backend.h"
#include "vox_mpool.h"
#include "vox_log.h"
#include "vox_socket.h"
#include <string.h>

/* 默认读取缓冲区大小 */
#define VOX_TLS_DEFAULT_READ_BUF_SIZE 4096
#define VOX_TLS_DEFAULT_BIO_BUF_SIZE 16384  /* BIO 缓冲区大小 */

/* TLS 写入请求结构 */
typedef struct vox_tls_write_req {
    void* buf;                         /* 数据缓冲区（我们拥有的副本） */
    size_t len;                        /* 总长度 */
    size_t offset;                     /* 已写入的偏移量 */
    vox_tls_write_cb cb;               /* 写入完成回调 */
    struct vox_tls_write_req* next;    /* 下一个请求（链表） */
} vox_tls_write_req_t;

/* TLS 句柄内部数据 */
typedef struct {
    vox_tls_t* tls;
    void* user_data;
} vox_tls_internal_data_t;

/* 前向声明 */
static void tls_process_write_queue(vox_tls_t* tls);
static int tls_process_rbio_data(vox_tls_t* tls);
static int tls_process_wbio_data(vox_tls_t* tls);
static void tls_tcp_read_cb(vox_tcp_t* tcp, ssize_t nread, const void* buf, void* user_data);
static void tls_tcp_write_cb(vox_tcp_t* tcp, int status, void* user_data);
static void tls_tcp_connect_cb(vox_tcp_t* tcp, int status, void* user_data);
static void tls_tcp_connection_cb(vox_tcp_t* server, int status, void* user_data);

/* 处理 rbio 数据：从 socket 读取后写入 rbio，然后尝试 SSL 操作 */
static int tls_process_rbio_data(vox_tls_t* tls) {
    if (!tls || !tls->ssl_session || !tls->tcp) {
        return -1;
    }

    /* 如果正在握手，尝试继续握手 */
    if (tls->handshaking) {
        int ret = vox_ssl_session_handshake(tls->ssl_session);
        if (ret == 0) {
            /* 握手成功 */
            tls->handshaking = false;
            tls->tls_connected = true;
            if (tls->handshake_cb) {
                vox_tls_handshake_cb saved_cb = tls->handshake_cb;
                void* saved_user_data = vox_handle_get_data((vox_handle_t*)tls);
                tls->handshake_cb = NULL;
                if (saved_cb) {
                    saved_cb(tls, 0, saved_user_data);
                }
            }
            /* 握手完成后，处理 wbio 数据和写队列 */
            tls_process_wbio_data(tls);
            tls_process_write_queue(tls);
        } else if (ret == VOX_SSL_ERROR_WANT_READ || ret == VOX_SSL_ERROR_WANT_WRITE) {
            /* 需要更多数据，这是正常的 */
            /* 处理 wbio 数据（服务器端可能需要先发送数据，即使返回 WANT_READ） */
            tls_process_wbio_data(tls);
            /* 处理写队列以确保握手消息可以发送 */
            tls_process_write_queue(tls);
            /* 握手会继续，等待更多数据 */
        } else {
            /* 握手失败 */
            char err_buf[256];
            vox_ssl_session_get_error_string(tls->ssl_session, err_buf, sizeof(err_buf));
            VOX_LOG_ERROR("TLS handshake failed in process_rbio: ret=%d, error=%s", ret, err_buf);
            tls->handshaking = false;
            tls->tls_connected = false;
            if (tls->handshake_cb) {
                vox_tls_handshake_cb saved_cb = tls->handshake_cb;
                void* saved_user_data = vox_handle_get_data((vox_handle_t*)tls);
                tls->handshake_cb = NULL;
                if (saved_cb) {
                    saved_cb(tls, -1, saved_user_data);
                }
            }
            /* 即使握手失败，也要尝试处理写队列 */
            tls_process_write_queue(tls);
            return -1;
        }
    }

    /* 检查是否正在关闭并尝试完成关闭 */
    if (tls->shutting_down && tls->shutdown_cb) {
        int ret = vox_ssl_session_shutdown(tls->ssl_session);
        if (ret == 0) {
            /* 关闭完成 */
            tls->shutting_down = false;
            vox_tls_shutdown_cb saved_cb = tls->shutdown_cb;
            void* saved_user_data = vox_handle_get_data((vox_handle_t*)tls);
            tls->shutdown_cb = NULL;
            if (saved_cb) {
                saved_cb(tls, 0, saved_user_data);
            }
        } else if (ret != VOX_SSL_ERROR_WANT_READ && ret != VOX_SSL_ERROR_WANT_WRITE) {
            /* 关闭失败 */
            tls->shutting_down = false;
            vox_tls_shutdown_cb saved_cb = tls->shutdown_cb;
            void* saved_user_data = vox_handle_get_data((vox_handle_t*)tls);
            tls->shutdown_cb = NULL;
            if (saved_cb) {
                saved_cb(tls, -1, saved_user_data);
            }
        }
        /* 处理 wbio 数据 */
        tls_process_wbio_data(tls);
    }

    /* 如果握手完成且是客户端，调用 connect_cb */
    if (tls->tls_connected && tls->connect_cb && !tls->listening) {
        vox_tls_connect_cb saved_cb = tls->connect_cb;
        void* saved_user_data = vox_handle_get_data((vox_handle_t*)tls);
        tls->connect_cb = NULL;
        if (saved_cb) {
            saved_cb(tls, 0, saved_user_data);
        }
    }

    /* 如果已连接，尝试读取解密后的数据 */
    if (tls->tls_connected && tls->reading && tls->read_cb) {
        /* 循环读取，直到没有更多数据 */
        /* 限制循环次数，避免无限循环 */
        int max_iterations = 100;
        while (max_iterations-- > 0) {
        //while (true) {    
            /* 分配读取缓冲区 */
            void* buf = NULL;
            size_t len = 0;

            if (tls->alloc_cb) {
                tls->alloc_cb(tls, VOX_TLS_DEFAULT_READ_BUF_SIZE, &buf, &len,
                             vox_handle_get_data((vox_handle_t*)tls));
            } else {
                /* 使用默认缓冲区 */
                vox_mpool_t* mpool = vox_loop_get_mpool(tls->handle.loop);
                if (!tls->read_buf || tls->read_buf_size < VOX_TLS_DEFAULT_READ_BUF_SIZE) {
                    if (tls->read_buf) {
                        vox_mpool_free(mpool, tls->read_buf);
                    }
                    tls->read_buf = vox_mpool_alloc(mpool, VOX_TLS_DEFAULT_READ_BUF_SIZE);
                    if (!tls->read_buf) {
                        VOX_LOG_ERROR("Failed to allocate read buffer");
                        break;
                    }
                    tls->read_buf_size = VOX_TLS_DEFAULT_READ_BUF_SIZE;
                }
                buf = tls->read_buf;
                len = tls->read_buf_size;
            }
            
            if (!buf || len == 0) {
                break;
            }

            /* 尝试读取解密后的数据 */
            ssize_t nread = vox_ssl_session_read(tls->ssl_session, buf, len);

            if (nread > 0) {
                /* 成功读取数据 */
                /* 注意：回调可能会修改 TLS 状态（例如停止读取），所以先保存状态 */
                bool still_reading = tls->reading;
                void* saved_user_data = vox_handle_get_data((vox_handle_t*)tls);
                tls->read_cb(tls, nread, buf, saved_user_data);
                /* 如果回调后不再读取，退出循环 */
                if (!still_reading || !tls->reading) {
                    break;
                }
                /* 检查 rbio 中是否还有更多数据 */
                /* 如果没有更多数据，退出循环，等待更多 TCP 数据 */
                size_t rbio_pending = vox_ssl_bio_pending(tls->ssl_session, VOX_SSL_BIO_RBIO);
                if (rbio_pending == 0) {
                    /* 没有更多数据，退出循环，等待更多 TCP 数据 */
                    break;
                }
                /* 继续循环，尝试读取更多数据 */
            } else if (nread == 0) {
                /* 连接关闭 */
                tls->read_cb(tls, 0, NULL, vox_handle_get_data((vox_handle_t*)tls));
                vox_tls_read_stop(tls);
                break;
            } else if (nread == VOX_SSL_ERROR_WANT_READ) {
                /* 需要更多数据，退出循环，等待更多 TCP 数据 */
                break;
            } else if (nread == VOX_SSL_ERROR_WANT_WRITE) {
                /* 需要写入，处理 wbio 数据 */
                tls_process_wbio_data(tls);
                /* 退出循环，避免无限循环，等待写操作完成后再继续 */
                break;
            } else {
                /* 读取错误 */
                tls->read_cb(tls, -1, NULL, vox_handle_get_data((vox_handle_t*)tls));
                break;
            }
        }
    }

    /* 只有在未处于握手状态时才处理 wbio 数据，避免在握手期间递归调用握手 */
    if (!tls->handshaking) {
        tls_process_wbio_data(tls);
    }

    return 0;
}

/* 处理 wbio 数据：从 wbio 读取加密数据，写入 socket */
static int tls_process_wbio_data(vox_tls_t* tls) {
    if (!tls || !tls->ssl_session || !tls->tcp) {
        return -1;
    }

    /* 循环处理，直到 wbio 中没有数据，带安全限制 */
    int max_iterations = 100;
    while (max_iterations-- > 0) {
        /* 检查 wbio 中是否有数据 */
        size_t pending = vox_ssl_bio_pending(tls->ssl_session, VOX_SSL_BIO_WBIO);
        if (pending == 0) {
            return 0;  /* 没有数据需要写入 */
        }

        /* 分配缓冲区 */
        vox_mpool_t* mpool = vox_loop_get_mpool(tls->handle.loop);
        if (!tls->wbio_buf || tls->wbio_buf_size < pending) {
            if (tls->wbio_buf) {
                vox_mpool_free(mpool, tls->wbio_buf);
            }
            tls->wbio_buf = vox_mpool_alloc(mpool, pending);
            if (!tls->wbio_buf) {
                VOX_LOG_ERROR("Failed to allocate wbio buffer");
                return -1;
            }
            tls->wbio_buf_size = pending;
        }

        /* 从 wbio 读取数据 */
        ssize_t nread = vox_ssl_bio_read(tls->ssl_session, VOX_SSL_BIO_WBIO, tls->wbio_buf, pending);
        if (nread <= 0) {
            /* 没有数据或错误，退出循环 */
            return 0;
        }

        /* 写入 TCP socket */
        int write_result = vox_tcp_write(tls->tcp, tls->wbio_buf, (size_t)nread, tls_tcp_write_cb);
        if (write_result != 0) {
            /* 写入失败，可能是 socket 缓冲区满，等待可写事件 */
            /* 注意：在 IOCP 模式下，vox_tcp_write 会将数据加入队列，如果队列满会返回错误 */
            /* 这种情况下，我们需要等待可写事件，然后继续处理 */
            VOX_LOG_ERROR("Failed to write to TCP socket, pending=%zu, nread=%zd, result=%d", pending, nread, write_result);
            /* TCP socket会自动处理可写事件，无需手动注册backend事件 */
            /* 不返回错误，等待可写事件后继续处理 */
            return 0;
        }

        /* 继续检查是否还有数据需要写入 */
    }

    if (max_iterations <= 0) {
        VOX_LOG_WARN("wbio processing reached iteration limit, possible SSL layer issue");
    }

    return 0;
}

/* TCP 读取回调：从 socket 读取数据后，写入 rbio，然后处理 SSL 操作 */
static void tls_tcp_read_cb(vox_tcp_t* tcp, ssize_t nread, const void* buf, void* user_data) {
    (void)tcp;
    vox_tls_t* tls = (vox_tls_t*)user_data;
    if (!tls || !tls->ssl_session) {
        return;
    }

    if (nread < 0) {
        /* 读取错误 */
        if (tls->read_cb) {
            tls->read_cb(tls, -1, NULL, vox_handle_get_data((vox_handle_t*)tls));
        }
        return;
    }

    if (nread == 0) {
        /* 连接关闭 */
        if (tls->handshaking) {
            /* 握手过程中连接关闭，握手失败 */
            tls->handshaking = false;
            if (tls->handshake_cb) {
                vox_tls_handshake_cb saved_cb = tls->handshake_cb;
                void* saved_user_data = vox_handle_get_data((vox_handle_t*)tls);
                tls->handshake_cb = NULL;
                if (saved_cb) {
                    saved_cb(tls, -1, saved_user_data);
                }
            }
        }
        if (tls->read_cb) {
            tls->read_cb(tls, 0, NULL, vox_handle_get_data((vox_handle_t*)tls));
        }
        return;
    }

    /* 将数据写入 rbio，处理部分写入 */
    size_t total_written = 0;
    while (total_written < (size_t)nread) {
        ssize_t written = vox_ssl_bio_write(tls->ssl_session, VOX_SSL_BIO_RBIO,
                                            (const char*)buf + total_written,
                                            (size_t)nread - total_written);
        if (written < 0) {
            VOX_LOG_ERROR("Failed to write to rbio");
            if (tls->read_cb) {
                tls->read_cb(tls, -1, NULL, vox_handle_get_data((vox_handle_t*)tls));
            }
            return;
        }
        if (written == 0) {
            /* BIO 已满，这在 Memory BIO 中不应该发生，但仍需处理 */
            VOX_LOG_ERROR("rbio write returned 0, possible BIO full");
            if (tls->read_cb) {
                tls->read_cb(tls, -1, NULL, vox_handle_get_data((vox_handle_t*)tls));
            }
            return;
        }
        total_written += (size_t)written;
    }

    /* 处理 rbio 数据（握手或读取） */
    tls_process_rbio_data(tls);
}

/* TCP 写入回调 */
static void tls_tcp_write_cb(vox_tcp_t* tcp, int status, void* user_data) {
    (void)tcp;
    (void)status;
    vox_tls_t* tls = (vox_tls_t*)user_data;

    if (!tls) {
        return;
    }

    /* 继续处理 wbio 数据（可能有更多数据需要写入） */
    tls_process_wbio_data(tls);

    /* 检查 TLS 是否仍在握手状态，如果是，优先处理握手 */
    if (tls->handshaking) {
        /* 尝试继续握手 */
        int ret = vox_ssl_session_handshake(tls->ssl_session);
        if (ret == 0) {
            /* 握手成功 */
            tls->handshaking = false;
            tls->tls_connected = true;
            if (tls->handshake_cb) {
                vox_tls_handshake_cb saved_cb = tls->handshake_cb;
                void* saved_user_data = vox_handle_get_data((vox_handle_t*)tls);
                tls->handshake_cb = NULL;
                if (saved_cb) {
                    saved_cb(tls, 0, saved_user_data);
                }
            }
        } else if (ret != VOX_SSL_ERROR_WANT_READ && ret != VOX_SSL_ERROR_WANT_WRITE) {
            /* 握手失败 */
            char err_buf[256];
            vox_ssl_session_get_error_string(tls->ssl_session, err_buf, sizeof(err_buf));
            VOX_LOG_ERROR("TLS handshake failed in write callback: ret=%d, error=%s", ret, err_buf);
            tls->handshaking = false;
            tls->tls_connected = false;
            if (tls->handshake_cb) {
                vox_tls_handshake_cb saved_cb = tls->handshake_cb;
                void* saved_user_data = vox_handle_get_data((vox_handle_t*)tls);
                tls->handshake_cb = NULL;
                if (saved_cb) {
                    saved_cb(tls, -1, saved_user_data);
                }
            }
        }
    }

    /* 如果写入队列中有请求，继续处理 */
    tls_process_write_queue(tls);
}

/* TCP 连接回调：TCP 连接成功后，开始 TLS 握手 */
static void tls_tcp_connect_cb(vox_tcp_t* tcp, int status, void* user_data) {
    (void)tcp;
    vox_tls_t* tls = (vox_tls_t*)user_data;
    if (!tls) {
        return;
    }

    if (status != 0) {
        /* TCP 连接失败 */
        if (tls->connect_cb) {
            vox_tls_connect_cb saved_cb = tls->connect_cb;
            void* saved_user_data = vox_handle_get_data((vox_handle_t*)tls);
            tls->connect_cb = NULL;
            if (saved_cb) {
                saved_cb(tls, -1, saved_user_data);
            }
        }
        return;
    }

    /* TCP 连接成功，创建 SSL Session（如果还没有创建） */
    tls->connected = true;
    
    if (!tls->ssl_session) {
        if (!tls->ssl_ctx) {
            VOX_LOG_ERROR("TLS context is NULL, cannot create session");
            if (tls->connect_cb) {
                vox_tls_connect_cb saved_cb = tls->connect_cb;
                void* saved_user_data = vox_handle_get_data((vox_handle_t*)tls);
                tls->connect_cb = NULL;
                if (saved_cb) {
                    saved_cb(tls, -1, saved_user_data);
                }
            }
            return;
        }
        
        vox_mpool_t* mpool = vox_loop_get_mpool(tls->handle.loop);
        tls->ssl_session = vox_ssl_session_create(tls->ssl_ctx, mpool);
        if (!tls->ssl_session) {
            VOX_LOG_ERROR("Failed to create SSL session");
            if (tls->connect_cb) {
                vox_tls_connect_cb saved_cb = tls->connect_cb;
                void* saved_user_data = vox_handle_get_data((vox_handle_t*)tls);
                tls->connect_cb = NULL;
                if (saved_cb) {
                    saved_cb(tls, -1, saved_user_data);
                }
            }
            return;
        }
    }
    
    /* 开始 TLS 握手 */
    if (vox_tls_handshake(tls, NULL) != 0) {
        /* 握手启动失败 */
        VOX_LOG_ERROR("Failed to start TLS handshake");
        if (tls->connect_cb) {
            vox_tls_connect_cb saved_cb = tls->connect_cb;
            void* saved_user_data = vox_handle_get_data((vox_handle_t*)tls);
            tls->connect_cb = NULL;
            if (saved_cb) {
                saved_cb(tls, -1, saved_user_data);
            }
        }
        return;
    }

    /* 握手回调会在握手完成后调用 connect_cb */
    
    /* 处理写队列以确保握手消息可以发送 */
    tls_process_write_queue(tls);
}

/* TCP 连接接受回调 */
static void tls_tcp_connection_cb(vox_tcp_t* server, int status, void* user_data) {
    (void)server;
    vox_tls_t* tls = (vox_tls_t*)user_data;
    if (!tls) {
        return;
    }

    if (status != 0) {
        /* 接受连接失败 */
        if (tls->connection_cb) {
            tls->connection_cb(tls, -1, vox_handle_get_data((vox_handle_t*)tls));
        }
        return;
    }

    /* 接受连接成功，调用 connection_cb */
    if (tls->connection_cb) {
        tls->connection_cb(tls, 0, vox_handle_get_data((vox_handle_t*)tls));
    }
    /* Application must call vox_tls_accept() then vox_tls_handshake() on client */
}

/* 注册 TLS 句柄到 backend */
/* 处理写入请求队列 */
static void tls_process_write_queue(vox_tls_t* tls) {
    if (!tls || !tls->write_queue) {
        return;
    }
    
    /* 在握手期间或TLS已连接时处理写队列 */
    if (!tls->tls_connected && !tls->handshaking) {
        return;
    }

    vox_mpool_t* mpool = vox_loop_get_mpool(tls->handle.loop);
    vox_tls_write_req_t* req = (vox_tls_write_req_t*)tls->write_queue;

    while (req) {
        /* 计算剩余需要写入的数据 */
        const void* buf = (const char*)req->buf + req->offset;
        size_t remaining = req->len - req->offset;

        if (remaining == 0) {
            /* 当前请求已完成 */
            vox_tls_write_cb cb = req->cb;
            vox_tls_write_req_t* next = req->next;

            /* 从链表中移除当前请求 */
            tls->write_queue = (void*)next;
            
            /* 更新尾指针：如果移除的是尾节点，需要更新尾指针 */
            if (tls->write_queue_tail == (void*)req) {
                tls->write_queue_tail = NULL;
            }

            /* 调用回调 */
            if (cb) {
                cb(tls, 0, vox_handle_get_data((vox_handle_t*)tls));
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
        ssize_t nwritten = vox_ssl_session_write(tls->ssl_session, buf, remaining);

        if (nwritten < 0) {
            if (nwritten == VOX_SSL_ERROR_WANT_WRITE) {
                /* 需要写入更多数据，处理 wbio 数据 */
                tls_process_wbio_data(tls);
                /* 等待可写事件 */
                break;
            } else if (nwritten == VOX_SSL_ERROR_WANT_READ) {
                /* 需要读取更多数据，这是不正常的，但继续处理 */
                break;
            } else {
                /* 写入错误 */
                vox_tls_write_cb cb = req->cb;
                vox_tls_write_req_t* next = req->next;

                /* 从链表中移除当前请求 */
                tls->write_queue = (void*)next;
                
                /* 更新尾指针：如果移除的是尾节点，需要更新尾指针 */
                if (tls->write_queue_tail == (void*)req) {
                    tls->write_queue_tail = NULL;
                }

                /* 调用回调（错误） */
                if (cb) {
                    cb(tls, -1, vox_handle_get_data((vox_handle_t*)tls));
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
        tls_process_wbio_data(tls);

        if (req->offset >= req->len) {
            /* 当前请求已完成 */
            vox_tls_write_cb cb = req->cb;
            vox_tls_write_req_t* next = req->next;

            /* 从链表中移除当前请求 */
            tls->write_queue = (void*)next;
            
            /* 更新尾指针：如果移除的是尾节点，需要更新尾指针 */
            if (tls->write_queue_tail == (void*)req) {
                tls->write_queue_tail = NULL;
            }

            /* 调用回调 */
            if (cb) {
                cb(tls, 0, vox_handle_get_data((vox_handle_t*)tls));
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

    /* 如果队列为空，记录状态 */
    if (!tls->write_queue) {
        /* 确保尾指针也为空 */
        tls->write_queue_tail = NULL;
        /* TCP socket会自动处理可写事件的取消，无需手动更新 */
    }
}

/* 初始化 TLS 句柄 */
int vox_tls_init(vox_tls_t* tls, vox_loop_t* loop, vox_ssl_context_t* ssl_ctx) {
    if (!tls || !loop) {
        return -1;
    }

    memset(tls, 0, sizeof(vox_tls_t));

    /* 初始化句柄基类 */
    if (vox_handle_init((vox_handle_t*)tls, VOX_HANDLE_TLS, loop) != 0) {
        return -1;
    }

    /* 创建底层 TCP 句柄 */
    tls->tcp = vox_tcp_create(loop);
    if (!tls->tcp) {
        return -1;
    }

    /* 设置 TCP 回调 */
    vox_handle_set_data((vox_handle_t*)tls->tcp, tls);

    /* 创建或使用提供的 SSL Context */
    vox_mpool_t* mpool = vox_loop_get_mpool(loop);
    if (!ssl_ctx) {
        /* 创建默认的客户端 context */
        tls->ssl_ctx = vox_ssl_context_create(mpool, VOX_SSL_MODE_CLIENT);
        if (!tls->ssl_ctx) {
            vox_tcp_destroy(tls->tcp);
            return -1;
        }
    } else {
        tls->ssl_ctx = ssl_ctx;
    }

    tls->connected = false;
    tls->tls_connected = false;
    tls->listening = false;
    tls->reading = false;
    tls->handshaking = false;
    tls->shutting_down = false;

    return 0;
}

/* 创建 TLS 句柄 */
vox_tls_t* vox_tls_create(vox_loop_t* loop, vox_ssl_context_t* ssl_ctx) {
    if (!loop) {
        return NULL;
    }

    vox_mpool_t* mpool = vox_loop_get_mpool(loop);
    vox_tls_t* tls = (vox_tls_t*)vox_mpool_alloc(mpool, sizeof(vox_tls_t));
    if (!tls) {
        return NULL;
    }

    if (vox_tls_init(tls, loop, ssl_ctx) != 0) {
        vox_mpool_free(mpool, tls);
        return NULL;
    }

    return tls;
}

/* 销毁 TLS 句柄 */
void vox_tls_destroy(vox_tls_t* tls) {
    if (!tls) {
        return;
    }

    /* 停止读取 */
    if (tls->reading) {
        vox_tls_read_stop(tls);
    }

    /* 销毁 SSL Session */
    if (tls->ssl_session) {
        vox_ssl_session_destroy(tls->ssl_session);
        tls->ssl_session = NULL;
    }

    /* 销毁底层 TCP 句柄 */
    if (tls->tcp) {
        /* 清除 user_data 以防止回调访问正在销毁的 TLS 句柄 */
        vox_handle_set_data((vox_handle_t*)tls->tcp, NULL);
        vox_tcp_destroy(tls->tcp);
        tls->tcp = NULL;
    }

    /* 释放读取缓冲区 */
    if (tls->read_buf) {
        vox_mpool_t* mpool = vox_loop_get_mpool(tls->handle.loop);
        vox_mpool_free(mpool, tls->read_buf);
        tls->read_buf = NULL;
        tls->read_buf_size = 0;
    }

    /* 释放 BIO 缓冲区 */
    vox_mpool_t* mpool = vox_loop_get_mpool(tls->handle.loop);
    if (tls->rbio_buf) {
        vox_mpool_free(mpool, tls->rbio_buf);
        tls->rbio_buf = NULL;
        tls->rbio_buf_size = 0;
    }
    if (tls->wbio_buf) {
        vox_mpool_free(mpool, tls->wbio_buf);
        tls->wbio_buf = NULL;
        tls->wbio_buf_size = 0;
    }

    /* Clean up write queue without invoking callbacks.
     * Applications should ensure operations complete before destroying handles.
     * Invoking callbacks during destroy can cause use-after-free issues. */
    if (tls->write_queue) {
        vox_tls_write_req_t* req = (vox_tls_write_req_t*)tls->write_queue;
        while (req) {
            vox_tls_write_req_t* next = req->next;
            /* Don't invoke callbacks during destroy - just free resources */
            /* 释放复制的缓冲区 */
            if (req->buf) {
                vox_mpool_free(mpool, req->buf);
            }
            /* 释放请求结构 */
            vox_mpool_free(mpool, req);
            req = next;
        }
        tls->write_queue = NULL;
        tls->write_queue_tail = NULL;
    }

    /* 销毁 SSL Context（如果是自己创建的） */
    /* 注意：如果 ssl_ctx 是外部提供的，不应该在这里销毁 */
    /* 这里我们假设如果 ssl_ctx 是外部提供的，调用者会负责销毁 */
}

/* 绑定地址 */
int vox_tls_bind(vox_tls_t* tls, const vox_socket_addr_t* addr, unsigned int flags) {
    if (!tls || !tls->tcp || !addr) {
        return -1;
    }

    return vox_tcp_bind(tls->tcp, addr, flags);
}

/* 开始监听 */
int vox_tls_listen(vox_tls_t* tls, int backlog, vox_tls_connection_cb cb) {
    if (!tls || !tls->tcp) {
        return -1;
    }

    tls->connection_cb = cb;
    tls->listening = true;

    return vox_tcp_listen(tls->tcp, backlog, tls_tcp_connection_cb);
}

/* 接受连接 */
int vox_tls_accept(vox_tls_t* server, vox_tls_t* client) {
    if (!server || !client || !server->tcp || !client->tcp) {
        return -1;
    }

    if (vox_tcp_accept(server->tcp, client->tcp) != 0) {
        return -1;
    }

    client->connected = true;

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
int vox_tls_connect(vox_tls_t* tls, const vox_socket_addr_t* addr, vox_tls_connect_cb cb) {
    if (!tls || !tls->tcp || !addr) {
        return -1;
    }

    tls->connect_cb = cb;

    return vox_tcp_connect(tls->tcp, addr, tls_tcp_connect_cb);
}

/* 开始 TLS 握手 */
int vox_tls_handshake(vox_tls_t* tls, vox_tls_handshake_cb cb) {
    if (!tls || !tls->ssl_session) {
        return -1;
    }

    if (tls->handshaking) {
        return 0;  /* 已经在握手 */
    }

    tls->handshaking = true;
    tls->handshake_cb = cb;

    /* 开始读取（用于接收握手数据） */
    if (!tls->tcp->reading) {
        if (vox_tcp_read_start(tls->tcp, NULL, tls_tcp_read_cb) != 0) {
            VOX_LOG_ERROR("Failed to start TCP read for TLS handshake");
            tls->handshaking = false;
            return -1;
        }
    }

    /* 尝试开始握手 */
    int ret = vox_ssl_session_handshake(tls->ssl_session);
    if (ret == 0) {
        /* 握手立即成功 */
        tls->handshaking = false;
        tls->tls_connected = true;
        /* 处理 wbio 数据（发送握手消息） */
        tls_process_wbio_data(tls);
        /* 处理写队列以确保握手消息可以发送 */
        tls_process_write_queue(tls);
        if (cb) {
            cb(tls, 0, vox_handle_get_data((vox_handle_t*)tls));
        }
    } else if (ret == VOX_SSL_ERROR_WANT_READ || ret == VOX_SSL_ERROR_WANT_WRITE) {
        /* 需要更多数据，这是正常的 */
        /* 处理 wbio 数据（发送握手消息到对端） */
        tls_process_wbio_data(tls);
        /* 处理写队列以确保握手消息可以发送 */
        tls_process_write_queue(tls);
        /* 握手会继续，等待更多数据 */
    } else {
        /* 握手失败 */
        char err_buf[256];
        vox_ssl_session_get_error_string(tls->ssl_session, err_buf, sizeof(err_buf));
        VOX_LOG_ERROR("TLS handshake failed: ret=%d, error=%s", ret, err_buf);
        tls->handshaking = false;
        if (cb) {
            cb(tls, -1, vox_handle_get_data((vox_handle_t*)tls));
        }
        return -1;
    }

    return 0;
}

/* 开始异步读取 */
int vox_tls_read_start(vox_tls_t* tls, vox_tls_alloc_cb alloc_cb, vox_tls_read_cb read_cb) {
    if (!tls || !tls->tcp) {
        return -1;
    }

    if (tls->tcp->socket.fd == VOX_INVALID_SOCKET) {
        return -1;
    }

    if (!tls->tls_connected) {
        return -1;  /* TLS 未连接 */
    }

    if (tls->reading) {
        return 0;  /* 已经在读取 */
    }

    tls->reading = true;
    tls->alloc_cb = alloc_cb;
    tls->read_cb = read_cb;

    /* 开始 TCP 读取（如果还没有开始） */
    if (!tls->tcp->reading) {
        if (vox_tcp_read_start(tls->tcp, NULL, tls_tcp_read_cb) != 0) {
            tls->reading = false;
            return -1;
        }
    }

    /* 尝试读取已有数据 */
    tls_process_rbio_data(tls);

    return 0;
}

/* 停止异步读取 */
int vox_tls_read_stop(vox_tls_t* tls) {
    if (!tls || !tls->tcp) {
        return -1;
    }

    if (!tls->reading) {
        return 0;
    }

    tls->reading = false;
    tls->read_cb = NULL;
    tls->alloc_cb = NULL;

    return 0;
}

/* 异步写入 */
int vox_tls_write(vox_tls_t* tls, const void* buf, size_t len, vox_tls_write_cb cb) {
    if (!tls || !buf || len == 0) {
        return -1;
    }

    /* 检查 TCP 句柄是否有效 */
    if (!tls->tcp) {
        return -1;
    }

    if (tls->tcp->socket.fd == VOX_INVALID_SOCKET) {
        return -1;
    }

    if (!tls->tls_connected) {
        return -1;  /* TLS 未连接 */
    }

    vox_mpool_t* mpool = vox_loop_get_mpool(tls->handle.loop);

    /* 如果有待处理的写入请求，直接加入队列 */
    if (tls->write_queue) {
        /* 创建新的写入请求 */
        vox_tls_write_req_t* req = (vox_tls_write_req_t*)vox_mpool_alloc(
            mpool, sizeof(vox_tls_write_req_t));
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
        req->cb = cb;
        req->next = NULL;

        /* 添加到队列末尾（使用尾指针，O(1) 操作） */
        vox_tls_write_req_t* old_tail = (vox_tls_write_req_t*)tls->write_queue_tail;
        if (old_tail) {
            old_tail->next = req;
        } else {
            /* 队列为空，这不应该发生（因为已经检查了 write_queue） */
            tls->write_queue = (void*)req;
        }
        tls->write_queue_tail = (void*)req;

        /* TCP socket会自动处理可写事件，无需手动注册 */

        return 0;
    }

    /* 尝试立即写入 */
    ssize_t nwritten = vox_ssl_session_write(tls->ssl_session, buf, len);

    if (nwritten < 0) {
        if (nwritten == VOX_SSL_ERROR_WANT_WRITE) {
            /* 需要写入更多数据，处理 wbio 数据（除非正在进行握手） */
            if (!tls->handshaking) {
                tls_process_wbio_data(tls);
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
    if (!tls->handshaking) {
        tls_process_wbio_data(tls);
    }

    if (nwritten >= 0 && (size_t)nwritten == len) {
        /* 全部写入完成 */
        if (cb) {
            cb(tls, 0, vox_handle_get_data((vox_handle_t*)tls));
        }
        return 0;
    }

    /* 部分写入或需要等待，创建写入请求并加入队列 */
    vox_tls_write_req_t* req = (vox_tls_write_req_t*)vox_mpool_alloc(
        mpool, sizeof(vox_tls_write_req_t));
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
    req->cb = cb;
    req->next = NULL;

    /* 添加到队列（使用尾指针，O(1) 操作） */
    vox_tls_write_req_t* old_tail = (vox_tls_write_req_t*)tls->write_queue_tail;
    if (old_tail) {
        old_tail->next = req;
    } else {
        /* 队列为空 */
        tls->write_queue = (void*)req;
    }
    tls->write_queue_tail = (void*)req;

    /* TCP层会自动检测写队列并触发tls_tcp_write_cb回调 */

    return 0;
}

/* 关闭写入端 */
int vox_tls_shutdown(vox_tls_t* tls, vox_tls_shutdown_cb cb) {
    if (!tls || !tls->ssl_session) {
        return -1;
    }

    tls->shutdown_cb = cb;
    tls->shutting_down = true;

    /* 执行 SSL shutdown */
    int ret = vox_ssl_session_shutdown(tls->ssl_session);
    if (ret == 0) {
        /* 关闭完成 */
        tls->shutting_down = false;
        if (cb) {
            cb(tls, 0, vox_handle_get_data((vox_handle_t*)tls));
        }
        tls->shutdown_cb = NULL;
        return 0;
    } else if (ret == VOX_SSL_ERROR_WANT_READ || ret == VOX_SSL_ERROR_WANT_WRITE) {
        /* 需要更多数据来完成关闭 */
        /* 处理 wbio 数据 */
        tls_process_wbio_data(tls);
        /* 关闭是异步的，回调会在完成后调用 */
        return 0;
    } else {
        /* 关闭失败 */
        tls->shutting_down = false;
        if (cb) {
            cb(tls, -1, vox_handle_get_data((vox_handle_t*)tls));
        }
        tls->shutdown_cb = NULL;
        return -1;
    }
}

/* 获取本地地址 */
int vox_tls_getsockname(vox_tls_t* tls, vox_socket_addr_t* addr) {
    if (!tls || !tls->tcp || !addr) {
        return -1;
    }

    return vox_tcp_getsockname(tls->tcp, addr);
}

/* 获取对端地址 */
int vox_tls_getpeername(vox_tls_t* tls, vox_socket_addr_t* addr) {
    if (!tls || !tls->tcp || !addr) {
        return -1;
    }

    return vox_tcp_getpeername(tls->tcp, addr);
}

/* 设置 TCP 无延迟 */
int vox_tls_nodelay(vox_tls_t* tls, bool enable) {
    if (!tls || !tls->tcp) {
        return -1;
    }

    return vox_tcp_nodelay(tls->tcp, enable);
}

/* 设置保持连接 */
int vox_tls_keepalive(vox_tls_t* tls, bool enable) {
    if (!tls || !tls->tcp) {
        return -1;
    }

    return vox_tcp_keepalive(tls->tcp, enable);
}

/* 设置地址重用 */
int vox_tls_reuseaddr(vox_tls_t* tls, bool enable) {
    if (!tls || !tls->tcp) {
        return -1;
    }

    return vox_tcp_reuseaddr(tls->tcp, enable);
}
