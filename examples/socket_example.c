/*
 * socket_example.c - Socket操作示例程序
 * 演示 vox_socket 的TCP和UDP操作
 */

#include "../vox_socket.h"
#include "../vox_time.h"
#include <stdio.h>
#include <string.h>

#define TEST_PORT 8888

/* TCP服务器示例 */
void test_tcp_server(void) {
    printf("\n=== TCP服务器示例 ===\n");
    
    vox_socket_t server;
    if (vox_socket_create(&server, VOX_SOCKET_TCP, VOX_AF_INET) != 0) {
        printf("创建socket失败，错误码: %d\n", vox_socket_get_error());
        char err_buf[256];
        vox_socket_error_string(vox_socket_get_error(), err_buf, sizeof(err_buf));
        printf("错误信息: %s\n", err_buf);
        return;
    }
    
    printf("Socket创建成功\n");
    
    /* 设置选项 */
    if (vox_socket_set_reuseaddr(&server, true) != 0) {
        printf("设置SO_REUSEADDR失败\n");
        vox_socket_destroy(&server);
        return;
    }
    
    if (vox_socket_set_keepalive(&server, true) != 0) {
        printf("设置SO_KEEPALIVE失败\n");
    }
    
    if (vox_socket_set_tcp_nodelay(&server, true) != 0) {
        printf("设置TCP_NODELAY失败\n");
    }
    
    /* 绑定地址 */
    vox_socket_addr_t addr;
    if (vox_socket_parse_address("127.0.0.1", TEST_PORT, &addr) != 0) {
        printf("解析地址失败\n");
        vox_socket_destroy(&server);
        return;
    }
    
    if (vox_socket_bind(&server, &addr) != 0) {
        printf("绑定地址失败，错误码: %d\n", vox_socket_get_error());
        char err_buf[256];
        vox_socket_error_string(vox_socket_get_error(), err_buf, sizeof(err_buf));
        printf("错误信息: %s\n", err_buf);
        vox_socket_destroy(&server);
        return;
    }
    
    printf("服务器绑定到 127.0.0.1:%d\n", TEST_PORT);
    
    /* 监听 */
    if (vox_socket_listen(&server, 5) != 0) {
        printf("监听失败，错误码: %d\n", vox_socket_get_error());
        vox_socket_destroy(&server);
        return;
    }
    
    printf("等待客户端连接...\n");
    
    /* 接受连接 */
    vox_socket_addr_t client_addr;
    vox_socket_t client;
    if (vox_socket_accept(&server, &client, &client_addr) != 0) {
        printf("接受连接失败，错误码: %d\n", vox_socket_get_error());
        char err_buf[256];
        vox_socket_error_string(vox_socket_get_error(), err_buf, sizeof(err_buf));
        printf("错误信息: %s\n", err_buf);
        vox_socket_destroy(&server);
        return;
    }
    
    char client_ip[64];
    vox_socket_address_to_string(&client_addr, client_ip, sizeof(client_ip));
    uint16_t client_port = vox_socket_get_port(&client_addr);
    printf("客户端连接: %s:%d\n", client_ip, client_port);
    
    /* 接收数据 */
    char buffer[1024];
    int64_t received = vox_socket_recv(&client, buffer, sizeof(buffer) - 1);
    if (received > 0) {
        buffer[received] = '\0';
        printf("收到数据: %s\n", buffer);
        
        /* 发送响应 */
        const char* response = "Hello from server!";
        int64_t sent = vox_socket_send(&client, response, strlen(response));
        if (sent > 0) {
            printf("发送响应: %lld 字节\n", (long long)sent);
        } else {
            printf("发送响应失败，错误码: %d\n", vox_socket_get_error());
        }
    } else if (received == 0) {
        printf("客户端关闭连接\n");
    } else {
        printf("接收数据失败，错误码: %d\n", vox_socket_get_error());
    }
    
    /* 清理 */
    vox_socket_destroy(&client);
    vox_socket_destroy(&server);
    printf("TCP服务器测试完成\n");
}

