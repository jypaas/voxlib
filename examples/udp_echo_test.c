/*
 * udp_echo_test.c - UDP Echo 服务器和客户端测试
 * 演示使用异步 IO 框架实现 UDP Echo 服务器
 */

#include "vox_loop.h"
#include "vox_udp.h"
#include "vox_socket.h"
#include "vox_log.h"
#include "vox_mpool.h"
#include "vox_backend.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

#define ECHO_PORT 8889
#define BUFFER_SIZE 65536

/* 全局变量 */
static vox_loop_t* g_loop = NULL;
static int g_packet_count = 0;

/* 客户端数据 */
typedef struct {
    char buffer[BUFFER_SIZE];
    const char* message;
    bool received;
    vox_loop_t* loop;
} client_ctx_t;

/* 客户端缓冲区分配回调 */
static void client_alloc_cb(vox_udp_t* udp, size_t suggested_size, 
                           void* buf, size_t* len, void* user_data) {
    client_ctx_t* ctx = (client_ctx_t*)user_data;
    (void)udp;
    (void)suggested_size;
    *(void**)buf = ctx->buffer;
    *len = sizeof(ctx->buffer);
}

/* 客户端接收回调 */
static void client_recv_cb(vox_udp_t* udp, ssize_t nread, 
                          const void* buf, 
                          const vox_socket_addr_t* addr,
                          unsigned int flags,
                          void* user_data) {
    client_ctx_t* ctx = (client_ctx_t*)user_data;
    (void)udp;
    (void)flags;
    
    if (nread < 0) {
        printf("接收错误: %zd\n", nread);
        vox_mpool_t* mpool = vox_loop_get_mpool(ctx->loop);
        vox_mpool_free(mpool, ctx);
        vox_loop_stop(ctx->loop);
        return;
    }
    
    if (nread == 0) {
        printf("收到空数据包\n");
        vox_mpool_t* mpool = vox_loop_get_mpool(ctx->loop);
        vox_mpool_free(mpool, ctx);
        vox_loop_stop(ctx->loop);
        return;
    }
    
    /* 显示服务器地址信息 */
    if (addr) {
        char server_ip[64];
        vox_socket_address_to_string(addr, server_ip, sizeof(server_ip));
        uint16_t server_port = vox_socket_get_port(addr);
        printf("[服务器 %s:%d] 收到 Echo 响应: %.*s\n", 
               server_ip, server_port, (int)nread, (const char*)buf);
    } else {
        printf("收到 Echo 响应: %.*s\n", (int)nread, (const char*)buf);
    }
    ctx->received = true;

    /* 收到响应后销毁句柄并退出（vox_udp_destroy 内部会调用 recv_stop 和 handle_close） */
    vox_loop_t* loop = ctx->loop;
    vox_mpool_t* mpool = vox_loop_get_mpool(loop);
    vox_udp_destroy(udp);
    vox_mpool_free(mpool, ctx);
    vox_loop_stop(loop);
}

/* 缓冲区分配回调 */
static void alloc_callback(vox_udp_t* udp, size_t suggested_size, 
                          void* buf, size_t* len, void* user_data) {
    static char recv_buffer[BUFFER_SIZE];
    (void)udp;
    (void)suggested_size;
    (void)user_data;
    
    *(void**)buf = recv_buffer;
    *len = sizeof(recv_buffer);
}

