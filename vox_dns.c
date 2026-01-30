/*
 * vox_dns.c - 异步DNS解析实现
 * 使用线程池执行阻塞的DNS查询，在事件循环中执行回调
 */

#include "vox_dns.h"
#include "vox_loop.h"
#include "vox_tpool.h"
#include "vox_mpool.h"
#include "vox_timer.h"
#include "vox_log.h"
#include "vox_os.h"
#include "vox_socket.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#ifdef VOX_OS_WINDOWS
#include <ws2tcpip.h>
#else
#include <netdb.h>
#endif

/* 地址转换辅助函数 */
static void convert_from_sockaddr(const struct sockaddr* sa, vox_socket_addr_t* addr) {
    if (sa->sa_family == AF_INET) {
        const struct sockaddr_in* sin = (const struct sockaddr_in*)sa;
        addr->family = VOX_AF_INET;
        addr->u.ipv4.addr = sin->sin_addr.s_addr;
        addr->u.ipv4.port = sin->sin_port;
    } else if (sa->sa_family == AF_INET6) {
        const struct sockaddr_in6* sin6 = (const struct sockaddr_in6*)sa;
        addr->family = VOX_AF_INET6;
        memcpy(addr->u.ipv6.addr, &sin6->sin6_addr, 16);
        addr->u.ipv6.port = sin6->sin6_port;
    }
}

/* getaddrinfo 工作项 */
typedef struct {
    vox_dns_getaddrinfo_t* req;
    char* node;
    char* service;
    int family;
    int result;
    struct addrinfo* res;
} getaddrinfo_work_t;

/* getnameinfo 工作项 */
typedef struct {
    vox_dns_getnameinfo_t* req;
    struct sockaddr_storage addr;
    socklen_t addr_len;
    int flags;
    int result;
    char* hostname;
    char* service;
    size_t hostname_len;
    size_t service_len;
} getnameinfo_work_t;

/* 前向声明 */
static void getaddrinfo_callback_wrapper(vox_loop_t* loop, void* user_data);
static void getnameinfo_callback_wrapper(vox_loop_t* loop, void* user_data);

/* 获取线程池（从事件循环中获取） */
static vox_tpool_t* get_thread_pool(vox_loop_t* loop) {
    if (!loop) {
        return NULL;
    }
    return vox_loop_get_thread_pool(loop);
}

/* getaddrinfo 线程任务 */
static void getaddrinfo_task(void* user_data) {
    getaddrinfo_work_t* work = (getaddrinfo_work_t*)user_data;
    if (!work || !work->req) {
        return;
    }
    
    /* 检查请求是否已取消 */
    if (work->req->handle.closing) {
        work->result = -1;
        return;
    }
    
    /* 准备参数 */
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    
    if (work->family == VOX_AF_INET) {
        hints.ai_family = AF_INET;
    } else if (work->family == VOX_AF_INET6) {
        hints.ai_family = AF_INET6;
    } else {
        hints.ai_family = AF_UNSPEC;
    }
    hints.ai_socktype = SOCK_STREAM;  /* 默认TCP */
    hints.ai_flags = AI_ADDRCONFIG;
    
    /* 执行阻塞的getaddrinfo */
    work->result = getaddrinfo(work->node, work->service, &hints, &work->res);
    
    /* 如果使用 AI_ADDRCONFIG 失败，尝试不使用该标志 */
    if (work->result != 0 && hints.ai_flags == AI_ADDRCONFIG) {
        hints.ai_flags = 0;
        work->result = getaddrinfo(work->node, work->service, &hints, &work->res);
    }
}

