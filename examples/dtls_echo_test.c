/*
 * dtls_echo_test.c - DTLS Echo 服务器和客户端测试
 * 演示使用异步 IO 框架实现 DTLS Echo 服务器
 */

#include "vox_loop.h"
#include "vox_dtls.h"
#include "vox_socket.h"
#include "vox_log.h"
#include "vox_mpool.h"
#include "vox_os.h"  /* 包含平台特定头文件 */
#include "vox_backend.h"
#include "ssl/vox_ssl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

#define ECHO_PORT 8890
#define BUFFER_SIZE 4096

/* 全局变量 */
static vox_loop_t* g_loop = NULL;
static int g_client_count = 0;

/* 服务器端客户端连接信息 */
typedef struct {
    vox_dtls_t* dtls;
    char buffer[BUFFER_SIZE];
    char client_ip[64];  /* 客户端 IP 地址字符串 */
    uint16_t client_port;  /* 客户端端口 */
    vox_socket_addr_t client_addr;  /* 客户端地址 */
} client_data_t;

/* 客户端数据 */
typedef struct {
    char buffer[BUFFER_SIZE];
    const char* message;
    vox_loop_t* loop;
    bool message_sent;  /* 标记消息是否已发送 */
} client_ctx_t;

/* 前向声明 */
static void client_connect_cb(vox_dtls_t* dtls, int status, void* user_data);
static void client_alloc_cb(vox_dtls_t* dtls, size_t suggested_size, 
                           void* buf, size_t* len, void* user_data);
static void client_read_cb(vox_dtls_t* dtls, ssize_t nread, const void* buf, 
                           const vox_socket_addr_t* addr, void* user_data);

/* 缓冲区分配回调 */
static void alloc_callback(vox_dtls_t* dtls, size_t suggested_size, 
                          void* buf, size_t* len, void* user_data) {
    client_data_t* data = (client_data_t*)user_data;
    (void)dtls;
    (void)suggested_size;
    
    *(void**)buf = data->buffer;
    *len = sizeof(data->buffer);
}

/* 读取回调 - Echo 服务器核心逻辑 */
static void read_callback(vox_dtls_t *dtls, ssize_t nread, const void *buf,
                          const vox_socket_addr_t *addr, void *user_data) {
    (void)addr;                        
    client_data_t* data = (client_data_t*)user_data;
    
    if (nread < 0) {
        /* 读取错误，关闭连接 */
        printf("[客户端 %s:%d] 读取错误，关闭连接\n", data->client_ip, data->client_port);
        vox_dtls_read_stop(dtls);
        vox_handle_close((vox_handle_t*)dtls, NULL);
        vox_dtls_destroy(dtls);
        vox_mpool_t* mpool = vox_loop_get_mpool(g_loop);
        vox_mpool_free(mpool, data);
        g_client_count--;
        return;
    }
    
    if (nread == 0) {
        /* 连接关闭 */
        printf("[客户端 %s:%d] 连接关闭\n", data->client_ip, data->client_port);
        vox_dtls_read_stop(dtls);
        vox_handle_close((vox_handle_t*)dtls, NULL);
        vox_dtls_destroy(dtls);
        vox_mpool_t* mpool = vox_loop_get_mpool(g_loop);
        vox_mpool_free(mpool, data);
        g_client_count--;
        return;
    }
    
    /* Echo: 将收到的数据原样发送回去 */
    /* 确保字符串以 null 结尾（虽然 nread 可能小于缓冲区大小） */
    if (nread > 0 && buf) {
        printf("[客户端 %s:%d] 收到 %zd 字节: ", data->client_ip, data->client_port, nread);
        fwrite(buf, 1, (size_t)nread, stdout);
        printf("\n");
    } else {
        printf("[客户端 %s:%d] 收到 %zd 字节\n", data->client_ip, data->client_port, nread);
    }
    
    /* 使用保存的客户端地址发送回数据 */
    if (vox_dtls_write(dtls, buf, (size_t)nread, &data->client_addr, NULL) != 0) {
        printf("[客户端 %s:%d] 写入失败，关闭连接\n", data->client_ip, data->client_port);
        vox_dtls_read_stop(dtls);
        vox_handle_close((vox_handle_t*)dtls, NULL);
        vox_dtls_destroy(dtls);
        vox_mpool_t* mpool = vox_loop_get_mpool(g_loop);
        vox_mpool_free(mpool, data);
        g_client_count--;
    }
}

