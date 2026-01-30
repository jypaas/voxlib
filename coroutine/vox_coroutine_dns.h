/*
 * vox_dns_coroutine.h - DNS协程适配器
 * 提供async/await风格的协程API，避免回调地狱
 */

#ifndef VOX_DNS_COROUTINE_H
#define VOX_DNS_COROUTINE_H

#include "../vox_dns.h"
#include "vox_coroutine.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ===== 协程适配接口 ===== */

/**
 * 在协程中解析主机名和服务名为地址
 * @param co 协程指针
 * @param node 节点名（主机名或IP地址字符串），NULL表示本地地址
 * @param service 服务名（端口号如"80"或服务名如"http"），NULL表示不指定端口
 * @param family 地址族（VOX_AF_INET, VOX_AF_INET6, 或0表示任意）
 * @param out_addrinfo 输出解析结果（可为NULL）
 * @return 成功返回0，失败返回-1
 * @note out_addrinfo指向的结果在协程结束后仍然有效（深拷贝），需要调用者释放
 * @note 释放结果使用 vox_dns_freeaddrinfo()
 */
int vox_coroutine_dns_getaddrinfo_await(vox_coroutine_t* co,
                                        const char* node,
                                        const char* service,
                                        vox_address_family_t family,
                                        vox_dns_addrinfo_t* out_addrinfo);

/**
 * 在协程中解析地址为主机名和服务名
 * @param co 协程指针
 * @param addr 要解析的地址
 * @param flags 标志位（保留，传0）
 * @param hostname_buf 主机名输出缓冲区
 * @param hostname_len 主机名缓冲区大小
 * @param service_buf 服务名输出缓冲区
 * @param service_len 服务名缓冲区大小
 * @return 成功返回0，失败返回-1
 * @note hostname_buf 和 service_buf 在协程结束后仍然有效（深拷贝）
 */
int vox_coroutine_dns_getnameinfo_await(vox_coroutine_t* co,
                                         const vox_socket_addr_t* addr,
                                         int flags,
                                         char* hostname_buf,
                                         size_t hostname_len,
                                         char* service_buf,
                                         size_t service_len);

#ifdef __cplusplus
}
#endif

#endif /* VOX_DNS_COROUTINE_H */
