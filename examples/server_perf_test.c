/*
 * server_perf_test.c - TCP/UDP/TLS 服务端性能测试工具
 * 测试服务端的并发连接数、吞吐量、延迟等性能指标
 */

#include "vox_loop.h"
#include "vox_tcp.h"
#include "vox_udp.h"
#include "vox_tls.h"
#include "vox_socket.h"
#include "vox_log.h"
#include "vox_mpool.h"
#include "vox_os.h"  /* 包含平台特定头文件 */
#include "vox_backend.h"
#include "vox_time.h"
#include "vox_timer.h"
#include "ssl/vox_ssl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <stdint.h>

/* 测试配置 */
#define DEFAULT_PORT 9999
#define BUFFER_SIZE 65536
#define STATS_INTERVAL_SEC 5  /* 统计输出间隔（秒） */

/* 性能统计结构 */
typedef struct {
    uint64_t total_connections;      /* 总连接数 */
    uint64_t active_connections;     /* 当前活跃连接数 */
    uint64_t total_bytes_received;   /* 总接收字节数 */
    uint64_t total_bytes_sent;       /* 总发送字节数 */
    uint64_t total_packets;          /* 总数据包数（UDP） */
    uint64_t connection_errors;      /* 连接错误数 */
    uint64_t read_errors;            /* 读取错误数 */
    uint64_t write_errors;           /* 写入错误数 */
    vox_time_t start_time;           /* 测试开始时间 */
    vox_time_t last_stats_time;      /* 上次统计时间 */
    uint64_t last_bytes_received;    /* 上次统计时的接收字节数 */
    uint64_t last_bytes_sent;        /* 上次统计时的发送字节数 */
    uint64_t last_connections;       /* 上次统计时的连接数 */
} perf_stats_t;

/* 全局变量 */
static vox_loop_t* g_loop = NULL;
static perf_stats_t g_stats = {0};
static bool g_running = true;

/* TCP 客户端连接数据 */
typedef struct {
    vox_tcp_t* tcp;
    char buffer[BUFFER_SIZE];
    uint64_t bytes_received;
    uint64_t bytes_sent;
    vox_time_t connect_time;
} tcp_client_data_t;

/* UDP 客户端数据 */
typedef struct {
    vox_socket_addr_t addr;
    uint64_t packets_received;
    uint64_t bytes_received;
} udp_client_data_t;

/* TLS 客户端连接数据 */
typedef struct {
    vox_tls_t* tls;
    char buffer[BUFFER_SIZE];
    uint64_t bytes_received;
    uint64_t bytes_sent;
    vox_time_t connect_time;
    vox_time_t handshake_time;
} tls_client_data_t;

/* 前向声明 */
static void print_stats(const char* protocol);
static void stats_timer_cb(vox_timer_t* timer, void* user_data);
static void signal_handler(int sig);
static vox_backend_type_t parse_backend_type(const char* backend_str);

/* TCP 服务器回调 */
static void tcp_alloc_cb(vox_tcp_t* tcp, size_t suggested_size, 
                         void* buf, size_t* len, void* user_data) {
    tcp_client_data_t* data = (tcp_client_data_t*)user_data;
    (void)tcp;
    (void)suggested_size;
    *(void**)buf = data->buffer;
    *len = sizeof(data->buffer);
}