/* getaddrinfo 超时回调 */
static void getaddrinfo_timeout_cb(vox_timer_t* timer, void* user_data) {
    VOX_UNUSED(timer);
    vox_dns_getaddrinfo_t* req = (vox_dns_getaddrinfo_t*)user_data;
    if (!req) {
        return;
    }
    
    /* 检查请求是否已经完成（pending为false表示已完成） */
    if (!req->pending || req->handle.closing) {
        return;  /* 请求已完成或已关闭，不需要处理超时 */
    }
    
    /* 停止定时器 */
    if (req->timeout_timer) {
        vox_timer_stop(req->timeout_timer);
    }
    
    /* 标记为超时并取消请求 */
    req->handle.closing = true;
    req->pending = false;
    vox_handle_deactivate((vox_handle_t*)req);
    
    /* 清空结果（超时没有结果） */
    req->addrinfo.count = 0;
    req->addrinfo.addrs = NULL;
    
    /* 在事件循环中调用用户回调，状态为超时 */
    vox_loop_t* loop = req->handle.loop;
    vox_loop_queue_work(loop, (vox_loop_cb)getaddrinfo_callback_wrapper, req);
}

/* getaddrinfo 完成回调（在线程池中调用） */
static void getaddrinfo_complete(void *user_data, int result) {
    VOX_UNUSED(result);
    getaddrinfo_work_t* work = (getaddrinfo_work_t*)user_data;
    if (!work || !work->req) {
        return;
    }
    
    vox_dns_getaddrinfo_t* req = work->req;
    vox_loop_t* loop = req->handle.loop;
    
    /* 停止超时定时器 */
    if (req->timeout_timer && vox_timer_is_active(req->timeout_timer)) {
        vox_timer_stop(req->timeout_timer);
    }
    
    /* 检查请求是否已关闭 */
    if (req->handle.closing) {
        if (work->res) {
            freeaddrinfo(work->res);
        }
        vox_mpool_t* mpool = vox_loop_get_mpool(loop);
        vox_mpool_free(mpool, work->node);
        vox_mpool_free(mpool, work->service);
        vox_mpool_free(mpool, work);
        return;
    }
    
    /* 转换结果 */
    vox_dns_addrinfo_t addrinfo = {0};
    
    /* 记录错误信息（如果有） */
    if (work->result != 0) {
        const char* error_msg = NULL;
#ifdef VOX_OS_WINDOWS
        error_msg = gai_strerrorA(work->result);
#else
        error_msg = gai_strerror(work->result);
#endif
        VOX_LOG_ERROR("getaddrinfo failed for %s:%s: %s (error=%d)", 
                     work->node ? work->node : "NULL",
                     work->service ? work->service : "NULL",
                     error_msg ? error_msg : "unknown error",
                     work->result);
    }
    
    if (work->result == 0 && work->res) {
        /* 计算地址数量 */
        size_t count = 0;
        struct addrinfo* ai = work->res;
        while (ai) {
            if (ai->ai_family == AF_INET || ai->ai_family == AF_INET6) {
                count++;
            }
            ai = ai->ai_next;
        }
        
        if (count > 0) {
            /* 分配地址数组 */
            vox_mpool_t* mpool = vox_loop_get_mpool(loop);
            addrinfo.addrs = (vox_socket_addr_t*)vox_mpool_alloc(mpool, 
                                                                  sizeof(vox_socket_addr_t) * count);
            if (addrinfo.addrs) {
                /* 转换地址 */
                size_t idx = 0;
                ai = work->res;
                while (ai && idx < count) {
                    if (ai->ai_family == AF_INET || ai->ai_family == AF_INET6) {
                        convert_from_sockaddr(ai->ai_addr, &addrinfo.addrs[idx]);
                        idx++;
                    }
                    ai = ai->ai_next;
                }
                addrinfo.count = idx;
            }
        }
        
        /* 释放系统addrinfo */
        freeaddrinfo(work->res);
        work->res = NULL;
    }
    
    /* 保存结果到请求结构 */
    req->addrinfo = addrinfo;
    req->pending = false;
    
    /* 在事件循环中执行回调 */
    vox_loop_queue_work(loop, (vox_loop_cb)getaddrinfo_callback_wrapper, req);
    
    /* 清理工作项 */
    vox_mpool_t* mpool = vox_loop_get_mpool(loop);
    vox_mpool_free(mpool, work->node);
    vox_mpool_free(mpool, work->service);
    vox_mpool_free(mpool, work);
}

