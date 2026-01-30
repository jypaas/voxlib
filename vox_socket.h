/* 
 * vox_socket.h - 跨平台Socket抽象接口
 * 提供统一的TCP/UDP socket操作API
*/

#ifndef VOX_SOCKET_H
#define VOX_SOCKET_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "vox_os.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Socket文件描述符类型定义 */
#ifdef VOX_OS_WINDOWS
    typedef SOCKET vox_socket_fd_t;
    #define VOX_INVALID_SOCKET INVALID_SOCKET
    #ifndef socklen_t
        typedef int socklen_t;
    #endif
#else
    typedef int vox_socket_fd_t;
    #define VOX_INVALID_SOCKET (-1)
    /* socklen_t 在 POSIX 系统中由 <sys/socket.h> 定义 */
#endif

/* 特殊标志位 */
#define VOX_PORT_REUSE_FLAG 0x01  /* 启用端口重用，支持多进程/多线程监听 */

/* Socket类型 */
typedef enum {
    VOX_SOCKET_TCP = 0,  /* TCP socket */
    VOX_SOCKET_UDP = 1   /* UDP socket */
} vox_socket_type_t;

/* Socket地址族 */
typedef enum {
    VOX_AF_INET = 0,   /* IPv4 */
    VOX_AF_INET6 = 1   /* IPv6 */
} vox_address_family_t;

/* Socket结构体 */
struct vox_socket {
    vox_socket_fd_t fd;              /* Socket文件描述符 */
    vox_socket_type_t type;          /* Socket类型 */
    vox_address_family_t family;     /* 地址族 */
    bool nonblock;                   /* 是否非阻塞 */
};

typedef struct vox_socket vox_socket_t;

/* Socket地址结构 */
typedef struct {
    vox_address_family_t family;  /* 地址族 */
    union {
        struct {
            uint32_t addr;        /* IPv4地址（网络字节序） */
            uint16_t port;        /* 端口（网络字节序） */
        } ipv4;
        struct {
            uint8_t addr[16];     /* IPv6地址 */
            uint16_t port;        /* 端口（网络字节序） */
        } ipv6;
    } u;
} vox_socket_addr_t;

/* ===== Socket创建和销毁 ===== */

/**
 * 创建socket
 * @param sock 外部传入的socket结构体指针
 * @param type Socket类型（TCP/UDP）
 * @param family 地址族（IPv4/IPv6）
 * @return 成功返回0，失败返回-1
 */
int vox_socket_create(vox_socket_t* sock, vox_socket_type_t type, vox_address_family_t family);

/**
 * 销毁socket（关闭socket，不释放内存）
 * @param sock Socket指针
 */
void vox_socket_destroy(vox_socket_t* sock);

/* ===== Socket配置 ===== */

/**
 * 设置socket为非阻塞模式
 * @param sock Socket指针
 * @param nonblock 是否非阻塞
 * @return 成功返回0，失败返回-1
 */
int vox_socket_set_nonblock(vox_socket_t* sock, bool nonblock);

/**
 * 设置socket选项（SO_REUSEADDR等）
 * @param sock Socket指针
 * @param reuseaddr 是否允许地址重用
 * @return 成功返回0，失败返回-1
 */
int vox_socket_set_reuseaddr(vox_socket_t* sock, bool reuseaddr);

/**
 * 设置socket端口重用选项（SO_REUSEPORT，仅非Windows平台）
 * @param sock Socket指针
 * @param reuseport 是否允许端口重用
 * @return 成功返回0，失败返回-1
 * @note 此选项仅在Linux、macOS等POSIX系统上可用，Windows不支持
 */
int vox_socket_set_reuseport(vox_socket_t* sock, bool reuseport);

/**
 * 设置接收缓冲区大小
 * @param sock Socket指针
 * @param size 缓冲区大小（字节）
 * @return 成功返回0，失败返回-1
 */