static void tcp_read_cb(vox_tcp_t* tcp, ssize_t nread, const void* buf, void* user_data) {
    tcp_client_data_t* data = (tcp_client_data_t*)user_data;
    
    if (nread < 0) {
        g_stats.read_errors++;
        g_stats.active_connections--;
        vox_tcp_read_stop(tcp);
        vox_handle_close((vox_handle_t*)tcp, NULL);
        vox_tcp_destroy(tcp);
        vox_mpool_t* mpool = vox_loop_get_mpool(g_loop);
        vox_mpool_free(mpool, data);
        return;
    }
    
    if (nread == 0) {
        /* 连接关闭 */
        g_stats.active_connections--;
        vox_tcp_read_stop(tcp);
        vox_handle_close((vox_handle_t*)tcp, NULL);
        vox_tcp_destroy(tcp);
        vox_mpool_t* mpool = vox_loop_get_mpool(g_loop);
        vox_mpool_free(mpool, data);
        return;
    }
    
    /* 更新统计 */
    data->bytes_received += (uint64_t)nread;
    g_stats.total_bytes_received += (uint64_t)nread;
    
    /* Echo: 将收到的数据原样发送回去 */
    if (vox_tcp_write(tcp, buf, (size_t)nread, NULL) != 0) {
        g_stats.write_errors++;
    } else {
        data->bytes_sent += (uint64_t)nread;
        g_stats.total_bytes_sent += (uint64_t)nread;
    }
}

static void tcp_connection_cb(vox_tcp_t* server, int status, void* user_data) {
    (void)user_data;
    
    if (status != 0) {
        g_stats.connection_errors++;
        return;
    }
    
    /* 创建客户端 TCP 句柄 */
    vox_tcp_t* client = vox_tcp_create(g_loop);
    if (!client) {
        g_stats.connection_errors++;
        return;
    }
    
    /* 接受连接 */
    if (vox_tcp_accept(server, client) != 0) {
        g_stats.connection_errors++;
        vox_tcp_destroy(client);
        return;
    }
    
    /* 分配客户端数据 */
    vox_mpool_t* mpool = vox_loop_get_mpool(g_loop);
    tcp_client_data_t* data = (tcp_client_data_t*)vox_mpool_alloc(mpool, sizeof(tcp_client_data_t));
    if (!data) {
        vox_tcp_destroy(client);
        g_stats.connection_errors++;
        return;
    }
    
    memset(data, 0, sizeof(tcp_client_data_t));
    data->tcp = client;
    data->connect_time = vox_time_monotonic();
    
    vox_handle_set_data((vox_handle_t*)client, data);
    
    /* 开始读取 */
    if (vox_tcp_read_start(client, tcp_alloc_cb, tcp_read_cb) != 0) {
        vox_mpool_free(mpool, data);
        vox_tcp_destroy(client);
        g_stats.connection_errors++;
        return;
    }
    
    g_stats.total_connections++;
    g_stats.active_connections++;
}

/* UDP 服务器回调 */
static void udp_alloc_cb(vox_udp_t* udp, size_t suggested_size,
                         void* buf, size_t* len, void* user_data) {
    static char recv_buffer[BUFFER_SIZE];
    (void)udp;
    (void)suggested_size;
    (void)user_data;
    *(void**)buf = recv_buffer;
    *len = sizeof(recv_buffer);
}

static void udp_recv_cb(vox_udp_t* udp, ssize_t nread,
                        const void* buf,
                        const vox_socket_addr_t* addr,
                        unsigned int flags,
                        void* user_data) {
    (void)udp;
    (void)flags;
    (void)user_data;
    
    if (nread < 0) {
        g_stats.read_errors++;
        return;
    }
    
    if (nread == 0) {
        return;
    }
    
    /* 更新统计 */
    g_stats.total_bytes_received += (uint64_t)nread;
    g_stats.total_packets++;
    
    /* Echo: 将收到的数据原样发送回去 */
    if (addr && vox_udp_send(udp, buf, (size_t)nread, addr, NULL) == 0) {
        g_stats.total_bytes_sent += (uint64_t)nread;
    } else {
        g_stats.write_errors++;
    }
}

/* TLS 服务器回调 */
static void tls_alloc_cb(vox_tls_t* tls, size_t suggested_size,
                         void* buf, size_t* len, void* user_data) {
    tls_client_data_t* data = (tls_client_data_t*)user_data;
    (void)tls;
    (void)suggested_size;
    *(void**)buf = data->buffer;
    *len = sizeof(data->buffer);
}

