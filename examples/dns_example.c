/*
 * dns_example.c - 异步DNS解析示例
 * 演示如何使用 vox_dns 进行异步DNS解析
 */

#include "../vox_dns.h"
#include "../vox_loop.h"
#include "../vox_socket.h"
#include "../coroutine/vox_coroutine_dns.h"
#include "../coroutine/vox_coroutine.h" 
#include <stdio.h>
#include <stdlib.h>

#ifdef VOX_OS_WINDOWS
    #include <ws2tcpip.h>
#else
    #include <netdb.h>
#endif

/* 常量定义 */
#ifndef NI_MAXHOST
#define NI_MAXHOST 1025
#endif
#ifndef NI_MAXSERV
#define NI_MAXSERV 32
#endif

/* 全局变量 */
static vox_loop_t* g_loop = NULL;
static int g_pending_requests = 0;

/* ===== 回调函数 ===== */

/* 示例1的回调函数 */
static void callback_example1(int status, const vox_dns_addrinfo_t* addrinfo, void* user_data) {
    (void)user_data;
    
    if (status == 0 && addrinfo && addrinfo->count > 0) {
        printf("解析成功，找到 %zu 个地址：\n", addrinfo->count);
        for (size_t i = 0; i < addrinfo->count; i++) {
            char addr_str[64];
            if (vox_socket_address_to_string(&addrinfo->addrs[i], addr_str, sizeof(addr_str)) >= 0) {
                uint16_t port = vox_socket_get_port(&addrinfo->addrs[i]);
                if (port > 0) {
                    printf("  [%zu] %s:%u\n", i + 1, addr_str, port);
                } else {
                    printf("  [%zu] %s (端口未指定)\n", i + 1, addr_str);
                }
            }
        }
    } else {
        printf("解析失败 (status=%d)\n", status);
    }
    
    g_pending_requests--;
    if (g_pending_requests == 0) {
        vox_loop_stop(g_loop);
    }
}

/* 示例2的回调函数 */
static void callback_example2(vox_dns_getaddrinfo_t* req, int status,
                              const vox_dns_addrinfo_t* addrinfo, void* user_data) {
    (void)user_data;
    
    if (status == 0 && addrinfo && addrinfo->count > 0) {
        printf("解析成功，找到 %zu 个地址：\n", addrinfo->count);
        for (size_t i = 0; i < addrinfo->count && i < 3; i++) {  /* 只显示前3个 */
            char addr_str[64];
            if (vox_socket_address_to_string(&addrinfo->addrs[i], addr_str, sizeof(addr_str)) >= 0) {
                uint16_t port = vox_socket_get_port(&addrinfo->addrs[i]);
                printf("  [%zu] %s:%u\n", i + 1, addr_str, port);
            }
        }
        if (addrinfo->count > 3) {
            printf("  ... 还有 %zu 个地址\n", addrinfo->count - 3);
        }
    } else {
        printf("解析失败 (status=%d)\n", status);
    }
    
    /* 释放结果 */
    vox_dns_freeaddrinfo((vox_dns_addrinfo_t*)addrinfo);
    
    /* 销毁请求对象 */
    vox_dns_getaddrinfo_destroy(req);
    
    g_pending_requests--;
    if (g_pending_requests == 0) {
        vox_loop_stop(g_loop);
    }
}

/* 示例3的回调函数 */
static void callback_example3(int status, const vox_dns_addrinfo_t* addrinfo, void* user_data) {
    (void)user_data;
    
    if (status == 0 && addrinfo && addrinfo->count > 0) {
        printf("解析成功，找到 %zu 个地址：\n", addrinfo->count);
        for (size_t i = 0; i < addrinfo->count; i++) {
            char addr_str[64];
            if (vox_socket_address_to_string(&addrinfo->addrs[i], addr_str, sizeof(addr_str)) >= 0) {
                uint16_t port = vox_socket_get_port(&addrinfo->addrs[i]);
                const char* family_str = (addrinfo->addrs[i].family == VOX_AF_INET6) ? "IPv6" : "IPv4";
                printf("  [%zu] %s (%s):%u\n", i + 1, addr_str, family_str, port);
            }
        }
    } else {
        printf("解析失败 (status=%d)\n", status);
    }
    
    g_pending_requests--;
    if (g_pending_requests == 0) {
        vox_loop_stop(g_loop);
    }
}

