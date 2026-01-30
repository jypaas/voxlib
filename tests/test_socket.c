/* ============================================================
 * test_socket.c - vox_socket 模块测试
 * ============================================================ */

#include "test_runner.h"
#include "../vox_socket.h"
#include "../vox_mpool.h"

/* 测试创建和销毁socket */
static void test_socket_create_destroy(vox_mpool_t* mpool) {
    (void)mpool;  /* 未使用的参数 */
    vox_socket_t sock;
    
    /* 测试TCP socket */
    TEST_ASSERT_EQ(vox_socket_create(&sock, VOX_SOCKET_TCP, VOX_AF_INET), 0, "创建TCP socket失败");
    TEST_ASSERT_NE(sock.fd, VOX_INVALID_SOCKET, "socket文件描述符无效");
    vox_socket_destroy(&sock);
    
    /* 测试UDP socket */
    TEST_ASSERT_EQ(vox_socket_create(&sock, VOX_SOCKET_UDP, VOX_AF_INET), 0, "创建UDP socket失败");
    TEST_ASSERT_NE(sock.fd, VOX_INVALID_SOCKET, "socket文件描述符无效");
    vox_socket_destroy(&sock);
}

/* 测试socket选项设置 */
static void test_socket_options(vox_mpool_t* mpool) {
    (void)mpool;  /* 未使用的参数 */
    vox_socket_t sock;
    TEST_ASSERT_EQ(vox_socket_create(&sock, VOX_SOCKET_TCP, VOX_AF_INET), 0, "创建socket失败");
    
    /* 测试非阻塞模式 */
    TEST_ASSERT_EQ(vox_socket_set_nonblock(&sock, true), 0, "设置非阻塞模式失败");
    TEST_ASSERT_EQ(vox_socket_set_nonblock(&sock, false), 0, "设置阻塞模式失败");
    
    /* 测试地址重用 */
    TEST_ASSERT_EQ(vox_socket_set_reuseaddr(&sock, true), 0, "设置地址重用失败");
    
    /* 测试缓冲区大小 */
    TEST_ASSERT_EQ(vox_socket_set_recv_buffer_size(&sock, 8192), 0, "设置接收缓冲区大小失败");
    TEST_ASSERT_EQ(vox_socket_set_send_buffer_size(&sock, 8192), 0, "设置发送缓冲区大小失败");
    
    /* 测试保持连接 */
    TEST_ASSERT_EQ(vox_socket_set_keepalive(&sock, true), 0, "设置保持连接失败");
    
    /* 测试TCP无延迟（仅TCP） */
    TEST_ASSERT_EQ(vox_socket_set_tcp_nodelay(&sock, true), 0, "设置TCP无延迟失败");
    
    vox_socket_destroy(&sock);
}

/* 测试地址解析 */
static void test_socket_address_parsing(vox_mpool_t* mpool) {
    (void)mpool;  /* 未使用的参数 */
    vox_socket_addr_t addr;
    
    /* 测试IPv4地址解析 */
    TEST_ASSERT_EQ(vox_socket_parse_address("127.0.0.1", 8080, &addr), 0, "解析IPv4地址失败");
    TEST_ASSERT_EQ(addr.family, VOX_AF_INET, "地址族不正确");
    
    /* 测试IPv6地址解析 */
    TEST_ASSERT_EQ(vox_socket_parse_address("::1", 8080, &addr), 0, "解析IPv6地址失败");
    TEST_ASSERT_EQ(addr.family, VOX_AF_INET6, "地址族不正确");
    
    /* 注意：vox_socket_format_address函数可能不存在，这里只测试地址解析 */
}

/* 测试套件 */
test_case_t test_socket_cases[] = {
    {"create_destroy", test_socket_create_destroy},
    {"options", test_socket_options},
    {"address_parsing", test_socket_address_parsing},
};

test_suite_t test_socket_suite = {
    "vox_socket",
    test_socket_cases,
    sizeof(test_socket_cases) / sizeof(test_socket_cases[0])
};