static void tls_read_cb(vox_tls_t* tls, ssize_t nread, const void* buf, void* user_data) {
    tls_client_data_t* data = (tls_client_data_t*)user_data;
    
    if (nread < 0) {
        g_stats.read_errors++;
        g_stats.active_connections--;
        vox_tls_read_stop(tls);
        vox_handle_close((vox_handle_t*)tls, NULL);
        vox_tls_destroy(tls);
        vox_mpool_t* mpool = vox_loop_get_mpool(g_loop);
        vox_mpool_free(mpool, data);
        return;
    }
    
    if (nread == 0) {
        /* 连接关闭 */
        g_stats.active_connections--;
        vox_tls_read_stop(tls);
        vox_handle_close((vox_handle_t*)tls, NULL);
        vox_tls_destroy(tls);
        vox_mpool_t* mpool = vox_loop_get_mpool(g_loop);
        vox_mpool_free(mpool, data);
        return;
    }
    
    /* 更新统计 */
    data->bytes_received += (uint64_t)nread;
    g_stats.total_bytes_received += (uint64_t)nread;
    
    /* Echo: 将收到的数据原样发送回去 */
    if (vox_tls_write(tls, buf, (size_t)nread, NULL) != 0) {
        g_stats.write_errors++;
    } else {
        data->bytes_sent += (uint64_t)nread;
        g_stats.total_bytes_sent += (uint64_t)nread;
    }
}

static void tls_handshake_cb(vox_tls_t* tls, int status, void* user_data) {
    tls_client_data_t* data = (tls_client_data_t*)user_data;
    
    if (status != 0) {
        g_stats.connection_errors++;
        g_stats.active_connections--;
        vox_handle_close((vox_handle_t*)tls, NULL);
        vox_tls_destroy(tls);
        vox_mpool_t* mpool = vox_loop_get_mpool(g_loop);
        vox_mpool_free(mpool, data);
        return;
    }
    
    data->handshake_time = vox_time_monotonic();
    
    /* 开始读取 */
    if (vox_tls_read_start(tls, tls_alloc_cb, tls_read_cb) != 0) {
        g_stats.connection_errors++;
        g_stats.active_connections--;
        vox_handle_close((vox_handle_t*)tls, NULL);
        vox_tls_destroy(tls);
        vox_mpool_t* mpool = vox_loop_get_mpool(g_loop);
        vox_mpool_free(mpool, data);
        return;
    }
}

static void tls_connection_cb(vox_tls_t* server, int status, void* user_data) {
    (void)user_data;
    
    if (status != 0) {
        g_stats.connection_errors++;
        return;
    }
    
    /* 创建客户端 TLS 句柄 */
    vox_tls_t* client = vox_tls_create(g_loop, server->ssl_ctx);
    if (!client) {
        g_stats.connection_errors++;
        return;
    }
    
    /* 接受连接 */
    if (vox_tls_accept(server, client) != 0) {
        g_stats.connection_errors++;
        vox_tls_destroy(client);
        return;
    }
    
    /* 分配客户端数据 */
    vox_mpool_t* mpool = vox_loop_get_mpool(g_loop);
    tls_client_data_t* data = (tls_client_data_t*)vox_mpool_alloc(mpool, sizeof(tls_client_data_t));
    if (!data) {
        vox_tls_destroy(client);
        g_stats.connection_errors++;
        return;
    }
    
    memset(data, 0, sizeof(tls_client_data_t));
    data->tls = client;
    data->connect_time = vox_time_monotonic();
    
    vox_handle_set_data((vox_handle_t*)client, data);
    
    /* 开始 TLS 握手 */
    if (vox_tls_handshake(client, tls_handshake_cb) != 0) {
        vox_mpool_free(mpool, data);
        vox_tls_destroy(client);
        g_stats.connection_errors++;
        return;
    }
    
    g_stats.total_connections++;
    g_stats.active_connections++;
}