/* 示例4的回调函数 */
static void callback_example4(int status, const char* hostname,
                              const char* service, void* user_data) {
    (void)user_data;
    
    if (status == 0 && hostname && service) {
        printf("反向解析成功：\n");
        printf("  地址: 8.8.8.8:53\n");
        printf("  主机名: %s\n", hostname);
        printf("  服务名: %s\n", service);
    } else {
        printf("反向解析失败 (status=%d)\n", status);
    }
    
    g_pending_requests--;
    if (g_pending_requests == 0) {
        vox_loop_stop(g_loop);
    }
}

/* 示例5的回调函数 */
static void callback_example5(int status, const vox_dns_addrinfo_t* addrinfo, void* user_data) {
    const char* hostname = (const char*)user_data;
    
    if (status == 0 && addrinfo && addrinfo->count > 0) {
        printf("  %s: 解析成功，找到 %zu 个地址：\n", hostname, addrinfo->count);
        for (size_t i = 0; i < addrinfo->count; i++) {
            char addr_str[64];
            if (vox_socket_address_to_string(&addrinfo->addrs[i], addr_str, sizeof(addr_str)) >= 0) {
                uint16_t port = vox_socket_get_port(&addrinfo->addrs[i]);
                const char* family_str = (addrinfo->addrs[i].family == VOX_AF_INET6) ? "IPv6" : "IPv4";
                if (port > 0) {
                    printf("    [%zu] %s (%s):%u\n", i + 1, addr_str, family_str, port);
                } else {
                    printf("    [%zu] %s (%s)\n", i + 1, addr_str, family_str);
                }
            }
        }
    } else {
        printf("  %s: 解析失败 (status=%d)\n", hostname, status);
    }
    
    g_pending_requests--;
    if (g_pending_requests == 0) {
        vox_loop_stop(g_loop);
    }
}

/* ===== 示例函数 ===== */

/* 示例1：使用便捷函数解析主机名（带端口） */
static void example_getaddrinfo_simple(void) {
    printf("\n=== 示例1：使用便捷函数解析主机名（带端口） ===\n");
    
    /* 创建事件循环 */
    vox_loop_t* loop = vox_loop_create();
    if (!loop) {
        fprintf(stderr, "创建事件循环失败\n");
        return;
    }
    
    g_loop = loop;
    g_pending_requests = 1;
    
    /* 解析 www.baidu.com:80（带端口） */
    printf("正在解析 www.baidu.com:80...\n");
    if (vox_dns_getaddrinfo_simple(loop, "www.baidu.com", "80", 
                                    VOX_AF_INET, callback_example1, NULL, 5000) != 0) {
        fprintf(stderr, "启动DNS解析失败\n");
        vox_loop_destroy(loop);
        return;
    }
    
    /* 运行事件循环 */
    vox_loop_run(loop, VOX_RUN_DEFAULT);
    
    /* 清理 */
    vox_loop_destroy(loop);
    printf("示例1完成\n");
}

/* 示例1b：解析主机名（不带端口） */
static void example_getaddrinfo_no_port(void) {
    printf("\n=== 示例1b：解析主机名（不带端口） ===\n");
    
    /* 创建事件循环 */
    vox_loop_t* loop = vox_loop_create();
    if (!loop) {
        fprintf(stderr, "创建事件循环失败\n");
        return;
    }
    
    g_loop = loop;
    g_pending_requests = 1;
    
    /* 解析 www.baidu.com（不带端口，service传NULL） */
    printf("正在解析 www.baidu.com（不带端口）...\n");
    if (vox_dns_getaddrinfo_simple(loop, "www.baidu.com", NULL, 
                                    VOX_AF_INET, callback_example1, NULL, 5000) != 0) {
        fprintf(stderr, "启动DNS解析失败\n");
        vox_loop_destroy(loop);
        return;
    }
    
    /* 运行事件循环 */
    vox_loop_run(loop, VOX_RUN_DEFAULT);
    
    /* 清理 */
    vox_loop_destroy(loop);
    printf("示例1b完成\n");
}

