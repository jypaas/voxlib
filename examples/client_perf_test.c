/*
 * client_perf_test.c - TCP/UDP/TLS 客户端性能测试工具
 * 用于对 server_perf_test 进行压力测试
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
#define DEFAULT_CONNECTIONS 100
#define DEFAULT_DURATION 30  /* 默认测试时长（秒） */
#define BUFFER_SIZE 65536
#define STATS_INTERVAL_SEC 2  /* 统计输出间隔（秒） */

/* 客户端连接数据 */
typedef struct {
    vox_tcp_t* tcp;
    char buffer[BUFFER_SIZE];
    uint64_t bytes_received;
    uint64_t bytes_sent;
    uint64_t packets_sent;
    vox_time_t connect_time;
    bool connected;
    bool closed;
} tcp_client_ctx_t;

typedef struct {
    vox_udp_t* udp;
    vox_socket_addr_t server_addr;
    char buffer[BUFFER_SIZE];
    uint64_t bytes_sent;
    uint64_t packets_sent;
    uint64_t bytes_received;
    uint64_t packets_received;
} udp_client_ctx_t;

typedef struct {
    vox_tls_t* tls;
    char buffer[BUFFER_SIZE];
    uint64_t bytes_received;
    uint64_t bytes_sent;
    vox_time_t connect_time;
    vox_time_t handshake_time;
    bool connected;
    bool closed;
} tls_client_ctx_t;

/* 全局统计 */
typedef struct {
    uint64_t total_connections;
    uint64_t active_connections;
    uint64_t total_bytes_sent;
    uint64_t total_bytes_received;
    uint64_t total_packets_sent;
    uint64_t total_packets_received;
    uint64_t connection_errors;
    uint64_t read_errors;
    uint64_t write_errors;
    vox_time_t start_time;
    vox_time_t last_stats_time;
    uint64_t last_bytes_sent;
    uint64_t last_bytes_received;
} client_stats_t;

/* 全局变量 */
static vox_loop_t* g_loop = NULL;
static client_stats_t g_stats = {0};
static bool g_running = true;
static int g_target_connections = DEFAULT_CONNECTIONS;
static int g_duration = DEFAULT_DURATION;
static int g_packet_size = 1024;
static int g_connections_created = 0;
static char* g_test_data = NULL;

/* 前向声明 */
static void print_stats(const char* protocol);
static void stats_timer_cb(vox_timer_t* timer, void* user_data);
static void signal_handler(int sig);
static vox_backend_type_t parse_backend_type(const char* backend_str);
static void create_tcp_connection(const char* host, int port);
/* TODO: 实现 UDP/TLS 客户端测试后使用 */
VOX_UNUSED_FUNC
static void create_udp_connection(const char* host, int port);
VOX_UNUSED_FUNC
static void create_tls_connection(const char* host, int port, const char* cert_file);

/* TCP 客户端回调 */
static void tcp_client_alloc_cb(vox_tcp_t* tcp, size_t suggested_size,
                                 void* buf, size_t* len, void* user_data) {
    tcp_client_ctx_t* ctx = (tcp_client_ctx_t*)user_data;
    (void)tcp;
    (void)suggested_size;
    *(void**)buf = ctx->buffer;
    *len = sizeof(ctx->buffer);
}

static void tcp_client_read_cb(vox_tcp_t* tcp, ssize_t nread, const void* buf, void* user_data) {
    tcp_client_ctx_t* ctx = (tcp_client_ctx_t*)user_data;
    (void)buf;  /* Echo 服务器，不需要使用接收到的数据内容 */
    
    if (nread < 0) {
        g_stats.read_errors++;
        g_stats.active_connections--;
        ctx->closed = true;
        vox_tcp_read_stop(tcp);
        vox_handle_close((vox_handle_t*)tcp, NULL);
        vox_tcp_destroy(tcp);
        vox_mpool_t* mpool = vox_loop_get_mpool(g_loop);
        vox_mpool_free(mpool, ctx);
        return;
    }
    
    if (nread == 0) {
        g_stats.active_connections--;
        ctx->closed = true;
        vox_tcp_read_stop(tcp);
        vox_handle_close((vox_handle_t*)tcp, NULL);
        vox_tcp_destroy(tcp);
        vox_mpool_t* mpool = vox_loop_get_mpool(g_loop);
        vox_mpool_free(mpool, ctx);
        return;
    }
    
    ctx->bytes_received += (uint64_t)nread;
    g_stats.total_bytes_received += (uint64_t)nread;
    
    /* Echo 测试：收到数据后继续发送 */
    if (ctx->connected && !ctx->closed) {
        size_t send_size = (size_t)g_packet_size;
        if (send_size > sizeof(ctx->buffer)) {
            send_size = sizeof(ctx->buffer);
        }
        if (vox_tcp_write(tcp, g_test_data, send_size, NULL) == 0) {
            ctx->bytes_sent += send_size;
            ctx->packets_sent++;
            g_stats.total_bytes_sent += send_size;
            g_stats.total_packets_sent++;
        } else {
            g_stats.write_errors++;
        }
    }
}