/* 解析 backend 类型 */
static vox_backend_type_t parse_backend_type(const char* backend_str) {
    if (!backend_str || strcmp(backend_str, "auto") == 0) {
        return VOX_BACKEND_TYPE_AUTO;
    } else if (strcmp(backend_str, "epoll") == 0) {
        return VOX_BACKEND_TYPE_EPOLL;
    } else if (strcmp(backend_str, "io_uring") == 0 || strcmp(backend_str, "iouring") == 0) {
        return VOX_BACKEND_TYPE_IOURING;
    } else if (strcmp(backend_str, "kqueue") == 0) {
        return VOX_BACKEND_TYPE_KQUEUE;
    } else if (strcmp(backend_str, "iocp") == 0) {
        return VOX_BACKEND_TYPE_IOCP;
    } else if (strcmp(backend_str, "select") == 0) {
        return VOX_BACKEND_TYPE_SELECT;
    } else {
        fprintf(stderr, "未知的 backend 类型: %s，使用 auto\n", backend_str);
        return VOX_BACKEND_TYPE_AUTO;
    }
}

/* 统计定时器回调 */
static void stats_timer_cb(vox_timer_t* timer, void* user_data) {
    (void)timer;
    const char* protocol = (const char*)user_data;
    print_stats(protocol);
}

/* 打印统计信息 */
static void print_stats(const char* protocol) {
    vox_time_t now = vox_time_monotonic();
    int64_t elapsed_us = vox_time_diff_us(now, g_stats.start_time);
    int64_t elapsed_sec = elapsed_us / 1000000;
    int64_t interval_us = vox_time_diff_us(now, g_stats.last_stats_time);
    int64_t interval_sec = interval_us / 1000000;
    
    if (interval_sec == 0) {
        interval_sec = 1;
    }
    
    /* 计算速率 */
    uint64_t bytes_received_delta = g_stats.total_bytes_received - g_stats.last_bytes_received;
    uint64_t bytes_sent_delta = g_stats.total_bytes_sent - g_stats.last_bytes_sent;
    uint64_t connections_delta = g_stats.total_connections - g_stats.last_connections;
    
    double recv_mbps = (bytes_received_delta * 8.0) / (interval_sec * 1000000.0);
    double send_mbps = (bytes_sent_delta * 8.0) / (interval_sec * 1000000.0);
    double total_mbps = ((bytes_received_delta + bytes_sent_delta) * 8.0) / (interval_sec * 1000000.0);
    double conn_per_sec = (double)connections_delta / interval_sec;
    
    printf("\n=== %s 服务端性能统计 ===\n", protocol);
    printf("运行时间: %lld 秒\n", (long long)elapsed_sec);
    printf("总连接数: %llu\n", (unsigned long long)g_stats.total_connections);
    printf("活跃连接数: %llu\n", (unsigned long long)g_stats.active_connections);
    printf("总接收: %.2f MB (%.2f Mbps)\n", 
           g_stats.total_bytes_received / 1048576.0,
           (g_stats.total_bytes_received * 8.0) / (elapsed_sec * 1000000.0));
    printf("总发送: %.2f MB (%.2f Mbps)\n",
           g_stats.total_bytes_sent / 1048576.0,
           (g_stats.total_bytes_sent * 8.0) / (elapsed_sec * 1000000.0));
    printf("总吞吐量: %.2f Mbps\n", total_mbps);
    
    if (strcmp(protocol, "UDP") == 0) {
        printf("总数据包数: %llu\n", (unsigned long long)g_stats.total_packets);
        printf("平均包大小: %.2f 字节\n",
               g_stats.total_packets > 0 ? 
               (double)g_stats.total_bytes_received / g_stats.total_packets : 0.0);
    }
    
    printf("连接错误: %llu\n", (unsigned long long)g_stats.connection_errors);
    printf("读取错误: %llu\n", (unsigned long long)g_stats.read_errors);
    printf("写入错误: %llu\n", (unsigned long long)g_stats.write_errors);
    printf("连接速率: %.2f 连接/秒\n", conn_per_sec);
    printf("接收速率: %.2f Mbps\n", recv_mbps);
    printf("发送速率: %.2f Mbps\n", send_mbps);
    printf("活跃句柄数: %zu\n", vox_loop_active_handles(g_loop));
    printf("========================\n");
    
    /* 更新上次统计值 */
    g_stats.last_stats_time = now;
    g_stats.last_bytes_received = g_stats.total_bytes_received;
    g_stats.last_bytes_sent = g_stats.total_bytes_sent;
    g_stats.last_connections = g_stats.total_connections;
}