/* getaddrinfo 回调包装器 */
static void getaddrinfo_callback_wrapper(vox_loop_t* loop, void* user_data) {
    VOX_UNUSED(loop);
    vox_dns_getaddrinfo_t* req = (vox_dns_getaddrinfo_t*)user_data;
    if (!req) {
        return;
    }
    
    /* 调用用户回调 */
    if (req->cb) {
        int status;
        if (req->handle.closing && req->addrinfo.count == 0) {
            /* 超时情况：请求被取消且没有结果，返回超时错误 */
            status = -1;
        } else {
            /* 正常完成：根据结果判断状态 */
            status = (req->addrinfo.count > 0) ? 0 : -1;
        }
        req->cb(req, status, &req->addrinfo, req->user_data);
    }
    
    /* 如果请求不再活跃，释放地址数组 */
    if (!req->handle.active) {
        if (req->addrinfo.addrs) {
            vox_mpool_t* mpool = vox_loop_get_mpool(loop);
            vox_mpool_free(mpool, req->addrinfo.addrs);
            req->addrinfo.addrs = NULL;
            req->addrinfo.count = 0;
        }
    }
}

/* getnameinfo 线程任务 */
static void getnameinfo_task(void* user_data) {
    getnameinfo_work_t* work = (getnameinfo_work_t*)user_data;
    if (!work || !work->req) {
        return;
    }
    
    /* 检查请求是否已取消 */
    if (work->req->handle.closing) {
        work->result = -1;
        return;
    }
    
    /* 执行阻塞的getnameinfo */
    work->result = getnameinfo((struct sockaddr*)&work->addr, work->addr_len,
                               work->hostname, (socklen_t)work->hostname_len,
                               work->service, (socklen_t)work->service_len,
                               work->flags);
}

/* getnameinfo 完成回调（在线程池中调用） */
static void getnameinfo_complete(void *user_data, int result) {
    VOX_UNUSED(result);
    getnameinfo_work_t* work = (getnameinfo_work_t*)user_data;
    if (!work || !work->req) {
        return;
    }
    
    vox_dns_getnameinfo_t* req = work->req;
    vox_loop_t* loop = req->handle.loop;
    
    /* 检查请求是否已关闭 */
    if (req->handle.closing) {
        vox_mpool_t* mpool = vox_loop_get_mpool(loop);
        vox_mpool_free(mpool, work);
        return;
    }
    
    /* 保存结果（hostname和service已经写入用户提供的缓冲区） */
    req->pending = false;
    
    /* 在事件循环中执行回调 */
    vox_loop_queue_work(loop, (vox_loop_cb)getnameinfo_callback_wrapper, req);
    
    /* 清理工作项（注意：hostname和service是用户提供的缓冲区，不要释放） */
    vox_mpool_t* mpool = vox_loop_get_mpool(loop);
    vox_mpool_free(mpool, work);
}

/* getnameinfo 回调包装器 */
static void getnameinfo_callback_wrapper(vox_loop_t* loop, void* user_data) {
    VOX_UNUSED(loop);
    vox_dns_getnameinfo_t* req = (vox_dns_getnameinfo_t*)user_data;
    if (!req || req->handle.closing) {
        return;
    }
    
    /* 调用用户回调 */
    if (req->cb) {
        int status = (req->hostname && req->service) ? 0 : -1;
        req->cb(req, status, req->hostname, req->service, req->user_data);
    }
}

/* ===== getaddrinfo API ===== */

int vox_dns_getaddrinfo_init(vox_dns_getaddrinfo_t* req, vox_loop_t* loop) {
    if (!req || !loop) {
        return -1;
    }
    
    memset(req, 0, sizeof(vox_dns_getaddrinfo_t));
    
    /* 初始化句柄基类 */
    if (vox_handle_init((vox_handle_t*)req, VOX_HANDLE_DNS, loop) != 0) {
        return -1;
    }
    
    req->pending = false;
    req->timeout_ms = 5000;  /* 默认5秒超时 */
    
    /* 创建超时定时器 */
    vox_mpool_t* mpool = vox_loop_get_mpool(loop);
    req->timeout_timer = (vox_timer_t*)vox_mpool_alloc(mpool, sizeof(vox_timer_t));
    if (req->timeout_timer) {
        if (vox_timer_init(req->timeout_timer, loop) != 0) {
            vox_mpool_free(mpool, req->timeout_timer);
            req->timeout_timer = NULL;
        }
    }
    
    return 0;
}