static void tcp_client_connect_cb(vox_tcp_t* tcp, int status, void* user_data) {
    tcp_client_ctx_t* ctx = (tcp_client_ctx_t*)user_data;
    
    if (status != 0) {
        g_stats.connection_errors++;
        g_stats.active_connections--;
        ctx->closed = true;
        vox_tcp_destroy(tcp);
        vox_mpool_t* mpool = vox_loop_get_mpool(g_loop);
        vox_mpool_free(mpool, ctx);
        return;
    }
    
    ctx->connected = true;
    ctx->connect_time = vox_time_monotonic();
    
    /* 开始读取 */
    if (vox_tcp_read_start(tcp, tcp_client_alloc_cb, tcp_client_read_cb) != 0) {
        g_stats.connection_errors++;
        g_stats.active_connections--;
        ctx->closed = true;
        vox_tcp_destroy(tcp);
        vox_mpool_t* mpool = vox_loop_get_mpool(g_loop);
        vox_mpool_free(mpool, ctx);
        return;
    }
    
    /* 发送初始数据 */
    size_t send_size = (size_t)g_packet_size;
    if (send_size > sizeof(ctx->buffer)) {
        send_size = sizeof(ctx->buffer);
    }
    if (vox_tcp_write(tcp, g_test_data, send_size, NULL) == 0) {
        ctx->bytes_sent += send_size;
        ctx->packets_sent++;
        g_stats.total_bytes_sent += send_size;
        g_stats.total_packets_sent++;
    } else {
        g_stats.write_errors++;
    }
}

/* UDP 客户端回调 */
static void udp_client_alloc_cb(vox_udp_t* udp, size_t suggested_size,
                                 void* buf, size_t* len, void* user_data) {
    udp_client_ctx_t* ctx = (udp_client_ctx_t*)user_data;
    (void)udp;
    (void)suggested_size;
    *(void**)buf = ctx->buffer;
    *len = sizeof(ctx->buffer);
}

static void udp_client_recv_cb(vox_udp_t* udp, ssize_t nread,
                               const void* buf,
                               const vox_socket_addr_t* addr,
                               unsigned int flags,
                               void* user_data) {
    udp_client_ctx_t* ctx = (udp_client_ctx_t*)user_data;
    (void)udp;
    (void)buf;  /* Echo 服务器，不需要使用接收到的数据内容 */
    (void)addr;
    (void)flags;
    
    if (nread < 0) {
        g_stats.read_errors++;
        return;
    }
    
    if (nread > 0) {
        ctx->bytes_received += (uint64_t)nread;
        ctx->packets_received++;
        g_stats.total_bytes_received += (uint64_t)nread;
        g_stats.total_packets_received++;
        
        /* Echo 测试：收到数据后继续发送 */
        size_t send_size = (size_t)g_packet_size;
        if (send_size > sizeof(ctx->buffer)) {
            send_size = sizeof(ctx->buffer);
        }
        if (vox_udp_send(udp, g_test_data, send_size, &ctx->server_addr, NULL) == 0) {
            ctx->bytes_sent += send_size;
            ctx->packets_sent++;
            g_stats.total_bytes_sent += send_size;
            g_stats.total_packets_sent++;
        } else {
            g_stats.write_errors++;
        }
    }
}

/* TLS 客户端回调 */
static void tls_client_alloc_cb(vox_tls_t* tls, size_t suggested_size,
                                 void* buf, size_t* len, void* user_data) {
    tls_client_ctx_t* ctx = (tls_client_ctx_t*)user_data;
    (void)tls;
    (void)suggested_size;
    *(void**)buf = ctx->buffer;
    *len = sizeof(ctx->buffer);
}

