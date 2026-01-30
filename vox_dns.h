/*
 * vox_dns.h - 异步DNS解析
 * 提供类似 libuv 的异步DNS解析接口
 */

#ifndef VOX_DNS_H
#define VOX_DNS_H

#include "vox_handle.h"
#include "vox_socket.h"
#include "vox_timer.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 前向声明 */
typedef struct vox_dns_getaddrinfo vox_dns_getaddrinfo_t;
typedef struct vox_dns_getnameinfo vox_dns_getnameinfo_t;

/* DNS解析结果结构 */
typedef struct {
    vox_socket_addr_t* addrs;    /* 地址数组 */
    size_t count;                 /* 地址数量 */
} vox_dns_addrinfo_t;

/* getaddrinfo 回调函数类型 */
typedef void (*vox_dns_getaddrinfo_cb)(vox_dns_getaddrinfo_t* req, 
                                       int status, 
                                       const vox_dns_addrinfo_t* addrinfo,
                                       void* user_data);

/* getaddrinfo 简化回调函数类型（不需要req参数） */
typedef void (*vox_dns_getaddrinfo_simple_cb)(int status,
                                               const vox_dns_addrinfo_t* addrinfo,
                                               void* user_data);

/* getnameinfo 回调函数类型 */
typedef void (*vox_dns_getnameinfo_cb)(vox_dns_getnameinfo_t* req,
                                       int status,
                                       const char* hostname,
                                       const char* service,
                                       void* user_data);

/* getnameinfo 简化回调函数类型（不需要req参数） */
typedef void (*vox_dns_getnameinfo_simple_cb)(int status,
                                               const char* hostname,
                                               const char* service,
                                               void* user_data);

/* DNS getaddrinfo 请求结构 */
struct vox_dns_getaddrinfo {
    /* 句柄基类（必须作为第一个成员） */
    vox_handle_t handle;
    
    /* 回调函数 */
    vox_dns_getaddrinfo_cb cb;
    void* user_data;
    
    /* 请求参数 */
    char* node;              /* 节点名（主机名或IP地址字符串） */
    char* service;            /* 服务名（端口号或服务名） */
    vox_address_family_t family;  /* 地址族（VOX_AF_INET, VOX_AF_INET6, 或0表示任意） */
    
    /* 解析结果 */
    vox_dns_addrinfo_t addrinfo;
    
    /* 内部状态 */
    bool pending;             /* 是否正在解析 */
    
    /* 超时机制 */
    vox_timer_t* timeout_timer;  /* 超时定时器 */
    uint64_t timeout_ms;         /* 超时时间（毫秒），0表示无超时 */
};

/* DNS getnameinfo 请求结构 */
struct vox_dns_getnameinfo {
    /* 句柄基类（必须作为第一个成员） */
    vox_handle_t handle;
    
    /* 回调函数 */
    vox_dns_getnameinfo_cb cb;
    void* user_data;
    
    /* 请求参数 */
    vox_socket_addr_t addr;   /* 要解析的地址 */
    int flags;                /* 标志位（保留，传0） */
    
    /* 解析结果 */
    char* hostname;           /* 主机名 */
    char* service;            /* 服务名 */
    size_t hostname_len;      /* 主机名缓冲区大小 */
    size_t service_len;       /* 服务名缓冲区大小 */
    
    /* 内部状态 */
    bool pending;             /* 是否正在解析 */
};

/**
 * 初始化 getaddrinfo 请求
 * @param req 请求指针
 * @param loop 事件循环指针
 * @return 成功返回0，失败返回-1
 */
int vox_dns_getaddrinfo_init(vox_dns_getaddrinfo_t* req, vox_loop_t* loop);

/**
 * 使用内存池创建 getaddrinfo 请求
 * @param loop 事件循环指针
 * @return 成功返回请求指针，失败返回 NULL
 */
vox_dns_getaddrinfo_t* vox_dns_getaddrinfo_create(vox_loop_t* loop);

/**
 * 销毁 getaddrinfo 请求
 * @param req 请求指针
 */
void vox_dns_getaddrinfo_destroy(vox_dns_getaddrinfo_t* req);

/**
 * 异步解析主机名和服务名为地址
 * @param req 请求指针（必须已初始化）
 * @param node 节点名（主机名或IP地址字符串），NULL表示本地地址
 * @param service 服务名（端口号如"80"或服务名如"http"），NULL表示不指定端口（返回的地址端口为0）
 * @param family 地址族（VOX_AF_INET, VOX_AF_INET6, 或0表示任意）
 * @param cb 回调函数
 * @param user_data 用户数据
 * @param timeout_ms 超时时间（毫秒），0表示无超时（默认5000毫秒）
 * @return 成功返回0，失败返回-1
 * 
 * @note 回调函数中，如果status为0，addrinfo包含解析结果；否则解析失败
 * @note 解析结果在回调函数中有效，回调返回后可能被释放
 * @note 如果需要在回调外使用结果，需要复制地址数组
 * @note 超时后会自动取消请求并调用回调，status为-1
 */