/* 接收回调 - Echo 服务器核心逻辑 */
static void recv_callback(vox_udp_t* udp, ssize_t nread, 
                         const void* buf, 
                         const vox_socket_addr_t* addr,
                         unsigned int flags,
                         void* user_data) {
    (void)user_data;
    (void)flags;
    
    if (nread < 0) {
        /* 接收错误 */
        printf("[UDP] 接收错误: %zd\n", nread);
        return;
    }
    
    if (nread == 0) {
        /* 空数据包 */
        return;
    }
    
    /* 显示接收信息 */
    char ip[64];
    vox_socket_address_to_string(addr, ip, sizeof(ip));
    uint16_t port = vox_socket_get_port(addr);
    printf("[UDP] 从 %s:%d 收到 %zd 字节 (总包数: %d)\n", 
           ip, port, nread, ++g_packet_count);
    
    /* 限制输出长度 */
    int print_len = nread > 64 ? 64 : (int)nread;
    printf("      数据: %.*s%s\n", print_len, (const char*)buf, 
           nread > 64 ? "..." : "");
    
    /* Echo: 将收到的数据原样发送回去 */
    if (vox_udp_send(udp, buf, (size_t)nread, addr, NULL) != 0) {
        printf("[UDP] 发送失败\n");
    } else {
        printf("[UDP] 已发送 Echo 响应到 %s:%d\n", ip, port);
    }
}

/* 信号处理 */
static void signal_handler(int sig) {
    (void)sig;
    if (g_loop) {
        printf("\n收到信号，停止服务器...\n");
        vox_loop_stop(g_loop);
    }
}

/* UDP Echo 服务器 */
int udp_echo_server(const char* host, uint16_t port, vox_backend_type_t backend_type) {
    printf("=== UDP Echo 服务器 ===\n");
    printf("监听地址: %s:%d\n", host, port);
    printf("按 Ctrl+C 停止服务器\n\n");
    
    /* 创建事件循环（使用配置接口） */
    
    /* 配置 backend */
    vox_backend_config_t backend_config = {0};
    backend_config.type = backend_type;
    backend_config.mpool = NULL;  /* 使用 loop 内部创建的内存池 */
    backend_config.max_events = 0;  /* 使用默认值 */
    
    /* 配置 loop */
    vox_loop_config_t loop_config = {0};
    loop_config.backend_config = &backend_config;
    /* 其他配置使用默认值（NULL） */
    
    g_loop = vox_loop_create_with_config(&loop_config);
    if (!g_loop) {
        fprintf(stderr, "创建事件循环失败\n");
        return 1;
    }
    
    /* 打印使用的 backend 名称 */
    vox_backend_t* backend = vox_loop_get_backend(g_loop);
    if (backend) {
        const char* backend_name = vox_backend_name(backend);
        printf("使用的 backend: %s\n", backend_name);
    } else {
        printf("警告: 无法获取 backend 信息\n");
    }
    
    /* 创建服务器 UDP 句柄（vox_udp_create 内部已调用 vox_udp_init） */
    vox_udp_t* server = vox_udp_create(g_loop);
    if (!server) {
        fprintf(stderr, "创建服务器句柄失败\n");
        vox_loop_destroy(g_loop);
        return 1;
    }
    
    /* 解析地址 */
    vox_socket_addr_t addr;
    if (vox_socket_parse_address(host, port, &addr) != 0) {
        fprintf(stderr, "解析地址失败: %s:%d\n", host, port);
        vox_udp_destroy(server);
        vox_loop_destroy(g_loop);
        return 1;
    }
    
    /* 绑定地址 */
    if (vox_udp_bind(server, &addr, 0) != 0) {
        fprintf(stderr, "绑定地址失败\n");
        vox_udp_destroy(server);
        vox_loop_destroy(g_loop);
        return 1;
    }
    
    printf("服务器已启动，等待数据包...\n\n");
    
    /* 开始接收 */
    printf("[DEBUG UDP] 服务器开始接收前，backend_events=0x%x\n", server->backend_events);
    if (vox_udp_recv_start(server, alloc_callback, recv_callback) != 0) {
        fprintf(stderr, "开始接收失败\n");
        vox_udp_destroy(server);
        vox_loop_destroy(g_loop);
        return 1;
    }
    printf("[DEBUG UDP] 服务器开始接收后，backend_events=0x%x\n", server->backend_events);
    
    /* 注册信号处理 */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    /* 运行事件循环 */
    int ret = vox_loop_run(g_loop, VOX_RUN_DEFAULT);
    
    printf("\n服务器停止，总接收包数: %d\n", g_packet_count);
    
    /* 清理 */
    vox_udp_destroy(server);
    vox_loop_destroy(g_loop);
    
    return ret;
}