/* 握手回调 */
static void handshake_callback(vox_dtls_t* dtls, int status, void* user_data) {
    client_data_t* data = (client_data_t*)user_data;
    
    if (status != 0) {
        printf("[客户端 %s:%d] DTLS 握手失败\n", data->client_ip, data->client_port);
        vox_handle_close((vox_handle_t*)dtls, NULL);
        vox_dtls_destroy(dtls);
        vox_mpool_t* mpool = vox_loop_get_mpool(g_loop);
        vox_mpool_free(mpool, data);
        g_client_count--;
        return;
    }
    
    printf("[客户端 %s:%d] DTLS 握手成功\n", data->client_ip, data->client_port);
    
    /* 开始读取 */
    if (vox_dtls_read_start(dtls, alloc_callback, read_callback) != 0) {
        printf("[客户端 %s:%d] 开始读取失败\n", data->client_ip, data->client_port);
        vox_handle_close((vox_handle_t*)dtls, NULL);
        vox_dtls_destroy(dtls);
        vox_mpool_t* mpool = vox_loop_get_mpool(g_loop);
        vox_mpool_free(mpool, data);
        g_client_count--;
    }
}

/* 连接接受回调 */
static void connection_callback(vox_dtls_t* server, int status, void* user_data) {
    (void)user_data;
    
    if (status != 0) {
        printf("接受连接失败: %d\n", status);
        return;
    }
    
    /* 注意：对于 UDP/DTLS，connection_cb 在收到第一个数据包时被调用
     * 但当前实现中，服务器 DTLS 句柄会直接处理所有接收到的数据
     * 这意味着服务器只能处理一个客户端连接
     * 
     * 在实际应用中，如果需要支持多客户端，应该：
     * 1. 维护一个客户端地址到 DTLS 句柄的映射表
     * 2. 当收到新客户端的数据包时，创建新的客户端 DTLS 句柄
     * 3. 将数据包路由到对应的客户端 DTLS 句柄
     * 
     * 为了简化示例，我们假设服务器只处理一个客户端连接
     */
    printf("收到新的 DTLS 连接请求\n");
    
    /* 获取客户端地址信息 */
    vox_socket_addr_t peer_addr;
    char client_ip[64] = "unknown";
    uint16_t client_port = 0;
    if (vox_dtls_getpeername(server, &peer_addr) == 0) {
        vox_socket_address_to_string(&peer_addr, client_ip, sizeof(client_ip));
        client_port = vox_socket_get_port(&peer_addr);
        printf("[新连接] %s:%d (总连接数: %d)\n", client_ip, client_port, ++g_client_count);
    } else {
        printf("[新连接] (总连接数: %d)\n", ++g_client_count);
    }
    
    /* 分配客户端数据（使用内存池） */
    vox_mpool_t* mpool = vox_loop_get_mpool(g_loop);
    client_data_t* data = (client_data_t*)vox_mpool_alloc(mpool, sizeof(client_data_t));
    if (!data) {
        printf("分配客户端数据失败\n");
        return;
    }
    
    memset(data, 0, sizeof(client_data_t));
    data->dtls = server;
    strncpy(data->client_ip, client_ip, sizeof(data->client_ip) - 1);
    data->client_ip[sizeof(data->client_ip) - 1] = '\0';
    data->client_port = client_port;
    if (vox_dtls_getpeername(server, &data->client_addr) == 0) {
        /* 地址已保存 */
    }
    
    /* 设置用户数据 */
    vox_handle_set_data((vox_handle_t*)server, data);
    
    /* 开始 DTLS 握手 */
    if (vox_dtls_handshake(server, handshake_callback) != 0) {
        printf("开始握手失败\n");
        vox_mpool_free(mpool, data);
        g_client_count--;
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

/* DTLS Echo 服务器 */
int dtls_echo_server(const char* host, uint16_t port, const char* cert_file, const char* key_file, vox_backend_type_t backend_type) {
    printf("=== DTLS Echo 服务器 ===\n");
    printf("监听地址: %s:%d\n", host, port);
    printf("证书文件: %s\n", cert_file);
    printf("私钥文件: %s\n", key_file);
    printf("按 Ctrl+C 停止服务器\n\n");
    
    /* 创建事件循环 */
    printf("正在创建事件循环...\n");
    
    vox_backend_config_t backend_config = {0};
    backend_config.type = backend_type;
    backend_config.mpool = NULL;
    backend_config.max_events = 0;
    
    vox_loop_config_t loop_config = {0};
    loop_config.backend_config = &backend_config;
    
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
    }
    
    /* 创建 SSL Context（服务器模式） */
    vox_mpool_t* mpool = vox_loop_get_mpool(g_loop);
    vox_ssl_context_t* ssl_ctx = vox_ssl_context_create(mpool, VOX_SSL_MODE_SERVER);
    if (!ssl_ctx) {
        fprintf(stderr, "创建 SSL Context 失败\n");
        vox_loop_destroy(g_loop);
        return 1;
    }
    
    /* 配置 SSL Context */
    vox_ssl_config_t ssl_config = {0};
    ssl_config.cert_file = cert_file;
    ssl_config.key_file = key_file;
    ssl_config.protocols = "DTLS";  /* 指定使用 DTLS */
    if (vox_ssl_context_configure(ssl_ctx, &ssl_config) != 0) {
        fprintf(stderr, "配置 SSL Context 失败\n");
        vox_ssl_context_destroy(ssl_ctx);
        vox_loop_destroy(g_loop);
        return 1;
    }
    printf("SSL Context 配置成功\n");
    
    /* 创建服务器 DTLS 句柄 */
    printf("正在创建服务器 DTLS 句柄...\n");
    vox_dtls_t* server = vox_dtls_create(g_loop, ssl_ctx);
    if (!server) {
        fprintf(stderr, "创建服务器句柄失败\n");
        vox_ssl_context_destroy(ssl_ctx);
        vox_loop_destroy(g_loop);
        return 1;
    }
    printf("服务器 DTLS 句柄创建成功\n");
    
    /* 设置选项 */
    vox_dtls_set_reuseaddr(server, true);
    printf("DTLS 选项设置完成\n");
    
    /* 解析地址 */
    vox_socket_addr_t addr;
    if (vox_socket_parse_address(host, port, &addr) != 0) {
        fprintf(stderr, "解析地址失败: %s:%d\n", host, port);
        vox_dtls_destroy(server);
        vox_ssl_context_destroy(ssl_ctx);
        vox_loop_destroy(g_loop);
        return 1;
    }
    printf("地址解析成功\n");
    
    /* 绑定地址 */
    if (vox_dtls_bind(server, &addr, 0) != 0) {
        fprintf(stderr, "绑定地址失败\n");
        vox_dtls_destroy(server);
        vox_ssl_context_destroy(ssl_ctx);
        vox_loop_destroy(g_loop);
        return 1;
    }
    printf("地址绑定成功\n");
    
    /* 开始监听 */
    if (vox_dtls_listen(server, 128, connection_callback) != 0) {
        fprintf(stderr, "监听失败\n");
        vox_dtls_destroy(server);
        vox_ssl_context_destroy(ssl_ctx);
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
    vox_dtls_destroy(server);
    vox_ssl_context_destroy(ssl_ctx);
    vox_loop_destroy(g_loop);
    
    return ret;
}

/* 连接回调 */
static void client_connect_cb(vox_dtls_t* dtls, int status, void* user_data) {
    client_ctx_t* ctx = (client_ctx_t*)user_data;
    
    if (status != 0) {
        printf("DTLS 连接失败: %d\n", status);
        vox_mpool_t* mpool = vox_loop_get_mpool(ctx->loop);
        vox_mpool_free(mpool, ctx);
        vox_loop_stop(ctx->loop);
        return;
    }
    
    printf("DTLS 连接成功\n");
    
    /* 开始读取 */
    if (vox_dtls_read_start(dtls, client_alloc_cb, client_read_cb) != 0) {
        printf("开始读取失败\n");
        vox_mpool_t* mpool = vox_loop_get_mpool(ctx->loop);
        vox_mpool_free(mpool, ctx);
        vox_loop_stop(ctx->loop);
        return;
    }
    
    /* 发送消息 */
    if (ctx->message && strlen(ctx->message) > 0) {
        printf("发送消息: %s\n", ctx->message);
        /* 获取对端地址 */
        vox_socket_addr_t peer_addr;
        if (vox_dtls_getpeername(dtls, &peer_addr) == 0) {
            if (vox_dtls_write(dtls, ctx->message, strlen(ctx->message), &peer_addr, NULL) != 0) {
                printf("发送失败\n");
                vox_mpool_t* mpool = vox_loop_get_mpool(ctx->loop);
                vox_mpool_free(mpool, ctx);
                vox_loop_stop(ctx->loop);
            } else {
                /* 标记消息已发送 */
                ctx->message_sent = true;
            }
        } else {
            printf("获取对端地址失败\n");
            vox_mpool_t* mpool = vox_loop_get_mpool(ctx->loop);
            vox_mpool_free(mpool, ctx);
            vox_loop_stop(ctx->loop);
        }
    }
}

/* 客户端缓冲区分配回调 */
static void client_alloc_cb(vox_dtls_t* dtls, size_t suggested_size, 
                           void* buf, size_t* len, void* user_data) {
    client_ctx_t* ctx = (client_ctx_t*)user_data;
    (void)dtls;
    (void)suggested_size;
    *(void**)buf = ctx->buffer;
    *len = sizeof(ctx->buffer);
}

/* 客户端读取回调 */
static void client_read_cb(vox_dtls_t* dtls, ssize_t nread, const void* buf, 
                           const vox_socket_addr_t* addr, void* user_data) {
    client_ctx_t* ctx = (client_ctx_t*)user_data;
    (void)addr;

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

    /* 如果消息还未发送，说明这是 DTLS post-handshake 消息，忽略它 */
    if (!ctx->message_sent) {
        printf("收到 DTLS post-handshake 消息 (%zd 字节)，忽略\n", nread);
        return;
    }

    /* 消息已发送，这是真正的 Echo 响应 */
    printf("收到 Echo 响应 (%zd 字节): ", nread);
    if (buf && nread > 0) {
        /* 打印十六进制和字符串 */
        printf("hex=[");
        for (ssize_t i = 0; i < nread; i++) {
            printf("%02x ", ((unsigned char*)buf)[i]);
        }
        printf("] str=[%.*s]\n", (int)nread, (const char*)buf);
    } else {
        printf("(buf=%p)\n", buf);
    }

    /* 收到响应后关闭连接 */
    vox_dtls_read_stop(dtls);
    vox_handle_close((vox_handle_t*)dtls, NULL);
    vox_dtls_destroy(dtls);
    vox_mpool_t* mpool = vox_loop_get_mpool(ctx->loop);
    vox_mpool_free(mpool, ctx);
    vox_loop_stop(ctx->loop);
}

/* DTLS Echo 客户端 */
int dtls_echo_client(const char* host, uint16_t port, const char* message, const char* ca_file, bool verify_peer, vox_backend_type_t backend_type) {
    printf("=== DTLS Echo 客户端 ===\n");
    printf("连接到: %s:%d\n", host, port);
    
    /* 创建事件循环 */
    vox_backend_config_t backend_config = {0};
    backend_config.type = backend_type;
    backend_config.mpool = NULL;
    backend_config.max_events = 0;
    
    vox_loop_config_t loop_config = {0};
    loop_config.backend_config = &backend_config;
    
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
    }
    
    /* 创建 SSL Context（客户端模式） */
    vox_mpool_t* mpool = vox_loop_get_mpool(loop);
    vox_ssl_context_t* ssl_ctx = vox_ssl_context_create(mpool, VOX_SSL_MODE_CLIENT);
    if (!ssl_ctx) {
        fprintf(stderr, "创建 SSL Context 失败\n");
        vox_loop_destroy(loop);
        return 1;
    }
    
    /* 配置 SSL Context */
    vox_ssl_config_t ssl_config = {0};
    ssl_config.ca_file = ca_file;
    ssl_config.verify_peer = verify_peer;
    ssl_config.protocols = "DTLS";  /* 指定使用 DTLS */
    if (vox_ssl_context_configure(ssl_ctx, &ssl_config) != 0) {
        fprintf(stderr, "配置 SSL Context 失败\n");
        vox_ssl_context_destroy(ssl_ctx);
        vox_loop_destroy(loop);
        return 1;
    }
    
    /* 创建客户端 DTLS 句柄 */
    vox_dtls_t* client = vox_dtls_create(loop, ssl_ctx);
    if (!client) {
        fprintf(stderr, "创建客户端句柄失败\n");
        vox_ssl_context_destroy(ssl_ctx);
        vox_loop_destroy(loop);
        return 1;
    }
    
    /* 解析服务器地址 */
    vox_socket_addr_t addr;
    if (vox_socket_parse_address(host, port, &addr) != 0) {
        fprintf(stderr, "解析地址失败: %s:%d\n", host, port);
        vox_dtls_destroy(client);
        vox_ssl_context_destroy(ssl_ctx);
        vox_loop_destroy(loop);
        return 1;
    }
    
    /* 分配客户端上下文 */
    client_ctx_t* ctx = (client_ctx_t*)vox_mpool_alloc(mpool, sizeof(client_ctx_t));
    if (!ctx) {
        fprintf(stderr, "分配客户端上下文失败\n");
        vox_dtls_destroy(client);
        vox_ssl_context_destroy(ssl_ctx);
        vox_loop_destroy(loop);
        return 1;
    }
    
    memset(ctx, 0, sizeof(client_ctx_t));
    ctx->message = message;
    ctx->loop = loop;
    ctx->message_sent = false;
    
    /* 设置用户数据 */
    vox_handle_set_data((vox_handle_t*)client, ctx);
    
    /* 连接（vox_dtls_connect 会自动绑定本地地址） */
    printf("正在连接...\n");
    if (vox_dtls_connect(client, &addr, client_connect_cb) != 0) {
        fprintf(stderr, "连接失败（可能是握手启动失败或 UDP socket 未准备好）\n");
        vox_mpool_free(mpool, ctx);
        vox_dtls_destroy(client);
        vox_ssl_context_destroy(ssl_ctx);
        vox_loop_destroy(loop);
        return 1;
    }
    printf("连接请求已发送，等待握手...\n");
    
    /* 运行事件循环 */
    int ret = vox_loop_run(loop, VOX_RUN_DEFAULT);
    
    if (ret != 0) {
        fprintf(stderr, "事件循环运行失败: %d\n", ret);
    }
    
    /* 清理 */
    vox_ssl_context_destroy(ssl_ctx);
    vox_loop_destroy(loop);
    
    return ret;
}

/* 主函数 */
int main(int argc, char* argv[]) {
    if (argc < 2) {
        printf("用法:\n");
        printf("  服务器模式: %s server <host> <port> <cert_file> <key_file> [backend_type]\n", argv[0]);
        printf("  客户端模式: %s client <host> <port> <message> [ca_file] [verify_peer] [backend_type]\n", argv[0]);
        printf("\n");
        printf("示例:\n");
        printf("  服务器: %s server 0.0.0.0 %d cert/server.crt cert/server.key\n", argv[0], ECHO_PORT);
        printf("  客户端: %s client 127.0.0.1 %d \"Hello DTLS\" cert/ca.crt false epoll\n", argv[0], ECHO_PORT);
        printf("\n");
        printf("backend_type: select, epoll, kqueue, iocp (默认: auto)\n");
        return 1;
    }
    
    const char* mode = argv[1];
    vox_backend_type_t backend_type = VOX_BACKEND_TYPE_AUTO;
    
    /* 解析 backend 类型 */
    if (argc > 2 && strcmp(argv[argc - 1], "select") == 0) {
        backend_type = VOX_BACKEND_TYPE_SELECT;
        argc--;
    } else if (argc > 2 && strcmp(argv[argc - 1], "epoll") == 0) {
        backend_type = VOX_BACKEND_TYPE_EPOLL;
        argc--;
    } else if (argc > 2 && strcmp(argv[argc - 1], "kqueue") == 0) {
        backend_type = VOX_BACKEND_TYPE_KQUEUE;
        argc--;
    } else if (argc > 2 && strcmp(argv[argc - 1], "iocp") == 0) {
        backend_type = VOX_BACKEND_TYPE_IOCP;
        argc--;
    }
    
    if (strcmp(mode, "server") == 0) {
        if (argc < 5) {
            fprintf(stderr, "服务器模式需要参数: <host> <port> <cert_file> <key_file>\n");
            return 1;
        }
        
        const char* host = argv[2];
        uint16_t port = (uint16_t)atoi(argv[3]);
        const char* cert_file = argv[4];
        const char* key_file = argv[5];
        
        return dtls_echo_server(host, port, cert_file, key_file, backend_type);
    } else if (strcmp(mode, "client") == 0) {
        if (argc < 5) {
            fprintf(stderr, "客户端模式需要参数: <host> <port> <message> [ca_file] [verify_peer]\n");
            return 1;
        }
        
        const char* host = argv[2];
        uint16_t port = (uint16_t)atoi(argv[3]);
        const char* message = argv[4];
        const char* ca_file = (argc > 5) ? argv[5] : NULL;
        bool verify_peer = (argc > 6) ? (strcmp(argv[6], "true") == 0) : false;
        
        return dtls_echo_client(host, port, message, ca_file, verify_peer, backend_type);
    } else {
        fprintf(stderr, "未知模式: %s\n", mode);
        return 1;
    }
}