int vox_dns_getaddrinfo(vox_dns_getaddrinfo_t* req,
                        const char* node,
                        const char* service,
                        vox_address_family_t family,
                        vox_dns_getaddrinfo_cb cb,
                        void* user_data,
                        uint64_t timeout_ms);

/**
 * 取消正在进行的 getaddrinfo 请求
 * @param req 请求指针
 * @return 成功返回0，失败返回-1
 */
int vox_dns_getaddrinfo_cancel(vox_dns_getaddrinfo_t* req);

/**
 * 释放 getaddrinfo 结果
 * @param addrinfo 结果指针
 */
void vox_dns_freeaddrinfo(vox_dns_addrinfo_t* addrinfo);

/**
 * 便捷函数：异步解析主机名和服务名为地址（自动管理请求对象）
 * @param loop 事件循环指针
 * @param node 节点名（主机名或IP地址字符串），NULL表示本地地址
 * @param service 服务名（端口号如"80"或服务名如"http"），NULL表示不指定端口（返回的地址端口为0）
 * @param family 地址族（VOX_AF_INET, VOX_AF_INET6, 或0表示任意）
 * @param cb 回调函数（简化版，不需要req参数）
 * @param user_data 用户数据
 * @return 成功返回0，失败返回-1
 * 
 * @note 此函数自动创建和管理请求对象，回调完成后自动释放
 * @note 如果需要取消请求，请使用 vox_dns_getaddrinfo() 和手动管理请求对象
 */
int vox_dns_getaddrinfo_simple(vox_loop_t* loop,
                               const char* node,
                               const char* service,
                               vox_address_family_t family,
                               vox_dns_getaddrinfo_simple_cb cb,
                               void* user_data,
                               uint64_t timeout_ms);

/**
 * 初始化 getnameinfo 请求
 * @param req 请求指针
 * @param loop 事件循环指针
 * @return 成功返回0，失败返回-1
 */
int vox_dns_getnameinfo_init(vox_dns_getnameinfo_t* req, vox_loop_t* loop);

/**
 * 使用内存池创建 getnameinfo 请求
 * @param loop 事件循环指针
 * @return 成功返回请求指针，失败返回 NULL
 */
vox_dns_getnameinfo_t* vox_dns_getnameinfo_create(vox_loop_t* loop);

/**
 * 销毁 getnameinfo 请求
 * @param req 请求指针
 */
void vox_dns_getnameinfo_destroy(vox_dns_getnameinfo_t* req);

/**
 * 异步解析地址为主机名和服务名
 * @param req 请求指针（必须已初始化）
 * @param addr 要解析的地址
 * @param flags 标志位（保留，传0）
 * @param hostname_buf 主机名输出缓冲区
 * @param hostname_len 主机名缓冲区大小
 * @param service_buf 服务名输出缓冲区
 * @param service_len 服务名缓冲区大小
 * @param cb 回调函数
 * @param user_data 用户数据
 * @return 成功返回0，失败返回-1
 * 
 * @note 回调函数中，如果status为0，hostname和service包含解析结果；否则解析失败
 * @note hostname和service在回调函数中有效，回调返回后可能被释放
 */
int vox_dns_getnameinfo(vox_dns_getnameinfo_t* req,
                        const vox_socket_addr_t* addr,
                        int flags,
                        char* hostname_buf,
                        size_t hostname_len,
                        char* service_buf,
                        size_t service_len,
                        vox_dns_getnameinfo_cb cb,
                        void* user_data);

/**
 * 取消正在进行的 getnameinfo 请求
 * @param req 请求指针
 * @return 成功返回0，失败返回-1
 */
int vox_dns_getnameinfo_cancel(vox_dns_getnameinfo_t* req);

/**
 * 便捷函数：异步解析地址为主机名和服务名（自动管理请求对象）
 * @param loop 事件循环指针
 * @param addr 要解析的地址
 * @param flags 标志位（保留，传0）
 * @param hostname_buf 主机名输出缓冲区
 * @param hostname_len 主机名缓冲区大小
 * @param service_buf 服务名输出缓冲区
 * @param service_len 服务名缓冲区大小
 * @param cb 回调函数（简化版，不需要req参数）
 * @param user_data 用户数据
 * @return 成功返回0，失败返回-1
 * 
 * @note 此函数自动创建和管理请求对象，回调完成后自动释放
 * @note 如果需要取消请求，请使用 vox_dns_getnameinfo() 和手动管理请求对象
 */
int vox_dns_getnameinfo_simple(vox_loop_t* loop,
                                const vox_socket_addr_t* addr,
                                int flags,
                                char* hostname_buf,
                                size_t hostname_len,
                                char* service_buf,
                                size_t service_len,
                                vox_dns_getnameinfo_simple_cb cb,
                                void* user_data);

#ifdef __cplusplus
}
#endif

#endif /* VOX_DNS_H */