/* UDP Echo 客户端 */
int udp_echo_client(const char* host, uint16_t port, const char* message, vox_backend_type_t backend_type) {
    printf("=== UDP Echo 客户端 ===\n");
    printf("发送到: %s:%d\n", host, port);
    
    /* 创建事件循环（使用配置接口） */
    
    /* 配置 backend */
    vox_backend_config_t backend_config = {0};
    backend_config.type = backend_type;
    backend_config.mpool = NULL;  /* 使用 loop 内部创建的内存池 */
    backend_config.max_events = 0;  /* 使用默认值 */
    
    /* 配置 loop */
    vox_loop_config_t loop_config = {0};
    loop_config.backend_config = &backend_config;
    /* 其他配置使用默认值（NULL） */
    
    vox_loop_t* loop = vox_loop_create_with_config(&loop_config);
    if (!loop) {
        fprintf(stderr, "创建事件循环失败\n");
        return 1;
    }
    
    /* 打印使用的 backend 名称 */
    vox_backend_t* backend = vox_loop_get_backend(loop);
    if (backend) {
        const char* backend_name = vox_backend_name(backend);
        printf("使用的 backend: %s\n", backend_name);
    } else {
        printf("警告: 无法获取 backend 信息\n");
    }
    
    /* 创建客户端 UDP 句柄（vox_udp_create 内部已调用 vox_udp_init） */
    vox_udp_t* client = vox_udp_create(loop);
    if (!client) {
        fprintf(stderr, "创建客户端句柄失败\n");
        vox_loop_destroy(loop);
        return 1;
    }
    
    /* 解析服务器地址 */
    vox_socket_addr_t addr;
    if (vox_socket_parse_address(host, port, &addr) != 0) {
        fprintf(stderr, "解析地址失败: %s:%d\n", host, port);
        vox_udp_destroy(client);
        vox_loop_destroy(loop);
        return 1;
    }
    
    /* 为客户端绑定一个本地地址 */
    vox_socket_addr_t bind_addr;
    if (vox_socket_parse_address("0.0.0.0", 0, &bind_addr) != 0) {  /* 使用0端口让系统自动分配 */
        fprintf(stderr, "解析本地绑定地址失败\n");
        vox_udp_destroy(client);
        vox_loop_destroy(loop);
        return 1;
    }
    
    if (vox_udp_bind(client, &bind_addr, 0) != 0) {
        fprintf(stderr, "绑定本地地址失败\n");
        vox_udp_destroy(client);
        vox_loop_destroy(loop);
        return 1;
    }
    
    /* 分配客户端数据（使用内存池） */
    vox_mpool_t* mpool = vox_loop_get_mpool(loop);
    client_ctx_t* ctx = (client_ctx_t*)vox_mpool_alloc(mpool, sizeof(client_ctx_t));
    if (!ctx) {
        fprintf(stderr, "分配客户端上下文失败\n");
        vox_udp_destroy(client);
        vox_loop_destroy(loop);
        return 1;
    }
    memset(ctx, 0, sizeof(client_ctx_t));
    ctx->message = message;
    ctx->received = false;
    ctx->loop = loop;
    
    /* 设置用户数据 */
    vox_handle_set_data((vox_handle_t*)client, ctx);
    
    /* 开始接收 */
    g_loop = loop;
    printf("[DEBUG UDP] 开始接收前，backend_events=0x%x\n", client->backend_events);
    if (vox_udp_recv_start(client, client_alloc_cb, client_recv_cb) != 0) {
        fprintf(stderr, "开始接收失败\n");
        vox_mpool_free(mpool, ctx);
        vox_udp_destroy(client);
        vox_loop_destroy(loop);
        return 1;
    }
    printf("[DEBUG UDP] 开始接收后，backend_events=0x%x\n", client->backend_events);
    
    /* 发送消息 */
    printf("发送消息: %s\n", message);
    printf("[DEBUG UDP] 发送前，backend_events=0x%x\n", client->backend_events);
    g_loop = loop;
    if (vox_udp_send(client, message, strlen(message), &addr, NULL) != 0) {
        fprintf(stderr, "发送失败\n");
        vox_mpool_free(mpool, ctx);
        vox_udp_destroy(client);
        vox_loop_destroy(loop);
        return 1;
    }
    printf("[DEBUG UDP] 发送后，backend_events=0x%x\n", client->backend_events);
    
    /* 运行事件循环 */
    int ret = vox_loop_run(loop, VOX_RUN_DEFAULT);
    
    /* 清理 */
    if (!ctx->received) {
        vox_mpool_free(mpool, ctx);
    }
    vox_udp_destroy(client);
    vox_loop_destroy(loop);
    
    return ret;
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

/* 获取 backend 类型名称 */
static const char* get_backend_type_name(vox_backend_type_t type) {
    switch (type) {
        case VOX_BACKEND_TYPE_AUTO: return "auto";
        case VOX_BACKEND_TYPE_EPOLL: return "epoll";
        case VOX_BACKEND_TYPE_IOURING: return "io_uring";
        case VOX_BACKEND_TYPE_KQUEUE: return "kqueue";
        case VOX_BACKEND_TYPE_IOCP: return "iocp";
        case VOX_BACKEND_TYPE_SELECT: return "select";
        default: return "unknown";
    }
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        printf("用法:\n");
        printf("  服务器: %s server [host] [port] [backend]\n", argv[0]);
        printf("  客户端: %s client <host> <port> [message] [backend]\n", argv[0]);
        printf("\nBackend 类型:\n");
        printf("  auto     - 自动选择（默认）\n");
        printf("  epoll    - Linux epoll\n");
        printf("  io_uring - Linux io_uring\n");
        printf("  kqueue   - macOS/BSD kqueue\n");
        printf("  iocp     - Windows IOCP\n");
        printf("  select   - select（跨平台兜底方案）\n");
        printf("\n示例:\n");
        printf("  %s server 0.0.0.0 8889 kqueue\n", argv[0]);
        printf("  %s client 127.0.0.1 8889 \"Hello, UDP Echo!\" kqueue\n", argv[0]);
        return 1;
    }
    
    /* 初始化 socket 库 */
    if (vox_socket_init() != 0) {
        fprintf(stderr, "初始化 socket 库失败\n");
        return 1;
    }
    
    if (strcmp(argv[1], "server") == 0) {
        const char* host = argc > 2 ? argv[2] : "0.0.0.0";
        uint16_t port = argc > 3 ? (uint16_t)atoi(argv[3]) : ECHO_PORT;
        const char* backend_str = argc > 4 ? argv[4] : "auto";
        vox_backend_type_t backend_type = parse_backend_type(backend_str);
        printf("指定 backend 类型: %s\n", get_backend_type_name(backend_type));
        int ret = udp_echo_server(host, port, backend_type);
        vox_socket_cleanup();
        return ret;
    } else if (strcmp(argv[1], "client") == 0) {
        if (argc < 4) {
            fprintf(stderr, "客户端需要指定 host 和 port\n");
            vox_socket_cleanup();
            return 1;
        }
        const char* host = argv[2];
        uint16_t port = (uint16_t)atoi(argv[3]);
        const char* message = argc > 4 ? argv[4] : "Hello, UDP Echo Server!";
        const char* backend_str = argc > 5 ? argv[5] : "auto";
        vox_backend_type_t backend_type = parse_backend_type(backend_str);
        printf("指定 backend 类型: %s\n", get_backend_type_name(backend_type));
        int ret = udp_echo_client(host, port, message, backend_type);
        vox_socket_cleanup();
        return ret;
    } else {
        fprintf(stderr, "未知模式: %s\n", argv[1]);
        vox_socket_cleanup();
        return 1;
    }
}