/* 信号处理 */
static void signal_handler(int sig) {
    (void)sig;
    g_running = false;
    if (g_loop) {
        vox_loop_stop(g_loop);
    }
}

/* 运行 TCP 服务器性能测试 */
static int run_tcp_server(const char* host, int port, const char* backend_str) {
    printf("=== TCP 服务端性能测试 ===\n");
    printf("监听地址: %s:%d\n", host, port);
    
    /* 初始化 socket 库 */
    if (vox_socket_init() != 0) {
        fprintf(stderr, "初始化 socket 库失败\n");
        return -1;
    }
    
    /* 配置 backend */
    vox_backend_type_t backend_type = parse_backend_type(backend_str);
    vox_backend_config_t backend_cfg = {0};
    backend_cfg.type = backend_type;
    backend_cfg.mpool = NULL;  /* 使用 loop 内部创建的内存池 */
    backend_cfg.max_events = 0;  /* 使用默认值 */
    
    vox_loop_config_t loop_cfg = {0};
    loop_cfg.backend_config = &backend_cfg;
    
    /* 创建事件循环 */
    g_loop = vox_loop_create_with_config(&loop_cfg);
    if (!g_loop) {
        fprintf(stderr, "创建事件循环失败\n");
        vox_socket_cleanup();
        return -1;
    }
    
    /* 打印使用的 backend 名称 */
    vox_backend_t* backend = vox_loop_get_backend(g_loop);
    if (backend) {
        const char* backend_name = vox_backend_name(backend);
        printf("使用的 backend: %s\n", backend_name);
    }
    
    /* 创建 TCP 服务器 */
    vox_tcp_t* server = vox_tcp_create(g_loop);
    if (!server) {
        fprintf(stderr, "创建 TCP 服务器失败\n");
        vox_loop_destroy(g_loop);
        vox_socket_cleanup();
        return -1;
    }
    
    /* 解析地址 */
    vox_socket_addr_t addr;
    if (vox_socket_parse_address(host, (uint16_t)port, &addr) != 0) {
        fprintf(stderr, "解析地址失败: %s:%d\n", host, port);
        vox_tcp_destroy(server);
        vox_loop_destroy(g_loop);
        vox_socket_cleanup();
        return -1;
    }
    
    /* 绑定地址 */
    if (vox_tcp_bind(server, &addr, 0) != 0) {
        fprintf(stderr, "绑定地址失败\n");
        vox_tcp_destroy(server);
        vox_loop_destroy(g_loop);
        vox_socket_cleanup();
        return -1;
    }
    
    /* 开始监听 */
    if (vox_tcp_listen(server, 128, tcp_connection_cb) != 0) {
        fprintf(stderr, "监听失败\n");
        vox_tcp_destroy(server);
        vox_loop_destroy(g_loop);
        vox_socket_cleanup();
        return -1;
    }
    
    /* 初始化统计 */
    memset(&g_stats, 0, sizeof(g_stats));
    g_stats.start_time = vox_time_monotonic();
    g_stats.last_stats_time = g_stats.start_time;
    
    /* 创建统计定时器 */
    vox_timer_t stats_timer;
    if (vox_timer_init(&stats_timer, g_loop) == 0) {
        vox_timer_start(&stats_timer, STATS_INTERVAL_SEC * 1000, STATS_INTERVAL_SEC * 1000, stats_timer_cb, (void*)"TCP");
    }
    
    printf("TCP 服务器已启动，等待连接...\n");
    printf("按 Ctrl+C 停止服务器\n\n");
    
    /* 运行事件循环 */
    int ret = vox_loop_run(g_loop, VOX_RUN_DEFAULT);
    
    /* 打印最终统计 */
    printf("\n=== 最终统计 ===\n");
    print_stats("TCP");
    
    /* 清理 */
    if (vox_timer_is_active(&stats_timer)) {
        vox_timer_destroy(&stats_timer);
    }
    vox_tcp_destroy(server);
    vox_loop_destroy(g_loop);
    vox_socket_cleanup();
    
    return ret;
}

