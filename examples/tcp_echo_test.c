/*
 * tcp_echo_test.c - TCP Echo 服务器和客户端测试
 * 演示使用异步 IO 框架实现 TCP Echo 服务器
 */

#include "vox_loop.h"
#include "vox_tcp.h"
#include "vox_socket.h"
#include "vox_log.h"
#include "vox_mpool.h"
#include "vox_os.h"  /* 包含平台特定头文件 */
#include "vox_backend.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

#define ECHO_PORT 8888
#define BUFFER_SIZE 4096

/* 全局变量 */
static vox_loop_t* g_loop = NULL;
static int g_client_count = 0;

/* 服务器端客户端连接信息 */
typedef struct {
    vox_tcp_t* tcp;
    char buffer[BUFFER_SIZE];
    char client_ip[64];  /* 客户端 IP 地址字符串 */
    uint16_t client_port;  /* 客户端端口 */
} client_data_t;

/* 客户端数据 */
typedef struct {
    char buffer[BUFFER_SIZE];
    const char* message;
    vox_loop_t* loop;
} client_ctx_t;

/* 前向声明 */
static void client_connect_cb(vox_tcp_t* tcp, int status, void* user_data);
static void client_alloc_cb(vox_tcp_t* tcp, size_t suggested_size, 
                           void* buf, size_t* len, void* user_data);
static void client_read_cb(vox_tcp_t* tcp, ssize_t nread, const void* buf, void* user_data);

/* 缓冲区分配回调 */
static void alloc_callback(vox_tcp_t* tcp, size_t suggested_size, 
                          void* buf, size_t* len, void* user_data) {
    client_data_t* data = (client_data_t*)user_data;
    (void)tcp;
    (void)suggested_size;
    
    *(void**)buf = data->buffer;
    *len = sizeof(data->buffer);
}

/* 读取回调 - Echo 服务器核心逻辑 */
static void read_callback(vox_tcp_t* tcp, ssize_t nread, const void* buf, void* user_data) {
    client_data_t* data = (client_data_t*)user_data;
    
    printf("[DEBUG] read_callback 被调用, nread=%zd\n", nread);
    
    if (nread < 0) {
        /* 读取错误，关闭连接 */
        printf("[客户端 %s:%d] 读取错误，关闭连接\n", data->client_ip, data->client_port);
        vox_tcp_read_stop(tcp);
        vox_handle_close((vox_handle_t*)tcp, NULL);
        vox_tcp_destroy(tcp);
        vox_mpool_t* mpool = vox_loop_get_mpool(g_loop);
        vox_mpool_free(mpool, data);
        g_client_count--;
        return;
    }
    
    if (nread == 0) {
        /* 连接关闭 */
        printf("[客户端 %s:%d] 连接关闭\n", data->client_ip, data->client_port);
        vox_tcp_read_stop(tcp);
        vox_handle_close((vox_handle_t*)tcp, NULL);
        vox_tcp_destroy(tcp);
        vox_mpool_t* mpool = vox_loop_get_mpool(g_loop);
        vox_mpool_free(mpool, data);
        g_client_count--;
        return;
    }
    
    /* Echo: 将收到的数据原样发送回去 */
    printf("[客户端 %s:%d] 收到 %zd 字节: %.*s\n", 
           data->client_ip, data->client_port, nread, (int)nread, (const char*)buf);
    
    if (vox_tcp_write(tcp, buf, (size_t)nread, NULL) != 0) {
        printf("[客户端 %s:%d] 写入失败，关闭连接\n", data->client_ip, data->client_port);
        vox_tcp_read_stop(tcp);
        vox_handle_close((vox_handle_t*)tcp, NULL);
        vox_tcp_destroy(tcp);
        vox_mpool_t* mpool = vox_loop_get_mpool(g_loop);
        vox_mpool_free(mpool, data);
        g_client_count--;
    }
}