static void tls_client_read_cb(vox_tls_t* tls, ssize_t nread, const void* buf, void* user_data) {
    tls_client_ctx_t* ctx = (tls_client_ctx_t*)user_data;
    (void)buf;  /* Echo 服务器，不需要使用接收到的数据内容 */
    
    if (nread < 0) {
        g_stats.read_errors++;
        g_stats.active_connections--;
        ctx->closed = true;
        vox_tls_read_stop(tls);
        vox_handle_close((vox_handle_t*)tls, NULL);
        vox_tls_destroy(tls);
        vox_mpool_t* mpool = vox_loop_get_mpool(g_loop);
        vox_mpool_free(mpool, ctx);
        return;
    }
    
    if (nread == 0) {
        g_stats.active_connections--;
        ctx->closed = true;
        vox_tls_read_stop(tls);
        vox_handle_close((vox_handle_t*)tls, NULL);
        vox_tls_destroy(tls);
        vox_mpool_t* mpool = vox_loop_get_mpool(g_loop);
        vox_mpool_free(mpool, ctx);
        return;
    }
    
    ctx->bytes_received += (uint64_t)nread;
    g_stats.total_bytes_received += (uint64_t)nread;
    
    /* Echo 测试：收到数据后继续发送 */
    if (ctx->connected && !ctx->closed) {
        size_t send_size = (size_t)g_packet_size;
        if (send_size > sizeof(ctx->buffer)) {
            send_size = sizeof(ctx->buffer);
        }
        if (vox_tls_write(tls, g_test_data, send_size, NULL) == 0) {
            ctx->bytes_sent += send_size;
            g_stats.total_bytes_sent += send_size;
        } else {
            g_stats.write_errors++;
        }
    }
}

static void tls_client_handshake_cb(vox_tls_t* tls, int status, void* user_data) {
    tls_client_ctx_t* ctx = (tls_client_ctx_t*)user_data;
    
    if (status != 0) {
        g_stats.connection_errors++;
        g_stats.active_connections--;
        ctx->closed = true;
        vox_tls_destroy(tls);
        vox_mpool_t* mpool = vox_loop_get_mpool(g_loop);
        vox_mpool_free(mpool, ctx);
        return;
    }
    
    ctx->handshake_time = vox_time_monotonic();
    ctx->connected = true;
    
    /* 开始读取 */
    if (vox_tls_read_start(tls, tls_client_alloc_cb, tls_client_read_cb) != 0) {
        g_stats.connection_errors++;
        g_stats.active_connections--;
        ctx->closed = true;
        vox_tls_destroy(tls);
        vox_mpool_t* mpool = vox_loop_get_mpool(g_loop);
        vox_mpool_free(mpool, ctx);
        return;
    }
    
    /* 发送初始数据 */
    size_t send_size = (size_t)g_packet_size;
    if (send_size > sizeof(ctx->buffer)) {
        send_size = sizeof(ctx->buffer);
    }
    if (vox_tls_write(tls, g_test_data, send_size, NULL) == 0) {
        ctx->bytes_sent += send_size;
        g_stats.total_bytes_sent += send_size;
    } else {
        g_stats.write_errors++;
    }
}

static void tls_client_connect_cb(vox_tls_t* tls, int status, void* user_data) {
    tls_client_ctx_t* ctx = (tls_client_ctx_t*)user_data;
    
    if (status != 0) {
        g_stats.connection_errors++;
        g_stats.active_connections--;
        ctx->closed = true;
        vox_tls_destroy(tls);
        vox_mpool_t* mpool = vox_loop_get_mpool(g_loop);
        vox_mpool_free(mpool, ctx);
        return;
    }
    
    ctx->connect_time = vox_time_monotonic();
    
    /* 开始 TLS 握手 */
    if (vox_tls_handshake(tls, tls_client_handshake_cb) != 0) {
        g_stats.connection_errors++;
        g_stats.active_connections--;
        ctx->closed = true;
        vox_tls_destroy(tls);
        vox_mpool_t* mpool = vox_loop_get_mpool(g_loop);
        vox_mpool_free(mpool, ctx);
        return;
    }
}

/* 创建 TCP 连接的辅助数据结构 */
typedef struct {
    const char* host;
    int port;
} conn_timer_data_t;