/* 运行 UDP 服务器性能测试 */
static int run_udp_server(const char* host, int port, const char* backend_str) {
    printf("=== UDP 服务端性能测试 ===\n");
    printf("监听地址: %s:%d\n", host, port);
    
    /* 初始化 socket 库 */
    if (vox_socket_init() != 0) {
        fprintf(stderr, "初始化 socket 库失败\n");
        return -1;
    }
    
    /* 配置 backend */
    vox_backend_type_t backend_type = parse_backend_type(backend_str);
    vox_backend_config_t backend_cfg = {0};
    backend_cfg.type = backend_type;
    backend_cfg.mpool = NULL;  /* 使用 loop 内部创建的内存池 */
    backend_cfg.max_events = 0;  /* 使用默认值 */
    
    vox_loop_config_t loop_cfg = {0};
    loop_cfg.backend_config = &backend_cfg;
    
    /* 创建事件循环 */
    g_loop = vox_loop_create_with_config(&loop_cfg);
    if (!g_loop) {
        fprintf(stderr, "创建事件循环失败\n");
        vox_socket_cleanup();
        return -1;
    }
    
    /* 打印使用的 backend 名称 */
    vox_backend_t* backend = vox_loop_get_backend(g_loop);
    if (backend) {
        const char* backend_name = vox_backend_name(backend);
        printf("使用的 backend: %s\n", backend_name);
    }
    
    /* 创建 UDP 服务器 */
    vox_udp_t* server = vox_udp_create(g_loop);
    if (!server) {
        fprintf(stderr, "创建 UDP 服务器失败\n");
        vox_loop_destroy(g_loop);
        vox_socket_cleanup();
        return -1;
    }
    
    /* 解析地址 */
    vox_socket_addr_t addr;
    if (vox_socket_parse_address(host, (uint16_t)port, &addr) != 0) {
        fprintf(stderr, "解析地址失败: %s:%d\n", host, port);
        vox_udp_destroy(server);
        vox_loop_destroy(g_loop);
        vox_socket_cleanup();
        return -1;
    }
    
    /* 绑定地址 */
    if (vox_udp_bind(server, &addr, 0) != 0) {
        fprintf(stderr, "绑定地址失败\n");
        vox_udp_destroy(server);
        vox_loop_destroy(g_loop);
        vox_socket_cleanup();
        return -1;
    }
    
    /* 开始接收 */
    if (vox_udp_recv_start(server, udp_alloc_cb, udp_recv_cb) != 0) {
        fprintf(stderr, "开始接收失败\n");
        vox_udp_destroy(server);
        vox_loop_destroy(g_loop);
        vox_socket_cleanup();
        return -1;
    }
    
    /* 初始化统计 */
    memset(&g_stats, 0, sizeof(g_stats));
    g_stats.start_time = vox_time_monotonic();
    g_stats.last_stats_time = g_stats.start_time;
    
    /* 创建统计定时器 */
    vox_timer_t stats_timer;
    if (vox_timer_init(&stats_timer, g_loop) == 0) {
        vox_timer_start(&stats_timer, STATS_INTERVAL_SEC * 1000, STATS_INTERVAL_SEC * 1000, stats_timer_cb, (void*)"UDP");
    }
    
    printf("UDP 服务器已启动，等待数据包...\n");
    printf("按 Ctrl+C 停止服务器\n\n");
    
    /* 运行事件循环 */
    int ret = vox_loop_run(g_loop, VOX_RUN_DEFAULT);
    
    /* 打印最终统计 */
    printf("\n=== 最终统计 ===\n");
    print_stats("UDP");
    
    /* 清理 */
    if (vox_timer_is_active(&stats_timer)) {
        vox_timer_destroy(&stats_timer);
    }
    vox_udp_destroy(server);
    vox_loop_destroy(g_loop);
    vox_socket_cleanup();
    
    return ret;
}