int vox_socket_set_recv_buffer_size(vox_socket_t* sock, int size);

/**
 * 设置发送缓冲区大小
 * @param sock Socket指针
 * @param size 缓冲区大小（字节）
 * @return 成功返回0，失败返回-1
 */
int vox_socket_set_send_buffer_size(vox_socket_t* sock, int size);

/**
 * 设置socket保持连接选项（SO_KEEPALIVE）
 * @param sock Socket指针
 * @param keepalive 是否启用保持连接
 * @return 成功返回0，失败返回-1
 */
int vox_socket_set_keepalive(vox_socket_t* sock, bool keepalive);

/**
 * 设置TCP无延迟选项（TCP_NODELAY，仅TCP socket）
 * @param sock Socket指针
 * @param nodelay 是否禁用Nagle算法
 * @return 成功返回0，失败返回-1
 * @note 仅对TCP socket有效，UDP socket调用此函数会失败
 */
int vox_socket_set_tcp_nodelay(vox_socket_t* sock, bool nodelay);

/**
 * 设置socket延迟关闭选项（SO_LINGER）
 * @param sock Socket指针
 * @param enable 是否启用延迟关闭
 * @param seconds 延迟关闭时间（秒），仅在enable为true时有效
 * @return 成功返回0，失败返回-1
 */
int vox_socket_set_linger(vox_socket_t* sock, bool enable, int seconds);

/**
 * 设置接收超时（SO_RCVTIMEO）
 * @param sock Socket指针
 * @param timeout_ms 超时时间（毫秒），0表示禁用超时
 * @return 成功返回0，失败返回-1
 */
int vox_socket_set_recv_timeout(vox_socket_t* sock, int timeout_ms);

/**
 * 设置发送超时（SO_SNDTIMEO）
 * @param sock Socket指针
 * @param timeout_ms 超时时间（毫秒），0表示禁用超时
 * @return 成功返回0，失败返回-1
 */
int vox_socket_set_send_timeout(vox_socket_t* sock, int timeout_ms);

/**
 * 设置socket广播选项（SO_BROADCAST，仅UDP socket）
 * @param sock Socket指针
 * @param broadcast 是否允许广播
 * @return 成功返回0，失败返回-1
 * @note 仅对UDP socket有效
 */
int vox_socket_set_broadcast(vox_socket_t* sock, bool broadcast);

/**
 * 设置IPv4服务类型（IP_TOS）
 * @param sock Socket指针
 * @param tos 服务类型值（0-255）
 * @return 成功返回0，失败返回-1
 * @note 仅对IPv4 socket有效
 */
int vox_socket_set_ip_tos(vox_socket_t* sock, int tos);

/**
 * 设置IPv6流量类别（IPV6_TCLASS）
 * @param sock Socket指针
 * @param tclass 流量类别值（0-255）
 * @return 成功返回0，失败返回-1
 * @note 仅对IPv6 socket有效
 */
int vox_socket_set_ipv6_tclass(vox_socket_t* sock, int tclass);

/* ===== 地址操作 ===== */

/**
 * 解析地址字符串（支持IPv4和IPv6）
 * @param addr_str 地址字符串（如 "127.0.0.1" 或 "::1"）
 * @param port 端口号（主机字节序）
 * @param addr 输出地址结构
 * @return 成功返回0，失败返回-1
 */
int vox_socket_parse_address(const char* addr_str, uint16_t port, vox_socket_addr_t* addr);

/**
 * 将地址转换为字符串
 * @param addr 地址结构
 * @param buf 输出缓冲区
 * @param size 缓冲区大小
 * @return 成功返回写入的字符数，失败返回-1
 */
int vox_socket_address_to_string(const vox_socket_addr_t* addr, char* buf, size_t size);

/**
 * 获取端口号（主机字节序）
 * @param addr 地址结构
 * @return 返回端口号
 */
uint16_t vox_socket_get_port(const vox_socket_addr_t* addr);

