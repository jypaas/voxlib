/*
 * vox_coroutine_dns.c - DNS协程适配器实现
 */

#include "vox_coroutine_dns.h"
#include "vox_coroutine_promise.h"
#include "../vox_log.h"
#include "../vox_mpool.h"

#include <string.h>

/* ===== getaddrinfo操作的协程适配 ===== */

typedef struct {
    vox_coroutine_promise_t* promise;
    vox_dns_addrinfo_t* out_addrinfo;
    vox_mpool_t* mpool;
} dns_getaddrinfo_await_data_t;

static void dns_getaddrinfo_await_cb(int status,
                                     const vox_dns_addrinfo_t* addrinfo,
                                     void* user_data) {
    dns_getaddrinfo_await_data_t* data = (dns_getaddrinfo_await_data_t*)user_data;
    if (!data || !data->promise) return;
    
    if (status == 0 && addrinfo && addrinfo->count > 0 && data->out_addrinfo) {
        /* 深拷贝地址数组 */
        vox_mpool_t* mpool = data->mpool;
        size_t count = addrinfo->count;
        
        vox_socket_addr_t* addrs_copy = (vox_socket_addr_t*)vox_mpool_alloc(
            mpool, sizeof(vox_socket_addr_t) * count);
        if (addrs_copy) {
            memcpy(addrs_copy, addrinfo->addrs, sizeof(vox_socket_addr_t) * count);
            data->out_addrinfo->addrs = addrs_copy;
            data->out_addrinfo->count = count;
        } else {
            /* 内存分配失败 */
            data->out_addrinfo->addrs = NULL;
            data->out_addrinfo->count = 0;
            status = -1;
        }
    } else if (data->out_addrinfo) {
        data->out_addrinfo->addrs = NULL;
        data->out_addrinfo->count = 0;
    }
    
    vox_coroutine_promise_complete(data->promise, status, NULL);
}

int vox_coroutine_dns_getaddrinfo_await(vox_coroutine_t* co,
                                        const char* node,
                                        const char* service,
                                        vox_address_family_t family,
                                        vox_dns_addrinfo_t* out_addrinfo) {
    if (!co || !node) return -1;
    
    vox_loop_t* loop = vox_coroutine_get_loop(co);
    if (!loop) return -1;
    
    /* 创建Promise */
    vox_coroutine_promise_t* promise = vox_coroutine_promise_create(loop);
    if (!promise) return -1;
    
    /* 创建回调数据（从loop的内存池分配，会在loop销毁时自动释放） */
    vox_mpool_t* mpool = vox_loop_get_mpool(loop);
    dns_getaddrinfo_await_data_t* data = (dns_getaddrinfo_await_data_t*)vox_mpool_alloc(
        mpool, sizeof(dns_getaddrinfo_await_data_t));
    if (!data) {
        vox_coroutine_promise_destroy(promise);
        return -1;
    }
    memset(data, 0, sizeof(*data));
    data->promise = promise;
    data->out_addrinfo = out_addrinfo;
    data->mpool = mpool;
    
    if (out_addrinfo) {
        out_addrinfo->addrs = NULL;
        out_addrinfo->count = 0;
    }
    
    /* 调用异步getaddrinfo */
    if (vox_dns_getaddrinfo_simple(loop, node, service, family,
                                   dns_getaddrinfo_await_cb, data, 5000) != 0) {
        vox_mpool_free(mpool, data);
        vox_coroutine_promise_destroy(promise);
        return -1;
    }
    
    /* 等待Promise完成 */
    int status = vox_coroutine_await(co, promise);
    
    /* 清理Promise（data从loop的内存池分配，会在loop销毁时自动释放） */
    vox_coroutine_promise_destroy(promise);
    
    return status;
}

/* ===== getnameinfo操作的协程适配 ===== */

typedef struct {
    vox_coroutine_promise_t* promise;
    char* hostname_buf;
    size_t hostname_len;
    char* service_buf;
    size_t service_len;
    vox_mpool_t* mpool;
} dns_getnameinfo_await_data_t;

static void dns_getnameinfo_await_cb(int status,
                                     const char* hostname,
                                     const char* service,
                                     void* user_data) {
    dns_getnameinfo_await_data_t* data = (dns_getnameinfo_await_data_t*)user_data;
    if (!data || !data->promise) return;
    
    if (status == 0) {
        /* 深拷贝字符串 */
        if (hostname && data->hostname_buf && data->hostname_len > 0) {
            size_t hostname_strlen = strlen(hostname);
            size_t copy_len = (hostname_strlen < data->hostname_len - 1) ? 
                             hostname_strlen : data->hostname_len - 1;
            memcpy(data->hostname_buf, hostname, copy_len);
            data->hostname_buf[copy_len] = '\0';
        }
        
        if (service && data->service_buf && data->service_len > 0) {
            size_t service_strlen = strlen(service);
            size_t copy_len = (service_strlen < data->service_len - 1) ? 
                             service_strlen : data->service_len - 1;
            memcpy(data->service_buf, service, copy_len);
            data->service_buf[copy_len] = '\0';
        }
    }
    
    vox_coroutine_promise_complete(data->promise, status, NULL);
}

int vox_coroutine_dns_getnameinfo_await(vox_coroutine_t* co,
                                         const vox_socket_addr_t* addr,
                                         int flags,
                                         char* hostname_buf,
                                         size_t hostname_len,
                                         char* service_buf,
                                         size_t service_len) {
    if (!co || !addr || !hostname_buf || hostname_len == 0 || 
        !service_buf || service_len == 0) return -1;
    
    vox_loop_t* loop = vox_coroutine_get_loop(co);
    if (!loop) return -1;
    
    /* 创建Promise */
    vox_coroutine_promise_t* promise = vox_coroutine_promise_create(loop);
    if (!promise) return -1;
    
    /* 创建回调数据（从loop的内存池分配，会在loop销毁时自动释放） */
    vox_mpool_t* mpool = vox_loop_get_mpool(loop);
    dns_getnameinfo_await_data_t* data = (dns_getnameinfo_await_data_t*)vox_mpool_alloc(
        mpool, sizeof(dns_getnameinfo_await_data_t));
    if (!data) {
        vox_coroutine_promise_destroy(promise);
        return -1;
    }
    memset(data, 0, sizeof(*data));
    data->promise = promise;
    data->hostname_buf = hostname_buf;
    data->hostname_len = hostname_len;
    data->service_buf = service_buf;
    data->service_len = service_len;
    data->mpool = mpool;
    
    /* 初始化输出缓冲区 */
    if (hostname_buf && hostname_len > 0) {
        hostname_buf[0] = '\0';
    }
    if (service_buf && service_len > 0) {
        service_buf[0] = '\0';
    }
    
    /* 调用异步getnameinfo */
    if (vox_dns_getnameinfo_simple(loop, addr, flags,
                                   hostname_buf, hostname_len,
                                   service_buf, service_len,
                                   dns_getnameinfo_await_cb, data) != 0) {
        vox_mpool_free(mpool, data);
        vox_coroutine_promise_destroy(promise);
        return -1;
    }
    
    /* 等待Promise完成 */
    int status = vox_coroutine_await(co, promise);
    
    /* 清理Promise（data从loop的内存池分配，会在loop销毁时自动释放） */
    vox_coroutine_promise_destroy(promise);
    
    return status;
}