/* 示例2：手动管理请求对象（支持取消） */
static void example_getaddrinfo_manual(void) {
    printf("\n=== 示例2：手动管理请求对象 ===\n");
    
    /* 创建事件循环 */
    vox_loop_t* loop = vox_loop_create();
    if (!loop) {
        fprintf(stderr, "创建事件循环失败\n");
        return;
    }
    
    g_loop = loop;
    g_pending_requests = 1;
    
    /* 创建请求对象 */
    vox_dns_getaddrinfo_t* req = vox_dns_getaddrinfo_create(loop);
    if (!req) {
        fprintf(stderr, "创建DNS请求失败\n");
        vox_loop_destroy(loop);
        return;
    }
    
    /* 解析 github.com:443 */
    printf("正在解析 github.com:443...\n");
    if (vox_dns_getaddrinfo(req, "github.com", "443", 
                            VOX_AF_INET, callback_example2, NULL, 5000) != 0) {
        fprintf(stderr, "启动DNS解析失败\n");
        vox_dns_getaddrinfo_destroy(req);
        vox_loop_destroy(loop);
        return;
    }
    
    /* 运行事件循环 */
    vox_loop_run(loop, VOX_RUN_DEFAULT);
    
    /* 清理 */
    vox_loop_destroy(loop);
    printf("示例2完成\n");
}

/* 示例3：解析IPv6地址 */
static void example_getaddrinfo_ipv6(void) {
    printf("\n=== 示例3：解析IPv6地址 ===\n");
    
    /* 创建事件循环 */
    vox_loop_t* loop = vox_loop_create();
    if (!loop) {
        fprintf(stderr, "创建事件循环失败\n");
        return;
    }
    
    g_loop = loop;
    g_pending_requests = 1;
    
    /* 解析 IPv6 地址（family=0 表示任意，会返回IPv4和IPv6） */
    printf("正在解析 ipv6.google.com（任意地址族）...\n");
    if (vox_dns_getaddrinfo_simple(loop, "ipv6.google.com", "80", 
                                    0, callback_example3, NULL, 5000) != 0) {
        fprintf(stderr, "启动DNS解析失败\n");
        vox_loop_destroy(loop);
        return;
    }
    
    /* 运行事件循环 */
    vox_loop_run(loop, VOX_RUN_DEFAULT);
    
    /* 清理 */
    vox_loop_destroy(loop);
    printf("示例3完成\n");
}

/* 示例4：反向解析（地址到主机名） */
static void example_getnameinfo(void) {
    printf("\n=== 示例4：反向解析（地址到主机名） ===\n");
    
    /* 创建事件循环 */
    vox_loop_t* loop = vox_loop_create();
    if (!loop) {
        fprintf(stderr, "创建事件循环失败\n");
        return;
    }
    
    g_loop = loop;
    g_pending_requests = 1;
    
    /* 准备要解析的地址 */
    vox_socket_addr_t addr;
    if (vox_socket_parse_address("8.8.8.8", 53, &addr) != 0) {
        fprintf(stderr, "解析地址失败\n");
        vox_loop_destroy(loop);
        return;
    }
    
    char hostname_buf[NI_MAXHOST];
    char service_buf[NI_MAXSERV];
    
    /* 反向解析 */
    printf("正在反向解析 8.8.8.8:53...\n");
    if (vox_dns_getnameinfo_simple(loop, &addr, 0,
                                   hostname_buf, sizeof(hostname_buf),
                                   service_buf, sizeof(service_buf),
                                   callback_example4, NULL) != 0) {
        fprintf(stderr, "启动反向DNS解析失败\n");
        vox_loop_destroy(loop);
        return;
    }
    
    /* 运行事件循环 */
    vox_loop_run(loop, VOX_RUN_DEFAULT);
    
    /* 清理 */
    vox_loop_destroy(loop);
    printf("示例4完成\n");
}

/* 示例5：并发解析多个主机名 */
static void example_concurrent_resolve(void) {
    printf("\n=== 示例5：并发解析多个主机名 ===\n");
    
    /* 创建事件循环 */
    vox_loop_t* loop = vox_loop_create();
    if (!loop) {
        fprintf(stderr, "创建事件循环失败\n");
        return;
    }
    
    g_loop = loop;
    g_pending_requests = 0;
    
    /* 要解析的主机名列表 */
    const char* hosts[] = {
        "www.google.com",
        "www.github.com",
        "www.microsoft.com",
        "www.apple.com"
    };
    const int host_count = sizeof(hosts) / sizeof(hosts[0]);
    
    /* 并发启动多个解析请求 */
    printf("正在并发解析 %d 个主机名...\n", host_count);
    for (int i = 0; i < host_count; i++) {
        if (vox_dns_getaddrinfo_simple(loop, hosts[i], "80", 
                                        VOX_AF_INET, callback_example5, (void*)hosts[i], 5000) == 0) {
            g_pending_requests++;
        } else {
            printf("  启动 %s 的解析失败\n", hosts[i]);
        }
    }
    
    if (g_pending_requests == 0) {
        printf("没有成功启动任何解析请求\n");
        vox_loop_destroy(loop);
        return;
    }
    
    printf("等待解析完成...\n");
    
    /* 运行事件循环 */
    vox_loop_run(loop, VOX_RUN_DEFAULT);
    
    /* 清理 */
    vox_loop_destroy(loop);
    printf("示例5完成\n");
}