/* TCP客户端示例 */
void test_tcp_client(void) {
    printf("\n=== TCP客户端示例 ===\n");
    
    vox_socket_t client;
    if (vox_socket_create(&client, VOX_SOCKET_TCP, VOX_AF_INET) != 0) {
        printf("创建socket失败，错误码: %d\n", vox_socket_get_error());
        char err_buf[256];
        vox_socket_error_string(vox_socket_get_error(), err_buf, sizeof(err_buf));
        printf("错误信息: %s\n", err_buf);
        return;
    }
    
    printf("Socket创建成功\n");
    
    /* 设置选项 */
    if (vox_socket_set_tcp_nodelay(&client, true) != 0) {
        printf("设置TCP_NODELAY失败\n");
    }
    
    /* 连接到服务器 */
    vox_socket_addr_t addr;
    if (vox_socket_parse_address("127.0.0.1", TEST_PORT, &addr) != 0) {
        printf("解析地址失败\n");
        vox_socket_destroy(&client);
        return;
    }
    
    printf("连接到 127.0.0.1:%d...\n", TEST_PORT);
    if (vox_socket_connect(&client, &addr) != 0) {
        int err = vox_socket_get_error();
        printf("连接失败，错误码: %d\n", err);
        char err_buf[256];
        vox_socket_error_string(err, err_buf, sizeof(err_buf));
        printf("错误信息: %s\n", err_buf);
        vox_socket_destroy(&client);
        return;
    }
    
    printf("连接成功\n");
    
    /* 获取本地和对端地址 */
    vox_socket_addr_t local_addr, peer_addr;
    if (vox_socket_get_local_addr(&client, &local_addr) == 0) {
        char local_ip[64];
        vox_socket_address_to_string(&local_addr, local_ip, sizeof(local_ip));
        printf("本地地址: %s:%d\n", local_ip, vox_socket_get_port(&local_addr));
    }
    
    if (vox_socket_get_peer_addr(&client, &peer_addr) == 0) {
        char peer_ip[64];
        vox_socket_address_to_string(&peer_addr, peer_ip, sizeof(peer_ip));
        printf("对端地址: %s:%d\n", peer_ip, vox_socket_get_port(&peer_addr));
    }
    
    /* 发送数据 */
    const char* message = "Hello from client!";
    int64_t sent = vox_socket_send(&client, message, strlen(message));
    if (sent > 0) {
        printf("发送数据: %lld 字节\n", (long long)sent);
    } else {
        printf("发送数据失败，错误码: %d\n", vox_socket_get_error());
    }
    
    /* 接收响应 */
    char buffer[1024];
    int64_t received = vox_socket_recv(&client, buffer, sizeof(buffer) - 1);
    if (received > 0) {
        buffer[received] = '\0';
        printf("收到响应: %s\n", buffer);
    } else if (received == 0) {
        printf("服务器关闭连接\n");
    } else {
        printf("接收数据失败，错误码: %d\n", vox_socket_get_error());
    }
    
    /* 清理 */
    vox_socket_destroy(&client);
    printf("TCP客户端测试完成\n");
}