vox_dns_getaddrinfo_t* vox_dns_getaddrinfo_create(vox_loop_t* loop) {
    if (!loop) {
        return NULL;
    }
    
    vox_mpool_t* mpool = vox_loop_get_mpool(loop);
    vox_dns_getaddrinfo_t* req = (vox_dns_getaddrinfo_t*)vox_mpool_alloc(mpool, 
                                                                         sizeof(vox_dns_getaddrinfo_t));
    if (!req) {
        return NULL;
    }
    
    if (vox_dns_getaddrinfo_init(req, loop) != 0) {
        vox_mpool_free(mpool, req);
        return NULL;
    }
    
    return req;
}

void vox_dns_getaddrinfo_destroy(vox_dns_getaddrinfo_t* req) {
    if (!req) {
        return;
    }
    
    /* 停止超时定时器 */
    if (req->timeout_timer) {
        if (vox_timer_is_active(req->timeout_timer)) {
            vox_timer_stop(req->timeout_timer);
        }
        vox_mpool_t* mpool = vox_loop_get_mpool(req->handle.loop);
        vox_mpool_free(mpool, req->timeout_timer);
        req->timeout_timer = NULL;
    }
    
    /* 取消正在进行的请求 */
    if (req->pending) {
        vox_dns_getaddrinfo_cancel(req);
    }
    
    /* 释放地址数组 */
    if (req->addrinfo.addrs) {
        vox_mpool_t* mpool = vox_loop_get_mpool(req->handle.loop);
        vox_mpool_free(mpool, req->addrinfo.addrs);
        req->addrinfo.addrs = NULL;
        req->addrinfo.count = 0;
    }
    
    /* 释放字符串 */
    vox_mpool_t* mpool = vox_loop_get_mpool(req->handle.loop);
    if (req->node) {
        vox_mpool_free(mpool, req->node);
        req->node = NULL;
    }
    if (req->service) {
        vox_mpool_free(mpool, req->service);
        req->service = NULL;
    }
    
    /* 关闭句柄 */
    vox_handle_close((vox_handle_t*)req, NULL);
}