/* 创建 TCP 连接 */
static void create_tcp_connection(const char* host, int port) {
    vox_tcp_t* client = vox_tcp_create(g_loop);
    if (!client) {
        g_stats.connection_errors++;
        return;
    }
    
    vox_socket_addr_t addr;
    if (vox_socket_parse_address(host, (uint16_t)port, &addr) != 0) {
        g_stats.connection_errors++;
        vox_tcp_destroy(client);
        return;
    }
    
    vox_mpool_t* mpool = vox_loop_get_mpool(g_loop);
    tcp_client_ctx_t* ctx = (tcp_client_ctx_t*)vox_mpool_alloc(mpool, sizeof(tcp_client_ctx_t));
    if (!ctx) {
        vox_tcp_destroy(client);
        return;
    }
    
    memset(ctx, 0, sizeof(tcp_client_ctx_t));
    ctx->tcp = client;
    
    vox_handle_set_data((vox_handle_t*)client, ctx);
    
    if (vox_tcp_connect(client, &addr, tcp_client_connect_cb) == 0) {
        g_stats.total_connections++;
        g_stats.active_connections++;
        g_connections_created++;
        /* 调试信息：显示连接创建进度 */
        if (g_connections_created % 10 == 0 || g_connections_created == g_target_connections) {
            printf("[连接进度] 已创建 %d/%d 个连接\n", g_connections_created, g_target_connections);
        }
    } else {
        g_stats.connection_errors++;
        vox_mpool_free(mpool, ctx);
        vox_tcp_destroy(client);
    }
}

/* 创建 UDP 连接 */
/* TODO: 实现 UDP 客户端测试 */
VOX_UNUSED_FUNC
static void create_udp_connection(const char* host, int port) {
    vox_udp_t* client = vox_udp_create(g_loop);
    if (!client) {
        g_stats.connection_errors++;
        return;
    }
    
    vox_socket_addr_t addr;
    if (vox_socket_parse_address(host, (uint16_t)port, &addr) != 0) {
        g_stats.connection_errors++;
        vox_udp_destroy(client);
        return;
    }
    
    vox_mpool_t* mpool = vox_loop_get_mpool(g_loop);
    udp_client_ctx_t* ctx = (udp_client_ctx_t*)vox_mpool_alloc(mpool, sizeof(udp_client_ctx_t));
    if (!ctx) {
        vox_udp_destroy(client);
        return;
    }
    
    memset(ctx, 0, sizeof(udp_client_ctx_t));
    ctx->udp = client;
    ctx->server_addr = addr;
    
    vox_handle_set_data((vox_handle_t*)client, ctx);
    
    if (vox_udp_recv_start(client, udp_client_alloc_cb, udp_client_recv_cb) == 0) {
        /* 发送初始数据 */
        size_t send_size = (size_t)g_packet_size;
        if (send_size > sizeof(ctx->buffer)) {
            send_size = sizeof(ctx->buffer);
        }
        if (vox_udp_send(client, g_test_data, send_size, &addr, NULL) == 0) {
            ctx->bytes_sent += send_size;
            ctx->packets_sent++;
            g_stats.total_bytes_sent += send_size;
            g_stats.total_packets_sent++;
            g_stats.total_connections++;
            g_stats.active_connections++;
            g_connections_created++;
        } else {
            g_stats.write_errors++;
            vox_mpool_free(mpool, ctx);
            vox_udp_destroy(client);
        }
    } else {
        g_stats.connection_errors++;
        vox_mpool_free(mpool, ctx);
        vox_udp_destroy(client);
    }
}