/* 运行 TLS 服务器性能测试 */
static int run_tls_server(const char* host, int port, const char* cert_file, 
                          const char* key_file, const char* backend_str) {
    printf("=== TLS 服务端性能测试 ===\n");
    printf("监听地址: %s:%d\n", host, port);
    printf("证书文件: %s\n", cert_file);
    printf("私钥文件: %s\n", key_file);
    
    /* 初始化 socket 库 */
    if (vox_socket_init() != 0) {
        fprintf(stderr, "初始化 socket 库失败\n");
        return -1;
    }
    
    /* 配置 backend */
    vox_backend_type_t backend_type = parse_backend_type(backend_str);
    vox_backend_config_t backend_cfg = {0};
    backend_cfg.type = backend_type;
    backend_cfg.mpool = NULL;  /* 使用 loop 内部创建的内存池 */
    backend_cfg.max_events = 0;  /* 使用默认值 */
    
    vox_loop_config_t loop_cfg = {0};
    loop_cfg.backend_config = &backend_cfg;
    
    /* 创建事件循环 */
    g_loop = vox_loop_create_with_config(&loop_cfg);
    if (!g_loop) {
        fprintf(stderr, "创建事件循环失败\n");
        vox_socket_cleanup();
        return -1;
    }
    
    /* 打印使用的 backend 名称 */
    vox_backend_t* backend = vox_loop_get_backend(g_loop);
    if (backend) {
        const char* backend_name = vox_backend_name(backend);
        printf("使用的 backend: %s\n", backend_name);
    }
    
    /* 创建 SSL Context */
    vox_mpool_t* mpool = vox_loop_get_mpool(g_loop);
    vox_ssl_context_t* ssl_ctx = vox_ssl_context_create(mpool, VOX_SSL_MODE_SERVER);
    if (!ssl_ctx) {
        fprintf(stderr, "创建 SSL Context 失败\n");
        vox_loop_destroy(g_loop);
        vox_socket_cleanup();
        return -1;
    }
    
    /* 配置 SSL Context */
    vox_ssl_config_t ssl_config = {0};
    ssl_config.cert_file = cert_file;
    ssl_config.key_file = key_file;
    ssl_config.verify_peer = false;  /* 服务器模式不需要验证客户端 */
    
    if (vox_ssl_context_configure(ssl_ctx, &ssl_config) != 0) {
        fprintf(stderr, "配置 SSL Context 失败\n");
        vox_ssl_context_destroy(ssl_ctx);
        vox_loop_destroy(g_loop);
        vox_socket_cleanup();
        return -1;
    }
    
    /* 创建 TLS 服务器 */
    vox_tls_t* server = vox_tls_create(g_loop, ssl_ctx);
    if (!server) {
        fprintf(stderr, "创建 TLS 服务器失败\n");
        vox_ssl_context_destroy(ssl_ctx);
        vox_loop_destroy(g_loop);
        vox_socket_cleanup();
        return -1;
    }
    
    /* 解析地址 */
    vox_socket_addr_t addr;
    if (vox_socket_parse_address(host, (uint16_t)port, &addr) != 0) {
        fprintf(stderr, "解析地址失败: %s:%d\n", host, port);
        vox_tls_destroy(server);
        vox_ssl_context_destroy(ssl_ctx);
        vox_loop_destroy(g_loop);
        vox_socket_cleanup();
        return -1;
    }
    
    /* 绑定地址 */
    if (vox_tls_bind(server, &addr, 0) != 0) {
        fprintf(stderr, "绑定地址失败\n");
        vox_tls_destroy(server);
        vox_ssl_context_destroy(ssl_ctx);
        vox_loop_destroy(g_loop);
        vox_socket_cleanup();
        return -1;
    }
    
    /* 开始监听 */
    if (vox_tls_listen(server, 128, tls_connection_cb) != 0) {
        fprintf(stderr, "监听失败\n");
        vox_tls_destroy(server);
        vox_ssl_context_destroy(ssl_ctx);
        vox_loop_destroy(g_loop);
        vox_socket_cleanup();
        return -1;
    }
    
    /* 初始化统计 */
    memset(&g_stats, 0, sizeof(g_stats));
    g_stats.start_time = vox_time_monotonic();
    g_stats.last_stats_time = g_stats.start_time;
    
    /* 创建统计定时器 */
    vox_timer_t stats_timer;
    if (vox_timer_init(&stats_timer, g_loop) == 0) {
        vox_timer_start(&stats_timer, STATS_INTERVAL_SEC * 1000, STATS_INTERVAL_SEC * 1000, stats_timer_cb, (void*)"TLS");
    }
    
    printf("TLS 服务器已启动，等待连接...\n");
    printf("按 Ctrl+C 停止服务器\n\n");
    
    /* 运行事件循环 */
    int ret = vox_loop_run(g_loop, VOX_RUN_DEFAULT);
    
    /* 打印最终统计 */
    printf("\n=== 最终统计 ===\n");
    print_stats("TLS");
    
    /* 清理 */
    if (vox_timer_is_active(&stats_timer)) {
        vox_timer_destroy(&stats_timer);
    }
    vox_tls_destroy(server);
    vox_ssl_context_destroy(ssl_ctx);
    vox_loop_destroy(g_loop);
    vox_socket_cleanup();
    
    return ret;
}