int vox_dns_getaddrinfo(vox_dns_getaddrinfo_t* req,
                        const char* node,
                        const char* service,
                        vox_address_family_t family,
                        vox_dns_getaddrinfo_cb cb,
                        void* user_data,
                        uint64_t timeout_ms) {
    if (!req || !cb) {
        return -1;
    }
    
    if (req->pending) {
        VOX_LOG_ERROR("DNS请求已在进行中");
        return -1;
    }
    
    vox_loop_t* loop = req->handle.loop;
    vox_mpool_t* mpool = vox_loop_get_mpool(loop);
    
    /* 设置超时时间（0表示使用默认值5000毫秒） */
    req->timeout_ms = (timeout_ms == 0) ? 5000 : timeout_ms;
    
    /* 复制字符串 */
    char* node_copy = NULL;
    char* service_copy = NULL;
    
    if (node) {
        size_t node_len = strlen(node) + 1;
        node_copy = (char*)vox_mpool_alloc(mpool, node_len);
        if (!node_copy) {
            return -1;
        }
        memcpy(node_copy, node, node_len);
    }
    
    if (service) {
        size_t service_len = strlen(service) + 1;
        service_copy = (char*)vox_mpool_alloc(mpool, service_len);
        if (!service_copy) {
            if (node_copy) {
                vox_mpool_free(mpool, node_copy);
            }
            return -1;
        }
        memcpy(service_copy, service, service_len);
    }
    
    /* 保存参数 */
    req->node = node_copy;
    req->service = service_copy;
    req->family = family;
    req->cb = cb;
    req->user_data = user_data;
    req->pending = true;
    
    /* 激活句柄 */
    vox_handle_activate((vox_handle_t*)req);
    
    /* 创建工作项 */
    getaddrinfo_work_t* work = (getaddrinfo_work_t*)vox_mpool_alloc(mpool, sizeof(getaddrinfo_work_t));
    if (!work) {
        req->pending = false;
        vox_handle_deactivate((vox_handle_t*)req);
        if (node_copy) {
            vox_mpool_free(mpool, node_copy);
        }
        if (service_copy) {
            vox_mpool_free(mpool, service_copy);
        }
        return -1;
    }
    
    memset(work, 0, sizeof(getaddrinfo_work_t));
    work->req = req;
    work->node = node_copy;
    work->service = service_copy;
    work->family = (family == VOX_AF_INET) ? AF_INET : 
                   (family == VOX_AF_INET6) ? AF_INET6 : AF_UNSPEC;
    
    /* 获取线程池 */
    vox_tpool_t* tpool = get_thread_pool(loop);
    if (!tpool) {
        req->pending = false;
        vox_handle_deactivate((vox_handle_t*)req);
        vox_mpool_free(mpool, work);
        if (node_copy) {
            vox_mpool_free(mpool, node_copy);
        }
        if (service_copy) {
            vox_mpool_free(mpool, service_copy);
        }
        return -1;
    }
    
    /* 提交任务到线程池 */
    if (vox_tpool_submit(tpool, getaddrinfo_task, work, getaddrinfo_complete) != 0) {
        req->pending = false;
        vox_handle_deactivate((vox_handle_t*)req);
        vox_mpool_free(mpool, work);
        if (node_copy) {
            vox_mpool_free(mpool, node_copy);
        }
        if (service_copy) {
            vox_mpool_free(mpool, service_copy);
        }
        return -1;
    }
    
    /* 启动超时定时器（如果设置了超时且定时器可用） */
    if (req->timeout_ms > 0 && req->timeout_timer) {
        if (vox_timer_start(req->timeout_timer, req->timeout_ms, 0, 
                           getaddrinfo_timeout_cb, req) != 0) {
            /* 定时器启动失败不影响DNS解析，继续执行 */
            VOX_LOG_ERROR("DNS超时定时器启动失败");
        }
    }
    
    return 0;
}

int vox_dns_getaddrinfo_cancel(vox_dns_getaddrinfo_t* req) {
    if (!req) {
        return -1;
    }
    
    if (!req->pending) {
        return 0;  /* 没有正在进行的请求 */
    }
    
    /* 标记为关闭，线程任务会检查这个标志 */
    req->handle.closing = true;
    req->pending = false;
    
    /* 停用句柄 */
    vox_handle_deactivate((vox_handle_t*)req);
    
    return 0;
}

void vox_dns_freeaddrinfo(vox_dns_addrinfo_t* addrinfo) {
    if (!addrinfo || !addrinfo->addrs) {
        return;
    }
    
    /* 注意：这个函数只是将指针设置为NULL，实际的内存释放由请求对象管理
     * 在 getaddrinfo_callback_wrapper 中，如果请求不再活跃，会自动释放地址数组
     * 或者在 vox_dns_getaddrinfo_destroy 中释放
     * 这里只是标记内存可以被释放，避免重复释放
     */
    addrinfo->addrs = NULL;
    addrinfo->count = 0;
}

/* ===== getnameinfo API ===== */

int vox_dns_getnameinfo_init(vox_dns_getnameinfo_t* req, vox_loop_t* loop) {
    if (!req || !loop) {
        return -1;
    }
    
    memset(req, 0, sizeof(vox_dns_getnameinfo_t));
    
    /* 初始化句柄基类 */
    if (vox_handle_init((vox_handle_t*)req, VOX_HANDLE_DNS, loop) != 0) {
        return -1;
    }
    
    req->pending = false;
    return 0;
}