/* ===== TCP操作 ===== */

/**
 * 绑定地址
 * @param sock Socket指针
 * @param addr 地址结构
 * @return 成功返回0，失败返回-1
 */
int vox_socket_bind(vox_socket_t* sock, const vox_socket_addr_t* addr);

/**
 * 监听连接
 * @param sock Socket指针
 * @param backlog 最大等待连接数
 * @return 成功返回0，失败返回-1
 */
int vox_socket_listen(vox_socket_t* sock, int backlog);

/**
 * 接受连接（仅TCP）
 * @param sock 监听socket
 * @param client_sock 外部传入的客户端socket结构体指针
 * @param client_addr 输出客户端地址（可为NULL）
 * @return 成功返回0，失败返回-1
 */
int vox_socket_accept(vox_socket_t* sock, vox_socket_t* client_sock, vox_socket_addr_t* client_addr);

/**
 * 连接到服务器
 * @param sock Socket指针
 * @param addr 服务器地址
 * @return 成功返回0，失败返回-1
 */
int vox_socket_connect(vox_socket_t* sock, const vox_socket_addr_t* addr);

/* ===== 数据收发 ===== */

/**
 * 发送数据（TCP）
 * @param sock Socket指针
 * @param buf 数据缓冲区
 * @param len 数据长度
 * @return 成功返回发送的字节数，失败返回-1
 */
int64_t vox_socket_send(vox_socket_t* sock, const void* buf, size_t len);

/**
 * 接收数据（TCP）
 * @param sock Socket指针
 * @param buf 数据缓冲区
 * @param len 缓冲区大小
 * @return 成功返回接收的字节数，失败返回-1，连接关闭返回0
 */
int64_t vox_socket_recv(vox_socket_t* sock, void* buf, size_t len);

/**
 * 发送数据到指定地址（UDP）
 * @param sock Socket指针
 * @param buf 数据缓冲区
 * @param len 数据长度
 * @param addr 目标地址
 * @return 成功返回发送的字节数，失败返回-1
 */
int64_t vox_socket_sendto(vox_socket_t* sock, const void* buf, size_t len, 
                          const vox_socket_addr_t* addr);

/**
 * 接收数据并获取发送方地址（UDP）
 * @param sock Socket指针
 * @param buf 数据缓冲区
 * @param len 缓冲区大小
 * @param addr 输出发送方地址（可为NULL）
 * @return 成功返回接收的字节数，失败返回-1
 */
int64_t vox_socket_recvfrom(vox_socket_t* sock, void* buf, size_t len, 
                            vox_socket_addr_t* addr);

/* ===== 地址信息 ===== */

/**
 * 获取本地地址
 * @param sock Socket指针
 * @param addr 输出地址结构
 * @return 成功返回0，失败返回-1
 */
int vox_socket_get_local_addr(vox_socket_t* sock, vox_socket_addr_t* addr);

/**
 * 获取对端地址（仅TCP）
 * @param sock Socket指针
 * @param addr 输出地址结构
 * @return 成功返回0，失败返回-1
 */
int vox_socket_get_peer_addr(vox_socket_t* sock, vox_socket_addr_t* addr);

/* ===== 错误处理 ===== */

/**
 * 获取最后的错误码
 * @return 返回错误码
 */
int vox_socket_get_error(void);

/**
 * 获取错误描述
 * @param error_code 错误码
 * @param buf 输出缓冲区
 * @param size 缓冲区大小
 * @return 成功返回写入的字符数，失败返回-1
 */
int vox_socket_error_string(int error_code, char* buf, size_t size);

/* ===== 工具函数 ===== */

/**
 * 初始化socket库（Windows需要）
 * @return 成功返回0，失败返回-1
 */
int vox_socket_init(void);

/**
 * 清理socket库（Windows需要）
 */
void vox_socket_cleanup(void);

#ifdef __cplusplus
}
#endif

#endif /* VOX_SOCKET_H */