/* 创建 TLS 连接 */
/* TODO: 实现 TLS 客户端测试 */
VOX_UNUSED_FUNC
static void create_tls_connection(const char* host, int port, const char* cert_file) {
    vox_mpool_t* mpool = vox_loop_get_mpool(g_loop);
    
    vox_ssl_context_t* ssl_ctx = vox_ssl_context_create(mpool, VOX_SSL_MODE_CLIENT);
    if (!ssl_ctx) {
        g_stats.connection_errors++;
        return;
    }
    
    vox_ssl_config_t ssl_config = {0};
    ssl_config.verify_peer = false;  /* 测试时可以不验证证书 */
    if (cert_file) {
        ssl_config.ca_file = cert_file;
    }
    
    if (vox_ssl_context_configure(ssl_ctx, &ssl_config) != 0) {
        vox_ssl_context_destroy(ssl_ctx);
        g_stats.connection_errors++;
        return;
    }
    
    vox_tls_t* client = vox_tls_create(g_loop, ssl_ctx);
    if (!client) {
        vox_ssl_context_destroy(ssl_ctx);
        g_stats.connection_errors++;
        return;
    }
    
    vox_socket_addr_t addr;
    if (vox_socket_parse_address(host, (uint16_t)port, &addr) != 0) {
        vox_tls_destroy(client);
        vox_ssl_context_destroy(ssl_ctx);
        g_stats.connection_errors++;
        return;
    }
    
    tls_client_ctx_t* ctx = (tls_client_ctx_t*)vox_mpool_alloc(mpool, sizeof(tls_client_ctx_t));
    if (!ctx) {
        vox_tls_destroy(client);
        vox_ssl_context_destroy(ssl_ctx);
        return;
    }
    
    memset(ctx, 0, sizeof(tls_client_ctx_t));
    ctx->tls = client;
    
    vox_handle_set_data((vox_handle_t*)client, ctx);
    
    if (vox_tls_connect(client, &addr, tls_client_connect_cb) == 0) {
        g_stats.total_connections++;
        g_stats.active_connections++;
        g_connections_created++;
    } else {
        g_stats.connection_errors++;
        vox_mpool_free(mpool, ctx);
        vox_tls_destroy(client);
        vox_ssl_context_destroy(ssl_ctx);
    }
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
        return VOX_BACKEND_TYPE_AUTO;
    }
}

/* 统计定时器回调 */
static void stats_timer_cb(vox_timer_t* timer, void* user_data) {
    (void)timer;
    const char* protocol = (const char*)user_data;
    print_stats(protocol);
}

/* 测试时长定时器回调 */
static void duration_timer_cb(vox_timer_t* timer, void* user_data) {
    (void)timer;
    (void)user_data;
    printf("\n测试时长已到，停止测试...\n");
    g_running = false;
    if (g_loop) {
        vox_loop_stop(g_loop);
    }
}

/* 连接定时器回调：分批创建连接 */
static void conn_timer_cb(vox_timer_t* timer, void* user_data) {
    (void)timer;
    conn_timer_data_t* conn_data = (conn_timer_data_t*)user_data;
    
    /* 每批创建 10 个连接，直到达到目标连接数 */
    int batch_size = 10;
    int remaining = g_target_connections - g_connections_created;
    
    if (remaining <= 0) {
        /* 已达到目标连接数，停止定时器 */
        printf("[连接完成] 已创建所有 %d 个连接\n", g_target_connections);
        vox_timer_stop(timer);
        return;
    }
    
    int to_create = (remaining < batch_size) ? remaining : batch_size;
    /* 调试信息：显示正在创建连接 */
    if (g_connections_created > 0) {
        printf("[连接进度] 正在创建 %d 个连接 (%d/%d)\n", 
               to_create, g_connections_created, g_target_connections);
    }
    for (int i = 0; i < to_create; i++) {
        create_tcp_connection(conn_data->host, conn_data->port);
    }
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
    
    uint64_t bytes_sent_delta = g_stats.total_bytes_sent - g_stats.last_bytes_sent;
    uint64_t bytes_received_delta = g_stats.total_bytes_received - g_stats.last_bytes_received;
    
    double send_mbps = (bytes_sent_delta * 8.0) / (interval_sec * 1000000.0);
    double recv_mbps = (bytes_received_delta * 8.0) / (interval_sec * 1000000.0);
    double total_mbps = ((bytes_sent_delta + bytes_received_delta) * 8.0) / (interval_sec * 1000000.0);
    
    printf("\n=== %s 客户端性能统计 ===\n", protocol);
    printf("运行时间: %lld 秒\n", (long long)elapsed_sec);
    printf("总连接数: %llu\n", (unsigned long long)g_stats.total_connections);
    printf("活跃连接数: %llu\n", (unsigned long long)g_stats.active_connections);
    printf("总发送: %.2f MB (%.2f Mbps)\n",
           g_stats.total_bytes_sent / 1048576.0,
           (g_stats.total_bytes_sent * 8.0) / (elapsed_sec * 1000000.0));
    printf("总接收: %.2f MB (%.2f Mbps)\n",
           g_stats.total_bytes_received / 1048576.0,
           (g_stats.total_bytes_received * 8.0) / (elapsed_sec * 1000000.0));
    
    if (strcmp(protocol, "UDP") == 0) {
        printf("总发送包数: %llu\n", (unsigned long long)g_stats.total_packets_sent);
        printf("总接收包数: %llu\n", (unsigned long long)g_stats.total_packets_received);
    }
    
    printf("连接错误: %llu\n", (unsigned long long)g_stats.connection_errors);
    printf("读取错误: %llu\n", (unsigned long long)g_stats.read_errors);
    printf("写入错误: %llu\n", (unsigned long long)g_stats.write_errors);
    printf("发送速率: %.2f Mbps\n", send_mbps);
    printf("接收速率: %.2f Mbps\n", recv_mbps);
    printf("总吞吐量: %.2f Mbps\n", total_mbps);
    printf("========================\n");
    
    g_stats.last_stats_time = now;
    g_stats.last_bytes_sent = g_stats.total_bytes_sent;
    g_stats.last_bytes_received = g_stats.total_bytes_received;
}