vox_dns_getnameinfo_t* vox_dns_getnameinfo_create(vox_loop_t* loop) {
    if (!loop) {
        return NULL;
    }
    
    vox_mpool_t* mpool = vox_loop_get_mpool(loop);
    vox_dns_getnameinfo_t* req = (vox_dns_getnameinfo_t*)vox_mpool_alloc(mpool, 
                                                                          sizeof(vox_dns_getnameinfo_t));
    if (!req) {
        return NULL;
    }
    
    if (vox_dns_getnameinfo_init(req, loop) != 0) {
        vox_mpool_free(mpool, req);
        return NULL;
    }
    
    return req;
}

void vox_dns_getnameinfo_destroy(vox_dns_getnameinfo_t* req) {
    if (!req) {
        return;
    }
    
    /* 取消正在进行的请求 */
    if (req->pending) {
        vox_dns_getnameinfo_cancel(req);
    }
    
    /* 释放字符串 */
    vox_mpool_t* mpool = vox_loop_get_mpool(req->handle.loop);
    if (req->hostname) {
        vox_mpool_free(mpool, req->hostname);
        req->hostname = NULL;
    }
    if (req->service) {
        vox_mpool_free(mpool, req->service);
        req->service = NULL;
    }
    
    /* 关闭句柄 */
    vox_handle_close((vox_handle_t*)req, NULL);
}

int vox_dns_getnameinfo(vox_dns_getnameinfo_t* req,
                        const vox_socket_addr_t* addr,
                        int flags,
                        char* hostname_buf,
                        size_t hostname_len,
                        char* service_buf,
                        size_t service_len,
                        vox_dns_getnameinfo_cb cb,
                        void* user_data) {
    if (!req || !addr || !cb) {
        return -1;
    }
    
    if (!hostname_buf || hostname_len == 0 || !service_buf || service_len == 0) {
        return -1;
    }
    
    if (req->pending) {
        VOX_LOG_ERROR("DNS请求已在进行中");
        return -1;
    }
    
    vox_loop_t* loop = req->handle.loop;
    vox_mpool_t* mpool = vox_loop_get_mpool(loop);
    
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
    } else if (addr->family == VOX_AF_INET6) {
        struct sockaddr_in6* sin6 = (struct sockaddr_in6*)&sa;
        memset(sin6, 0, sizeof(*sin6));
        sin6->sin6_family = AF_INET6;
        memcpy(&sin6->sin6_addr, addr->u.ipv6.addr, 16);
        sin6->sin6_port = addr->u.ipv6.port;
        sa_len = sizeof(*sin6);
    } else {
        return -1;
    }
    
    /* 保存参数 */
    req->addr = *addr;
    req->flags = flags;
    req->hostname = hostname_buf;
    req->service = service_buf;
    req->hostname_len = hostname_len;
    req->service_len = service_len;
    req->cb = cb;
    req->user_data = user_data;
    req->pending = true;
    
    /* 激活句柄 */
    vox_handle_activate((vox_handle_t*)req);
    
    /* 创建工作项 */
    getnameinfo_work_t* work = (getnameinfo_work_t*)vox_mpool_alloc(mpool, sizeof(getnameinfo_work_t));
    if (!work) {
        req->pending = false;
        vox_handle_deactivate((vox_handle_t*)req);
        return -1;
    }
    
    memset(work, 0, sizeof(getnameinfo_work_t));
    work->req = req;
    memcpy(&work->addr, &sa, sa_len);
    work->addr_len = sa_len;
    work->flags = flags;
    work->hostname = hostname_buf;
    work->service = service_buf;
    work->hostname_len = hostname_len;
    work->service_len = service_len;
    
    /* 获取线程池 */
    vox_tpool_t* tpool = get_thread_pool(loop);
    if (!tpool) {
        req->pending = false;
        vox_handle_deactivate((vox_handle_t*)req);
        vox_mpool_free(mpool, work);
        return -1;
    }
    
    /* 提交任务到线程池 */
    if (vox_tpool_submit(tpool, getnameinfo_task, work, getnameinfo_complete) != 0) {
        req->pending = false;
        vox_handle_deactivate((vox_handle_t*)req);
        vox_mpool_free(mpool, work);
        return -1;
    }
    
    return 0;
}