/* UDP服务器示例 */
void test_udp_server(void) {
    printf("\n=== UDP服务器示例 ===\n");
    
    vox_socket_t server;
    if (vox_socket_create(&server, VOX_SOCKET_UDP, VOX_AF_INET) != 0) {
        printf("创建socket失败，错误码: %d\n", vox_socket_get_error());
        return;
    }
    
    printf("Socket创建成功\n");
    
    /* 设置选项 */
    if (vox_socket_set_reuseaddr(&server, true) != 0) {
        printf("设置SO_REUSEADDR失败\n");
    }
    
    if (vox_socket_set_broadcast(&server, false) != 0) {
        printf("设置SO_BROADCAST失败\n");
    }
    
    /* 绑定地址 */
    vox_socket_addr_t addr;
    if (vox_socket_parse_address("127.0.0.1", TEST_PORT + 1, &addr) != 0) {
        printf("解析地址失败\n");
        vox_socket_destroy(&server);
        return;
    }
    
    if (vox_socket_bind(&server, &addr) != 0) {
        printf("绑定地址失败，错误码: %d\n", vox_socket_get_error());
        vox_socket_destroy(&server);
        return;
    }
    
    printf("UDP服务器绑定到 127.0.0.1:%d\n", TEST_PORT + 1);
    printf("等待接收数据...\n");
    
    /* 接收数据 */
    char buffer[1024];
    vox_socket_addr_t client_addr;
    int64_t received = vox_socket_recvfrom(&server, buffer, sizeof(buffer) - 1, &client_addr);
    if (received > 0) {
        buffer[received] = '\0';
        char client_ip[64];
        vox_socket_address_to_string(&client_addr, client_ip, sizeof(client_ip));
        printf("从 %s:%d 收到数据: %s\n", 
               client_ip, vox_socket_get_port(&client_addr), buffer);
        
        /* 发送响应 */
        const char* response = "UDP response from server!";
        int64_t sent = vox_socket_sendto(&server, response, strlen(response), &client_addr);
        if (sent > 0) {
            printf("发送响应: %lld 字节\n", (long long)sent);
        } else {
            printf("发送响应失败，错误码: %d\n", vox_socket_get_error());
        }
    } else {
        printf("接收数据失败，错误码: %d\n", vox_socket_get_error());
    }
    
    /* 清理 */
    vox_socket_destroy(&server);
    printf("UDP服务器测试完成\n");
}

/* UDP客户端示例 */
void test_udp_client(void) {
    printf("\n=== UDP客户端示例 ===\n");
    
    vox_socket_t client;
    if (vox_socket_create(&client, VOX_SOCKET_UDP, VOX_AF_INET) != 0) {
        printf("创建socket失败，错误码: %d\n", vox_socket_get_error());
        return;
    }
    
    printf("Socket创建成功\n");
    
    /* 准备服务器地址 */
    vox_socket_addr_t addr;
    if (vox_socket_parse_address("127.0.0.1", TEST_PORT + 1, &addr) != 0) {
        printf("解析地址失败\n");
        vox_socket_destroy(&client);
        return;
    }
    
    printf("发送数据到 127.0.0.1:%d...\n", TEST_PORT + 1);
    
    /* 发送数据 */
    const char* message = "Hello UDP from client!";
    int64_t sent = vox_socket_sendto(&client, message, strlen(message), &addr);
    if (sent > 0) {
        printf("发送数据: %lld 字节\n", (long long)sent);
    } else {
        printf("发送数据失败，错误码: %d\n", vox_socket_get_error());
        vox_socket_destroy(&client);
        return;
    }
    
    /* 接收响应 */
    char buffer[1024];
    vox_socket_addr_t server_addr;
    int64_t received = vox_socket_recvfrom(&client, buffer, sizeof(buffer) - 1, &server_addr);
    if (received > 0) {
        buffer[received] = '\0';
        char server_ip[64];
        vox_socket_address_to_string(&server_addr, server_ip, sizeof(server_ip));
        printf("从 %s:%d 收到响应: %s\n", 
               server_ip, vox_socket_get_port(&server_addr), buffer);
    } else {
        printf("接收数据失败，错误码: %d\n", vox_socket_get_error());
    }
    
    /* 清理 */
    vox_socket_destroy(&client);
    printf("UDP客户端测试完成\n");
}