/* 信号处理 */
static void signal_handler(int sig) {
    (void)sig;
    g_running = false;
    if (g_loop) {
        vox_loop_stop(g_loop);
    }
}

/* 运行 TCP 客户端测试 */
static int run_tcp_client(const char* host, int port, int connections, int duration,
                          int packet_size, const char* backend_str) {
    printf("=== TCP 客户端性能测试 ===\n");
    printf("目标服务器: %s:%d\n", host, port);
    printf("并发连接数: %d\n", connections);
    printf("测试时长: %d 秒\n", duration);
    printf("数据包大小: %d 字节\n", packet_size);
    printf("目标连接数: %d\n", connections);
    
    if (vox_socket_init() != 0) {
        fprintf(stderr, "初始化 socket 库失败\n");
        return -1;
    }
    
    /* 配置 backend */
    vox_backend_type_t backend_type = parse_backend_type(backend_str);
    vox_backend_config_t backend_cfg = {0};
    backend_cfg.type = backend_type;
    backend_cfg.mpool = NULL;
    backend_cfg.max_events = 0;
    
    vox_loop_config_t loop_cfg = {0};
    loop_cfg.backend_config = &backend_cfg;
    
    g_loop = vox_loop_create_with_config(&loop_cfg);
    if (!g_loop) {
        fprintf(stderr, "创建事件循环失败\n");
        vox_socket_cleanup();
        return -1;
    }
    
    /* 准备测试数据（使用内存池） */
    vox_mpool_t* mpool = vox_loop_get_mpool(g_loop);
    g_test_data = (char*)vox_mpool_alloc(mpool, (size_t)packet_size);
    if (!g_test_data) {
        fprintf(stderr, "分配测试数据失败\n");
        vox_loop_destroy(g_loop);
        vox_socket_cleanup();
        return -1;
    }
    for (int i = 0; i < packet_size; i++) {
        g_test_data[i] = (char)('A' + (i % 26));
    }
    
    /* 初始化统计 */
    memset(&g_stats, 0, sizeof(g_stats));
    g_stats.start_time = vox_time_monotonic();
    g_stats.last_stats_time = g_stats.start_time;
    g_target_connections = connections;
    g_duration = duration;
    g_packet_size = packet_size;
    g_connections_created = 0;
    
    /* 创建统计定时器 */
    vox_timer_t stats_timer;
    vox_timer_t duration_timer;
    vox_timer_t conn_timer;
    
    if (vox_timer_init(&stats_timer, g_loop) == 0) {
        vox_timer_start(&stats_timer, STATS_INTERVAL_SEC * 1000, STATS_INTERVAL_SEC * 1000, stats_timer_cb, (void*)"TCP");
    }
    
    /* 创建测试时长定时器 */
    if (duration > 0) {
        if (vox_timer_init(&duration_timer, g_loop) == 0) {
            /* 在指定时长后停止测试 */
            vox_timer_start(&duration_timer, duration * 1000, 0, duration_timer_cb, NULL);
        }
    }
    
    /* 创建连接定时器：分批创建连接 */
    /* 使用静态变量确保数据在定时器回调期间有效 */
    static conn_timer_data_t conn_data;
    conn_data.host = host;
    conn_data.port = port;
    
    if (vox_timer_init(&conn_timer, g_loop) == 0) {
        /* 每 100ms 创建一批连接 */
        vox_timer_start(&conn_timer, 100, 100, conn_timer_cb, &conn_data);
    }
    
    /* 立即创建第一批连接（最多 10 个） */
    int initial_batch = (connections < 10) ? connections : 10;
    for (int i = 0; i < initial_batch; i++) {
        create_tcp_connection(host, port);
    }
    
    printf("开始测试...\n");
    
    /* 运行事件循环 */
    int ret = vox_loop_run(g_loop, VOX_RUN_DEFAULT);
    
    /* 打印最终统计 */
    printf("\n=== 最终统计 ===\n");
    print_stats("TCP");
    
    /* 清理 */
    if (vox_timer_is_active(&stats_timer)) {
        vox_timer_destroy(&stats_timer);
    }
    if (duration > 0 && vox_timer_is_active(&duration_timer)) {
        vox_timer_destroy(&duration_timer);
    }
    if (vox_timer_is_active(&conn_timer)) {
        vox_timer_destroy(&conn_timer);
    }
    /* 测试数据由内存池管理，随 loop 销毁自动释放 */
    vox_loop_destroy(g_loop);
    vox_socket_cleanup();
    
    return ret;
}