int vox_dns_getnameinfo_cancel(vox_dns_getnameinfo_t* req) {
    if (!req) {
        return -1;
    }
    
    if (!req->pending) {
        return 0;  /* 没有正在进行的请求 */
    }
    
    /* 标记为关闭，线程任务会检查这个标志 */
    req->handle.closing = true;
    req->pending = false;
    
    /* 停用句柄 */
    vox_handle_deactivate((vox_handle_t*)req);
    
    return 0;
}

/* ===== 便捷函数实现 ===== */

/* getaddrinfo 简化回调包装器 */
static void getaddrinfo_simple_callback_wrapper(vox_loop_t* loop, void* user_data) {
    typedef struct {
        vox_dns_getaddrinfo_t* req;
        vox_dns_getaddrinfo_simple_cb cb;
        void* user_data;
    } simple_wrapper_t;
    
    simple_wrapper_t* wrapper = (simple_wrapper_t*)user_data;
    if (!wrapper || !wrapper->req) {
        return;
    }
    
    vox_dns_getaddrinfo_t* req = wrapper->req;
    
    /* 调用简化回调 */
    if (wrapper->cb) {
        int status = (req->addrinfo.count > 0) ? 0 : -1;
        wrapper->cb(status, &req->addrinfo, wrapper->user_data);
    }
    
    /* 释放请求对象 */
    vox_dns_getaddrinfo_destroy(req);
    
    /* 释放包装器 */
    vox_mpool_t* mpool = vox_loop_get_mpool(loop);
    vox_mpool_free(mpool, wrapper);
}

/* getaddrinfo 回调包装器（用于便捷函数） */
static void getaddrinfo_simple_wrapper(vox_dns_getaddrinfo_t* req, int status,
                                       const vox_dns_addrinfo_t* addrinfo,
                                       void* user_data) {
    (void)status;
    (void)addrinfo;
    
    typedef struct {
        vox_dns_getaddrinfo_simple_cb cb;
        void* user_data;
    } simple_wrapper_t;
    
    simple_wrapper_t* wrapper = (simple_wrapper_t*)user_data;
    if (!wrapper) {
        return;
    }
    
    /* 在事件循环中执行简化回调 */
    vox_loop_t* loop = req->handle.loop;
    typedef struct {
        vox_dns_getaddrinfo_t* req;
        vox_dns_getaddrinfo_simple_cb cb;
        void* user_data;
    } callback_data_t;
    
    callback_data_t* cb_data = (callback_data_t*)vox_mpool_alloc(
        vox_loop_get_mpool(loop), sizeof(callback_data_t));
    if (!cb_data) {
        return;
    }
    
    cb_data->req = req;
    cb_data->cb = wrapper->cb;
    cb_data->user_data = wrapper->user_data;
    
    vox_loop_queue_work(loop, getaddrinfo_simple_callback_wrapper, cb_data);
}

int vox_dns_getaddrinfo_simple(vox_loop_t* loop,
                               const char* node,
                               const char* service,
                               vox_address_family_t family,
                               vox_dns_getaddrinfo_simple_cb cb,
                               void* user_data,
                               uint64_t timeout_ms) {
    if (!loop || !cb) {
        return -1;
    }
    
    /* 创建请求对象 */
    vox_dns_getaddrinfo_t* req = vox_dns_getaddrinfo_create(loop);
    if (!req) {
        return -1;
    }
    
    /* 创建包装器 */
    vox_mpool_t* mpool = vox_loop_get_mpool(loop);
    typedef struct {
        vox_dns_getaddrinfo_simple_cb cb;
        void* user_data;
    } simple_wrapper_t;
    
    simple_wrapper_t* wrapper = (simple_wrapper_t*)vox_mpool_alloc(mpool, sizeof(simple_wrapper_t));
    if (!wrapper) {
        vox_dns_getaddrinfo_destroy(req);
        return -1;
    }
    
    wrapper->cb = cb;
    wrapper->user_data = user_data;
    
    /* 调用标准函数 */
    if (vox_dns_getaddrinfo(req, node, service, family, 
                            getaddrinfo_simple_wrapper, wrapper, timeout_ms) != 0) {
        vox_mpool_free(mpool, wrapper);
        vox_dns_getaddrinfo_destroy(req);
        return -1;
    }
    
    return 0;
}