/* 连接接受回调 */
static void connection_callback(vox_tcp_t* server, int status, void* user_data) {
    (void)user_data;
    
    printf("[DEBUG] connection_callback 被调用, status=%d\n", status);
    
    if (status != 0) {
        printf("接受连接失败: %d\n", status);
        return;
    }
    
    /* 创建客户端 TCP 句柄 */
    vox_tcp_t* client = vox_tcp_create(g_loop);
    if (!client) {
        printf("创建客户端句柄失败\n");
        return;
    }
    
    /* 初始化客户端句柄 */
    if (vox_tcp_init(client, g_loop) != 0) {
        printf("初始化客户端句柄失败\n");
        vox_tcp_destroy(client);
        return;
    }
    
    /* 接受连接 */
    if (vox_tcp_accept(server, client) != 0) {
        /* 检查是否是 EAGAIN/EWOULDBLOCK（暂时没有连接） */
#ifdef VOX_OS_WINDOWS
        int err = WSAGetLastError();
        if (err == WSAEWOULDBLOCK) {
            /* 暂时没有连接，这是正常的，不需要报告错误 */
            vox_tcp_destroy(client);
            return;
        }
#else
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            /* 暂时没有连接，这是正常的，不需要报告错误 */
            vox_tcp_destroy(client);
            return;
        }
#endif
        printf("接受连接失败\n");
        vox_tcp_destroy(client);
        return;
    }
    
    /* 分配客户端数据（使用内存池） */
    vox_mpool_t* mpool = vox_loop_get_mpool(g_loop);
    client_data_t* data = (client_data_t*)vox_mpool_alloc(mpool, sizeof(client_data_t));
    if (!data) {
        printf("分配客户端数据失败\n");
        vox_tcp_destroy(client);
        return;
    }
    
    memset(data, 0, sizeof(client_data_t));
    data->tcp = client;
    
    /* 设置用户数据 */
    vox_handle_set_data((vox_handle_t*)client, data);
    
    /* 获取客户端地址 */
    vox_socket_addr_t peer_addr;
    if (vox_tcp_getpeername(client, &peer_addr) == 0) {
        char ip[64];
        vox_socket_address_to_string(&peer_addr, ip, sizeof(ip));
        uint16_t port = vox_socket_get_port(&peer_addr);
        /* 保存客户端 IP 信息 */
        strncpy(data->client_ip, ip, sizeof(data->client_ip) - 1);
        data->client_ip[sizeof(data->client_ip) - 1] = '\0';
        data->client_port = port;
        printf("[新连接] %s:%d (总连接数: %d)\n", ip, port, ++g_client_count);
    } else {
        strncpy(data->client_ip, "unknown", sizeof(data->client_ip) - 1);
        data->client_ip[sizeof(data->client_ip) - 1] = '\0';
        data->client_port = 0;
        printf("[新连接] (总连接数: %d)\n", ++g_client_count);
    }
    
    /* 开始读取 */
    if (vox_tcp_read_start(client, alloc_callback, read_callback) != 0) {
        printf("开始读取失败\n");
        vox_mpool_free(mpool, data);
        vox_tcp_destroy(client);
        g_client_count--;
        return;
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

/* TCP Echo 服务器 */
int tcp_echo_server(const char* host, uint16_t port, vox_backend_type_t backend_type) {
    printf("=== TCP Echo 服务器 ===\n");
    printf("监听地址: %s:%d\n", host, port);
    printf("按 Ctrl+C 停止服务器\n\n");
    
    /* 创建事件循环（使用配置接口） */
    printf("正在创建事件循环...\n");
    
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
    printf("事件循环创建成功\n");
    
    /* 打印使用的 backend 名称 */
    vox_backend_t* backend = vox_loop_get_backend(g_loop);
    if (backend) {
        const char* backend_name = vox_backend_name(backend);
        printf("使用的 backend: %s\n", backend_name);
    } else {
        printf("警告: 无法获取 backend 信息\n");
    }
    
    /* 创建服务器 TCP 句柄 */
    printf("正在创建服务器 TCP 句柄...\n");
    vox_tcp_t* server = vox_tcp_create(g_loop);
    if (!server) {
        fprintf(stderr, "创建服务器句柄失败\n");
        vox_loop_destroy(g_loop);
        return 1;
    }
    printf("服务器 TCP 句柄创建成功\n");
    
    /* 初始化服务器句柄 */
    if (vox_tcp_init(server, g_loop) != 0) {
        fprintf(stderr, "初始化服务器句柄失败\n");
        vox_tcp_destroy(server);
        vox_loop_destroy(g_loop);
        return 1;
    }
    printf("服务器 TCP 句柄初始化成功\n");
    
    /* 设置选项 */
    vox_tcp_reuseaddr(server, true);
    vox_tcp_nodelay(server, true);
    printf("TCP 选项设置完成\n");
    
    /* 解析地址 */
    vox_socket_addr_t addr;
    if (vox_socket_parse_address(host, port, &addr) != 0) {
        fprintf(stderr, "解析地址失败: %s:%d\n", host, port);
        vox_tcp_destroy(server);
        vox_loop_destroy(g_loop);
        return 1;
    }
    printf("地址解析成功\n");
    
    /* 绑定地址 */
    if (vox_tcp_bind(server, &addr, 0) != 0) {
        fprintf(stderr, "绑定地址失败\n");
        vox_tcp_destroy(server);
        vox_loop_destroy(g_loop);
        return 1;
    }
    printf("地址绑定成功\n");
    
    /* 开始监听 */
    if (vox_tcp_listen(server, 128, connection_callback) != 0) {
        fprintf(stderr, "监听失败\n");
        vox_tcp_destroy(server);
        vox_loop_destroy(g_loop);
        return 1;
    }
    printf("监听启动成功\n");
    
    printf("服务器已启动，等待连接...\n");
    printf("活跃句柄数: %zu\n", vox_loop_active_handles(g_loop));
    printf("事件循环运行中...\n\n");
    
    /* 注册信号处理 */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    /* 运行事件循环 */
    int ret = vox_loop_run(g_loop, VOX_RUN_DEFAULT);
    
    if (ret != 0) {
        fprintf(stderr, "事件循环运行失败: %d\n", ret);
    }
    
    printf("\n服务器停止，当前连接数: %d\n", g_client_count);
    
    /* 清理 */
    vox_tcp_destroy(server);
    vox_loop_destroy(g_loop);
    
    return ret;
}

/* 连接回调 */
static void client_connect_cb(vox_tcp_t* tcp, int status, void* user_data) {
    client_ctx_t* ctx = (client_ctx_t*)user_data;
    
    if (status != 0) {
        printf("连接失败: %d\n", status);
        vox_mpool_t* mpool = vox_loop_get_mpool(ctx->loop);
        vox_mpool_free(mpool, ctx);
        vox_loop_stop(ctx->loop);
        return;
    }
    
    printf("连接成功\n");
    
    /* 开始读取（连接成功后 socket 已创建） */
    if (vox_tcp_read_start(tcp, client_alloc_cb, client_read_cb) != 0) {
        printf("开始读取失败\n");
        vox_mpool_t* mpool = vox_loop_get_mpool(ctx->loop);
        vox_mpool_free(mpool, ctx);
        vox_loop_stop(ctx->loop);
        return;
    }
    
    /* 发送消息 */
    if (ctx->message && strlen(ctx->message) > 0) {
        printf("发送消息: %s\n", ctx->message);
        if (vox_tcp_write(tcp, ctx->message, strlen(ctx->message), NULL) != 0) {
            printf("发送失败\n");
            vox_mpool_t* mpool = vox_loop_get_mpool(ctx->loop);
            vox_mpool_free(mpool, ctx);
            vox_loop_stop(ctx->loop);
        }
    }
}

/* 客户端缓冲区分配回调 */
static void client_alloc_cb(vox_tcp_t* tcp, size_t suggested_size, 
                           void* buf, size_t* len, void* user_data) {
    client_ctx_t* ctx = (client_ctx_t*)user_data;
    (void)tcp;
    (void)suggested_size;
    *(void**)buf = ctx->buffer;
    *len = sizeof(ctx->buffer);
}

/* 客户端读取回调 */
static void client_read_cb(vox_tcp_t* tcp, ssize_t nread, const void* buf, void* user_data) {
    client_ctx_t* ctx = (client_ctx_t*)user_data;
    
    if (nread < 0) {
        printf("读取错误\n");
        vox_mpool_t* mpool = vox_loop_get_mpool(ctx->loop);
        vox_mpool_free(mpool, ctx);
        vox_loop_stop(ctx->loop);
        return;
    }
    
    if (nread == 0) {
        printf("服务器关闭连接\n");
        vox_mpool_t* mpool = vox_loop_get_mpool(ctx->loop);
        vox_mpool_free(mpool, ctx);
        vox_loop_stop(ctx->loop);
        return;
    }
    
    printf("收到 Echo 响应: %.*s\n", (int)nread, (const char*)buf);
    
    /* 收到响应后关闭连接 */
    vox_tcp_read_stop(tcp);
    vox_handle_close((vox_handle_t*)tcp, NULL);
    vox_tcp_destroy(tcp);
    vox_mpool_t* mpool = vox_loop_get_mpool(ctx->loop);
    vox_mpool_free(mpool, ctx);
    vox_loop_stop(ctx->loop);
}

/* TCP Echo 客户端 */
int tcp_echo_client(const char* host, uint16_t port, const char* message, vox_backend_type_t backend_type) {
    printf("=== TCP Echo 客户端 ===\n");
    printf("连接到: %s:%d\n", host, port);
    
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
    
    /* 创建客户端 TCP 句柄 */
    vox_tcp_t* client = vox_tcp_create(loop);
    if (!client) {
        fprintf(stderr, "创建客户端句柄失败\n");
        vox_loop_destroy(loop);
        return 1;
    }
    
    /* 初始化客户端句柄 */
    if (vox_tcp_init(client, loop) != 0) {
        fprintf(stderr, "初始化客户端句柄失败\n");
        vox_tcp_destroy(client);
        vox_loop_destroy(loop);
        return 1;
    }
    
    /* 设置选项 */
    vox_tcp_nodelay(client, true);
    
    /* 解析服务器地址 */
    vox_socket_addr_t addr;
    if (vox_socket_parse_address(host, port, &addr) != 0) {
        fprintf(stderr, "解析地址失败: %s:%d\n", host, port);
        vox_tcp_destroy(client);
        vox_loop_destroy(loop);
        return 1;
    }
    
    /* 分配客户端数据（使用内存池） */
    vox_mpool_t* mpool = vox_loop_get_mpool(loop);
    client_ctx_t* ctx = (client_ctx_t*)vox_mpool_alloc(mpool, sizeof(client_ctx_t));
    if (!ctx) {
        fprintf(stderr, "分配客户端上下文失败\n");
        vox_tcp_destroy(client);
        vox_loop_destroy(loop);
        return 1;
    }
    memset(ctx, 0, sizeof(client_ctx_t));
    ctx->message = message;
    ctx->loop = loop;
    
    /* 设置用户数据 */
    vox_handle_set_data((vox_handle_t*)client, ctx);
    
    /* 开始连接 */
    if (vox_tcp_connect(client, &addr, client_connect_cb) != 0) {
        fprintf(stderr, "开始连接失败\n");
        vox_mpool_free(mpool, ctx);
        vox_tcp_destroy(client);
        vox_loop_destroy(loop);
        return 1;
    }
    
    /* 注意：读取应该在连接成功后开始，在连接回调中处理 */
    
    /* 运行事件循环 */
    int ret = vox_loop_run(loop, VOX_RUN_DEFAULT);
    
    /* 清理 */
    vox_tcp_destroy(client);
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
        printf("  %s server 0.0.0.0 8888 epoll\n", argv[0]);
        printf("  %s client 127.0.0.1 8888 \"Hello, Echo!\" epoll\n", argv[0]);
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
        return tcp_echo_server(host, port, backend_type);
    } else if (strcmp(argv[1], "client") == 0) {
        if (argc < 4) {
            fprintf(stderr, "客户端需要指定 host 和 port\n");
            return 1;
        }
        const char* host = argv[2];
        uint16_t port = (uint16_t)atoi(argv[3]);
        const char* message = argc > 4 ? argv[4] : "Hello, Echo Server!";
        const char* backend_str = argc > 5 ? argv[5] : "auto";
        vox_backend_type_t backend_type = parse_backend_type(backend_str);
        printf("指定 backend 类型: %s\n", get_backend_type_name(backend_type));
        int ret = tcp_echo_client(host, port, message, backend_type);
        vox_socket_cleanup();
        return ret;
    } else {
        fprintf(stderr, "未知模式: %s\n", argv[1]);
        vox_socket_cleanup();
        return 1;
    }
}