/* 打印用法 */
static void print_usage(const char* prog_name) {
    printf("用法:\n");
    printf("  TCP 客户端: %s tcp <host> <port> [connections] [duration] [packet_size] [backend]\n", prog_name);
    printf("  UDP 客户端: %s udp <host> <port> [connections] [duration] [packet_size] [backend]\n", prog_name);
    printf("  TLS 客户端: %s tls <host> <port> [connections] [duration] [packet_size] [cert_file] [backend]\n", prog_name);
    printf("\n参数:\n");
    printf("  host         - 服务器地址\n");
    printf("  port         - 服务器端口\n");
    printf("  connections  - 并发连接数（默认: %d）\n", DEFAULT_CONNECTIONS);
    printf("  duration     - 测试时长（秒，默认: %d）\n", DEFAULT_DURATION);
    printf("  packet_size  - 数据包大小（字节，默认: 1024）\n");
    printf("  cert_file    - TLS CA 证书文件（可选）\n");
    printf("  backend      - Backend 类型（auto/epoll/io_uring/kqueue/iocp/select，默认: auto）\n");
    printf("\n示例:\n");
    printf("  %s tcp 127.0.0.1 9999 100 30 1024 epoll\n", prog_name);
    printf("  %s udp 127.0.0.1 9999 50 60 2048 io_uring\n", prog_name);
    printf("  %s tls 127.0.0.1 9999 100 30 1024 ca.crt iocp\n", prog_name);
}

int main(int argc, char* argv[]) {
    if (argc < 4) {
        print_usage(argv[0]);
        return 1;
    }
    
    signal(SIGINT, signal_handler);
#ifdef VOX_OS_UNIX
    signal(SIGTERM, signal_handler);
#endif
    
    const char* mode = argv[1];
    const char* host = argv[2];
    int port = atoi(argv[3]);
    int connections = (argc > 4) ? atoi(argv[4]) : DEFAULT_CONNECTIONS;
    int duration = (argc > 5) ? atoi(argv[5]) : DEFAULT_DURATION;
    int packet_size = (argc > 6) ? atoi(argv[6]) : 1024;
    
    if (strcmp(mode, "tcp") == 0) {
        const char* backend = (argc > 7) ? argv[7] : "auto";
        return run_tcp_client(host, port, connections, duration, packet_size, backend);
    } else if (strcmp(mode, "udp") == 0) {
        const char* backend = (argc > 7) ? argv[7] : "auto";
        (void)backend;  /* TODO: 实现 UDP 客户端测试后使用 */
        /* TODO: 实现 UDP 客户端测试 */
        fprintf(stderr, "UDP 客户端测试暂未实现\n");
        return 1;
    } else if (strcmp(mode, "tls") == 0) {
        const char* cert_file = (argc > 7) ? argv[7] : NULL;
        const char* backend = (argc > 8) ? argv[8] : "auto";
        (void)cert_file;  /* TODO: 实现 TLS 客户端测试后使用 */
        (void)backend;    /* TODO: 实现 TLS 客户端测试后使用 */
        /* TODO: 实现 TLS 客户端测试 */
        fprintf(stderr, "TLS 客户端测试暂未实现\n");
        return 1;
    } else {
        fprintf(stderr, "未知模式: %s\n", mode);
        print_usage(argv[0]);
        return 1;
    }
}