/* 打印用法 */
static void print_usage(const char* prog_name) {
    printf("用法:\n");
    printf("  TCP 服务器: %s tcp [host] [port] [backend]\n", prog_name);
    printf("  UDP 服务器: %s udp [host] [port] [backend]\n", prog_name);
    printf("  TLS 服务器: %s tls [host] [port] [cert_file] [key_file] [backend]\n", prog_name);
    printf("\n参数:\n");
    printf("  host        - 监听地址（默认: 0.0.0.0）\n");
    printf("  port        - 监听端口（默认: %d）\n", DEFAULT_PORT);
    printf("  cert_file   - TLS 证书文件路径\n");
    printf("  key_file    - TLS 私钥文件路径\n");
    printf("  backend     - Backend 类型（auto/epoll/io_uring/kqueue/iocp/select，默认: auto）\n");
    printf("\n示例:\n");
    printf("  %s tcp 0.0.0.0 9999 epoll\n", prog_name);
    printf("  %s udp 0.0.0.0 9999 io_uring\n", prog_name);
    printf("  %s tls 0.0.0.0 9999 cert/server.crt cert/server.key iocp\n", prog_name);
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }
    
    /* 注册信号处理 */
    signal(SIGINT, signal_handler);
#ifdef VOX_OS_UNIX
    signal(SIGTERM, signal_handler);
#endif
    
    const char* mode = argv[1];
    const char* host = (argc > 2) ? argv[2] : "0.0.0.0";
    int port = (argc > 3) ? atoi(argv[3]) : DEFAULT_PORT;
    const char* backend = (argc > 4) ? argv[4] : "auto";
    
    if (strcmp(mode, "tcp") == 0) {
        return run_tcp_server(host, port, backend);
    } else if (strcmp(mode, "udp") == 0) {
        return run_udp_server(host, port, backend);
    } else if (strcmp(mode, "tls") == 0) {
        const char* cert_file = (argc > 4) ? argv[4] : "server.crt";
        const char* key_file = (argc > 5) ? argv[5] : "server.key";
        const char* tls_backend = (argc > 6) ? argv[6] : "auto";
        return run_tls_server(host, port, cert_file, key_file, tls_backend);
    } else {
        fprintf(stderr, "未知模式: %s\n", mode);
        print_usage(argv[0]);
        return 1;
    }
}