/* 测试地址解析 */
void test_address_parsing(void) {
    printf("\n=== 地址解析测试 ===\n");
    
    const char* test_addresses[] = {
        "127.0.0.1",
        "192.168.1.1",
        "0.0.0.0",
        "255.255.255.255",
        "::1",
        "::",
        "2001:db8::1",
        "fe80::1"
    };
    
    int success_count = 0;
    int fail_count = 0;
    
    for (size_t i = 0; i < sizeof(test_addresses) / sizeof(test_addresses[0]); i++) {
        vox_socket_addr_t addr;
        if (vox_socket_parse_address(test_addresses[i], 8080, &addr) == 0) {
            char buf[64];
            vox_socket_address_to_string(&addr, buf, sizeof(buf));
            printf("  [✓] 解析 '%s' -> %s:%d\n", 
                   test_addresses[i], buf, vox_socket_get_port(&addr));
            success_count++;
        } else {
            printf("  [✗] 解析 '%s' 失败\n", test_addresses[i]);
            fail_count++;
        }
    }
    
    printf("地址解析测试完成: 成功 %d, 失败 %d\n", success_count, fail_count);
}

/* 测试Socket选项 */
void test_socket_options(void) {
    printf("\n=== Socket选项测试 ===\n");
    
    vox_socket_t sock;
    if (vox_socket_create(&sock, VOX_SOCKET_TCP, VOX_AF_INET) != 0) {
        printf("创建socket失败\n");
        return;
    }
    
    int test_count = 0;
    int success_count = 0;
    
    /* 测试各种选项 */
    test_count++;
    if (vox_socket_set_reuseaddr(&sock, true) == 0) {
        printf("  [✓] 设置SO_REUSEADDR成功\n");
        success_count++;
    } else {
        printf("  [✗] 设置SO_REUSEADDR失败\n");
    }
    
    test_count++;
    if (vox_socket_set_keepalive(&sock, true) == 0) {
        printf("  [✓] 设置SO_KEEPALIVE成功\n");
        success_count++;
    } else {
        printf("  [✗] 设置SO_KEEPALIVE失败\n");
    }
    
    test_count++;
    if (vox_socket_set_tcp_nodelay(&sock, true) == 0) {
        printf("  [✓] 设置TCP_NODELAY成功\n");
        success_count++;
    } else {
        printf("  [✗] 设置TCP_NODELAY失败\n");
    }
    
    test_count++;
    if (vox_socket_set_recv_buffer_size(&sock, 8192) == 0) {
        printf("  [✓] 设置接收缓冲区大小成功\n");
        success_count++;
    } else {
        printf("  [✗] 设置接收缓冲区大小失败\n");
    }
    
    test_count++;
    if (vox_socket_set_send_buffer_size(&sock, 8192) == 0) {
        printf("  [✓] 设置发送缓冲区大小成功\n");
        success_count++;
    } else {
        printf("  [✗] 设置发送缓冲区大小失败\n");
    }
    
    test_count++;
    if (vox_socket_set_recv_timeout(&sock, 5000) == 0) {
        printf("  [✓] 设置接收超时成功\n");
        success_count++;
    } else {
        printf("  [✗] 设置接收超时失败\n");
    }
    
    test_count++;
    if (vox_socket_set_send_timeout(&sock, 5000) == 0) {
        printf("  [✓] 设置发送超时成功\n");
        success_count++;
    } else {
        printf("  [✗] 设置发送超时失败\n");
    }
    
    test_count++;
    if (vox_socket_set_linger(&sock, true, 5) == 0) {
        printf("  [✓] 设置SO_LINGER成功\n");
        success_count++;
    } else {
        printf("  [✗] 设置SO_LINGER失败\n");
    }
    
    test_count++;
    if (vox_socket_set_nonblock(&sock, false) == 0) {
        printf("  [✓] 设置阻塞模式成功\n");
        success_count++;
    } else {
        printf("  [✗] 设置阻塞模式失败\n");
    }
    
    vox_socket_destroy(&sock);
    printf("Socket选项测试完成: 成功 %d/%d\n", success_count, test_count);
}