/* ===== 协程示例 ===== */

/* 示例6：使用协程解析主机名 */
VOX_COROUTINE_ENTRY(coroutine_getaddrinfo_example, void* user_data) {
    const char* hostname = (const char*)user_data;
    printf("\n=== 示例6：使用协程解析主机名 ===\n");
    printf("正在解析 %s:80...\n", hostname);
    
    vox_dns_addrinfo_t addrinfo = {0};
    int status = vox_coroutine_dns_getaddrinfo_await(co, hostname, "80", 
                                                      VOX_AF_INET, &addrinfo);
    
    if (status == 0 && addrinfo.count > 0) {
        printf("解析成功，找到 %zu 个地址：\n", addrinfo.count);
        for (size_t i = 0; i < addrinfo.count; i++) {
            char addr_str[64];
            if (vox_socket_address_to_string(&addrinfo.addrs[i], addr_str, sizeof(addr_str)) >= 0) {
                uint16_t port = vox_socket_get_port(&addrinfo.addrs[i]);
                const char* family_str = (addrinfo.addrs[i].family == VOX_AF_INET6) ? "IPv6" : "IPv4";
                printf("  [%zu] %s (%s):%u\n", i + 1, addr_str, family_str, port);
            }
        }
        
        /* 释放结果 */
        vox_dns_freeaddrinfo(&addrinfo);
    } else {
        printf("解析失败 (status=%d)\n", status);
    }
    
    printf("示例6完成\n");
    
    /* 停止事件循环 */
    vox_loop_t* loop = vox_coroutine_get_loop(co);
    if (loop) {
        vox_loop_stop(loop);
    }
}

/* 示例7：使用协程进行反向解析 */
VOX_COROUTINE_ENTRY(coroutine_getnameinfo_example, void* user_data) {
    (void)user_data;
    printf("\n=== 示例7：使用协程进行反向解析 ===\n");
    
    /* 准备要解析的地址 */
    vox_socket_addr_t addr;
    if (vox_socket_parse_address("8.8.8.8", 53, &addr) != 0) {
        printf("解析地址失败\n");
        vox_loop_t* loop = vox_coroutine_get_loop(co);
        if (loop) vox_loop_stop(loop);
        return;
    }
    
    char hostname_buf[NI_MAXHOST];
    char service_buf[NI_MAXSERV];
    
    printf("正在反向解析 8.8.8.8:53...\n");
    int status = vox_coroutine_dns_getnameinfo_await(co, &addr, 0,
                                                     hostname_buf, sizeof(hostname_buf),
                                                     service_buf, sizeof(service_buf));
    
    if (status == 0) {
        printf("反向解析成功：\n");
        printf("  地址: 8.8.8.8:53\n");
        printf("  主机名: %s\n", hostname_buf);
        printf("  服务名: %s\n", service_buf);
    } else {
        printf("反向解析失败 (status=%d)\n", status);
    }
    
    printf("示例7完成\n");
    
    /* 停止事件循环 */
    vox_loop_t* loop = vox_coroutine_get_loop(co);
    if (loop) {
        vox_loop_stop(loop);
    }
}