/* getnameinfo 简化回调包装器 */
static void getnameinfo_simple_callback_wrapper(vox_loop_t* loop, void* user_data) {
    typedef struct {
        vox_dns_getnameinfo_t* req;
        vox_dns_getnameinfo_simple_cb cb;
        void* user_data;
    } simple_wrapper_t;
    
    simple_wrapper_t* wrapper = (simple_wrapper_t*)user_data;
    if (!wrapper || !wrapper->req) {
        return;
    }
    
    vox_dns_getnameinfo_t* req = wrapper->req;
    
    /* 调用简化回调 */
    if (wrapper->cb) {
        int status = (req->hostname && req->service) ? 0 : -1;
        wrapper->cb(status, req->hostname, req->service, wrapper->user_data);
    }
    
    /* 释放请求对象 */
    vox_dns_getnameinfo_destroy(req);
    
    /* 释放包装器 */
    vox_mpool_t* mpool = vox_loop_get_mpool(loop);
    vox_mpool_free(mpool, wrapper);
}

/* getnameinfo 回调包装器（用于便捷函数） */
static void getnameinfo_simple_wrapper(vox_dns_getnameinfo_t* req, int status,
                                       const char* hostname,
                                       const char* service,
                                       void* user_data) {
    (void)status;
    (void)hostname;
    (void)service;
    
    typedef struct {
        vox_dns_getnameinfo_simple_cb cb;
        void* user_data;
    } simple_wrapper_t;
    
    simple_wrapper_t* wrapper = (simple_wrapper_t*)user_data;
    if (!wrapper) {
        return;
    }
    
    /* 在事件循环中执行简化回调 */
    vox_loop_t* loop = req->handle.loop;
    typedef struct {
        vox_dns_getnameinfo_t* req;
        vox_dns_getnameinfo_simple_cb cb;
        void* user_data;
    } callback_data_t;
    
    callback_data_t* cb_data = (callback_data_t*)vox_mpool_alloc(
        vox_loop_get_mpool(loop), sizeof(callback_data_t));
    if (!cb_data) {
        return;
    }
    
    cb_data->req = req;
    cb_data->cb = wrapper->cb;
    cb_data->user_data = wrapper->user_data;
    
    vox_loop_queue_work(loop, getnameinfo_simple_callback_wrapper, cb_data);
}

int vox_dns_getnameinfo_simple(vox_loop_t* loop,
                                const vox_socket_addr_t* addr,
                                int flags,
                                char* hostname_buf,
                                size_t hostname_len,
                                char* service_buf,
                                size_t service_len,
                                vox_dns_getnameinfo_simple_cb cb,
                                void* user_data) {
    if (!loop || !cb) {
        return -1;
    }
    
    /* 创建请求对象 */
    vox_dns_getnameinfo_t* req = vox_dns_getnameinfo_create(loop);
    if (!req) {
        return -1;
    }
    
    /* 创建包装器 */
    vox_mpool_t* mpool = vox_loop_get_mpool(loop);
    typedef struct {
        vox_dns_getnameinfo_simple_cb cb;
        void* user_data;
    } simple_wrapper_t;
    
    simple_wrapper_t* wrapper = (simple_wrapper_t*)vox_mpool_alloc(mpool, sizeof(simple_wrapper_t));
    if (!wrapper) {
        vox_dns_getnameinfo_destroy(req);
        return -1;
    }
    
    wrapper->cb = cb;
    wrapper->user_data = user_data;
    
    /* 调用标准函数 */
    if (vox_dns_getnameinfo(req, addr, flags, hostname_buf, hostname_len,
                            service_buf, service_len,
                            getnameinfo_simple_wrapper, wrapper) != 0) {
        vox_mpool_free(mpool, wrapper);
        vox_dns_getnameinfo_destroy(req);
        return -1;
    }
    
    return 0;
}