/* 测试错误处理 */
void test_error_handling(void) {
    printf("\n=== 错误处理测试 ===\n");
    
    /* 测试NULL指针 */
    printf("测试NULL指针处理...\n");
    if (vox_socket_create(NULL, VOX_SOCKET_TCP, VOX_AF_INET) == -1) {
        printf("  [✓] NULL指针检查正常\n");
    } else {
        printf("  [✗] NULL指针检查失败\n");
    }
    
    /* 测试无效操作 */
    vox_socket_t sock;
    if (vox_socket_create(&sock, VOX_SOCKET_TCP, VOX_AF_INET) == 0) {
        printf("  [✓] Socket创建成功\n");
        
        /* 尝试在未绑定的socket上监听 */
        if (vox_socket_listen(&sock, 5) == -1) {
            printf("  [✓] 未绑定socket监听失败（预期行为）\n");
        } else {
            printf("  [✗] 未绑定socket监听应该失败\n");
        }
        
        /* 尝试在未连接的socket上发送数据 */
        int64_t sent = vox_socket_send(&sock, "test", 4);
        if (sent == -1) {
            printf("  [✓] 未连接socket发送失败（预期行为）\n");
        } else {
            printf("  [✗] 未连接socket发送应该失败\n");
        }
        
        vox_socket_destroy(&sock);
    }
    
    printf("错误处理测试完成\n");
}

/* 测试IPv6支持 */
void test_ipv6_support(void) {
    printf("\n=== IPv6支持测试 ===\n");
    
    vox_socket_t sock;
    if (vox_socket_create(&sock, VOX_SOCKET_TCP, VOX_AF_INET6) != 0) {
        printf("  [✗] 创建IPv6 socket失败，错误码: %d\n", vox_socket_get_error());
        return;
    }
    
    printf("  [✓] IPv6 socket创建成功\n");
    
    /* 测试IPv6地址解析 */
    vox_socket_addr_t addr;
    if (vox_socket_parse_address("::1", 8080, &addr) == 0) {
        char buf[64];
        vox_socket_address_to_string(&addr, buf, sizeof(buf));
        printf("  [✓] IPv6地址解析成功: %s\n", buf);
    } else {
        printf("  [✗] IPv6地址解析失败\n");
    }
    
    vox_socket_destroy(&sock);
    printf("IPv6支持测试完成\n");
}

int main(void) {
    printf("========================================\n");
    printf("    vox_socket 示例程序\n");
    printf("========================================\n");
    
    /* 初始化socket库 */
    if (vox_socket_init() != 0) {
        printf("初始化socket库失败\n");
        return 1;
    }
    printf("Socket库初始化成功\n");
    
    /* 基础功能测试 */
    test_address_parsing();
    test_socket_options();
    test_error_handling();
    test_ipv6_support();
    
    /* 网络通信测试 */
    printf("\n========================================\n");
    printf("    网络通信测试\n");
    printf("========================================\n");
    printf("\n注意：以下测试需要分别运行服务器和客户端\n");
    printf("在实际使用中，服务器和客户端应该在不同的进程或线程中运行\n");
    printf("\n可用的测试函数：\n");
    printf("  - test_tcp_server()  : TCP服务器\n");
    printf("  - test_tcp_client()   : TCP客户端\n");
    printf("  - test_udp_server()  : UDP服务器\n");
    printf("  - test_udp_client()  : UDP客户端\n");
    printf("\n要运行网络测试，请取消注释相应的函数调用\n");
    
    /* 取消注释以下行来运行网络测试 */
    /*
    printf("\n运行TCP服务器测试...\n");
    test_tcp_server();
    
    printf("\n运行TCP客户端测试...\n");
    test_tcp_client();
    
    printf("\n运行UDP服务器测试...\n");
    test_udp_server();
    
    printf("\n运行UDP客户端测试...\n");
    test_udp_client();
    */
   
    test_tcp_server();
    /* 清理 */
    vox_socket_cleanup();
    
    printf("\n========================================\n");
    printf("    所有测试完成\n");
    printf("========================================\n");
    return 0;
}