/* 示例8：使用协程并发解析多个主机名 */
VOX_COROUTINE_ENTRY(coroutine_concurrent_example, void* user_data) {
    (void)user_data;
    printf("\n=== 示例8：使用协程并发解析多个主机名 ===\n");
    
    const char* hosts[] = {
        "www.google.com",
        "www.github.com",
        "www.microsoft.com"
    };
    const int host_count = sizeof(hosts) / sizeof(hosts[0]);
    
    printf("正在并发解析 %d 个主机名...\n", host_count);
    
    for (int i = 0; i < host_count; i++) {
        vox_dns_addrinfo_t addrinfo = {0};
        int status = vox_coroutine_dns_getaddrinfo_await(co, hosts[i], "80", 
                                                          VOX_AF_INET, &addrinfo);
        
        if (status == 0 && addrinfo.count > 0) {
            printf("  %s: 解析成功，找到 %zu 个地址\n", hosts[i], addrinfo.count);
            if (addrinfo.count > 0) {
                char addr_str[64];
                if (vox_socket_address_to_string(&addrinfo.addrs[0], addr_str, sizeof(addr_str)) >= 0) {
                    printf("    第一个地址: %s\n", addr_str);
                }
            }
            vox_dns_freeaddrinfo(&addrinfo);
        } else {
            printf("  %s: 解析失败 (status=%d)\n", hosts[i], status);
        }
    }
    
    printf("示例8完成\n");
    
    /* 停止事件循环 */
    vox_loop_t* loop = vox_coroutine_get_loop(co);
    if (loop) {
        vox_loop_stop(loop);
    }
}

static void example_coroutine_getaddrinfo(void) {
    /* 创建事件循环 */
    vox_loop_t* loop = vox_loop_create();
    if (!loop) {
        fprintf(stderr, "创建事件循环失败\n");
        return;
    }
    
    /* 启动协程 */
    VOX_COROUTINE_START(loop, coroutine_getaddrinfo_example, (void*)"www.baidu.com");
    
    /* 运行事件循环 */
    vox_loop_run(loop, VOX_RUN_DEFAULT);
    
    /* 清理 */
    vox_loop_destroy(loop);
}

static void example_coroutine_getnameinfo(void) {
    /* 创建事件循环 */
    vox_loop_t* loop = vox_loop_create();
    if (!loop) {
        fprintf(stderr, "创建事件循环失败\n");
        return;
    }
    
    /* 启动协程 */
    VOX_COROUTINE_START(loop, coroutine_getnameinfo_example, NULL);
    
    /* 运行事件循环 */
    vox_loop_run(loop, VOX_RUN_DEFAULT);
    
    /* 清理 */
    vox_loop_destroy(loop);
}

static void example_coroutine_concurrent(void) {
    /* 创建事件循环 */
    vox_loop_t* loop = vox_loop_create();
    if (!loop) {
        fprintf(stderr, "创建事件循环失败\n");
        return;
    }
    
    /* 启动协程 */
    VOX_COROUTINE_START(loop, coroutine_concurrent_example, NULL);
    
    /* 运行事件循环 */
    vox_loop_run(loop, VOX_RUN_DEFAULT);
    
    /* 清理 */
    vox_loop_destroy(loop);
}

int main(int argc, char* argv[]) {
    printf("=== 异步DNS解析示例 ===\n");
    
    /* 初始化socket库（Windows需要） */
    if (vox_socket_init() != 0) {
        fprintf(stderr, "初始化socket库失败\n");
        return 1;
    }
    
    /* 根据命令行参数选择示例 */
    if (argc > 1) {
        int example_num = atoi(argv[1]);
        switch (example_num) {
            case 1:
                example_getaddrinfo_simple();
                break;
            case 2:
                example_getaddrinfo_manual();
                break;
            case 3:
                example_getaddrinfo_ipv6();
                break;
            case 4:
                example_getnameinfo();
                break;
            case 5:
                example_concurrent_resolve();
                break;
            case 6:
                example_coroutine_getaddrinfo();
                break;
            case 7:
                example_coroutine_getnameinfo();
                break;
            case 8:
                example_coroutine_concurrent();
                break;
            default:
                fprintf(stderr, "无效的示例编号: %d (1-8)\n", example_num);
                vox_socket_cleanup();
                return 1;
        }
    } else {
        /* 运行所有示例 */
        example_getaddrinfo_simple();
        example_getaddrinfo_no_port();
        example_getaddrinfo_manual();
        example_getaddrinfo_ipv6();
        example_getnameinfo();
        example_concurrent_resolve();
        example_coroutine_getaddrinfo();
        example_coroutine_getnameinfo();
        example_coroutine_concurrent();
    }
    
    /* 清理socket库 */
    vox_socket_cleanup();
    
    printf("\n所有示例完成\n");
    return 0;
}
