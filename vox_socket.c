/*
 * vox_socket.c - 跨平台Socket实现
 * 支持Windows (WinSock) 和 POSIX (BSD sockets)
 */

#ifdef VOX_OS_MACOS
#define _DARWIN_C_SOURCE 1
#endif

#include "vox_socket.h"
#include "vox_os.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#ifndef VOX_OS_WINDOWS
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <netinet/in.h>
#include <sys/socket.h>
#if defined(VOX_OS_LINUX)
#include <sys/sendfile.h>
#elif defined(VOX_OS_MACOS) || defined(VOX_OS_FREEBSD)
#include <sys/types.h>
#include <sys/uio.h>
#endif
#endif

#ifdef VOX_OS_WINDOWS
#include <windows.h>
#endif

/* 全局状态 */
static bool g_socket_initialized = false;

/* ===== 初始化/清理 ===== */

int vox_socket_init(void) {
#ifdef VOX_OS_WINDOWS
    if (g_socket_initialized) {
        return 0;
    }
    
    WSADATA wsa_data;
    int result = WSAStartup(MAKEWORD(2, 2), &wsa_data);
    if (result != 0) {
        return -1;
    }
    
    g_socket_initialized = true;
    return 0;
#else
    g_socket_initialized = true;
    return 0;
#endif
}

void vox_socket_cleanup(void) {
#ifdef VOX_OS_WINDOWS
    if (g_socket_initialized) {
        WSACleanup();
        g_socket_initialized = false;
    }
#else
    g_socket_initialized = false;
#endif
}

/* ===== 地址转换 ===== */

static int convert_address_family(vox_address_family_t family) {
    return (family == VOX_AF_INET) ? AF_INET : AF_INET6;
}

static int convert_socket_type(vox_socket_type_t type) {
    return (type == VOX_SOCKET_TCP) ? SOCK_STREAM : SOCK_DGRAM;
}

static void convert_to_sockaddr(const vox_socket_addr_t* addr, struct sockaddr* sa, socklen_t* len) {
    if (addr->family == VOX_AF_INET) {
        struct sockaddr_in* sin = (struct sockaddr_in*)sa;
        memset(sa, 0, sizeof(struct sockaddr_storage));  /* 清零整个storage，避免遗留数据 */
        sin->sin_family = AF_INET;
        sin->sin_addr.s_addr = addr->u.ipv4.addr;
        sin->sin_port = addr->u.ipv4.port;
        *len = sizeof(struct sockaddr_in);
    } else {
        struct sockaddr_in6* sin6 = (struct sockaddr_in6*)sa;
        memset(sa, 0, sizeof(struct sockaddr_storage));  /* 清零整个storage，避免遗留数据 */
        sin6->sin6_family = AF_INET6;
        memcpy(&sin6->sin6_addr, addr->u.ipv6.addr, 16);
        sin6->sin6_port = addr->u.ipv6.port;
        *len = sizeof(struct sockaddr_in6);
    }
}

static void convert_from_sockaddr(const struct sockaddr* sa, vox_socket_addr_t* addr) {
    if (sa->sa_family == AF_INET) {
        const struct sockaddr_in* sin = (const struct sockaddr_in*)sa;
        addr->family = VOX_AF_INET;
        addr->u.ipv4.addr = sin->sin_addr.s_addr;
        addr->u.ipv4.port = sin->sin_port;
    } else {
        const struct sockaddr_in6* sin6 = (const struct sockaddr_in6*)sa;
        addr->family = VOX_AF_INET6;
        memcpy(addr->u.ipv6.addr, &sin6->sin6_addr, 16);
        addr->u.ipv6.port = sin6->sin6_port;
    }
}

/* ===== Socket创建和销毁 ===== */

int vox_socket_create(vox_socket_t* sock, vox_socket_type_t type, vox_address_family_t family) {
    if (!sock) {
        return -1;
    }
    
    if (!g_socket_initialized) {
        if (vox_socket_init() != 0) {
            return -1;
        }
    }
    
    int domain = convert_address_family(family);
    int sock_type = convert_socket_type(type);
    
    vox_socket_fd_t fd = socket(domain, sock_type, 0);
    if (fd == VOX_INVALID_SOCKET) {
        return -1;
    }
    
    sock->fd = fd;
    sock->type = type;
    sock->family = family;
    sock->nonblock = false;
    
    return 0;
}

void vox_socket_destroy(vox_socket_t* sock) {
    if (!sock) return;
    
    if (sock->fd != VOX_INVALID_SOCKET) {
#ifdef VOX_OS_WINDOWS
        closesocket(sock->fd);
#else
        close(sock->fd);
#endif
        sock->fd = VOX_INVALID_SOCKET;
    }
}

/* ===== Socket配置 ===== */

int vox_socket_set_nonblock(vox_socket_t* sock, bool nonblock) {
    if (!sock) return -1;
    
#ifdef VOX_OS_WINDOWS
    u_long mode = nonblock ? 1 : 0;
    if (ioctlsocket(sock->fd, FIONBIO, &mode) == SOCKET_ERROR) {
        return -1;
    }
#else
    int flags = fcntl(sock->fd, F_GETFL, 0);
    if (flags == -1) return -1;
    
    if (nonblock) {
        flags |= O_NONBLOCK;
    } else {
        flags &= ~O_NONBLOCK;
    }
    
    if (fcntl(sock->fd, F_SETFL, flags) == -1) {
        return -1;
    }
#endif
    
    sock->nonblock = nonblock;
    return 0;
}

int vox_socket_set_reuseaddr(vox_socket_t* sock, bool reuseaddr) {
    if (!sock) return -1;
    
    int opt = reuseaddr ? 1 : 0;
    if (setsockopt(sock->fd, SOL_SOCKET, SO_REUSEADDR, 
                   (const char*)&opt, sizeof(opt)) == SOCKET_ERROR) {
        return -1;
    }
    
    return 0;
}

int vox_socket_set_reuseport(vox_socket_t* sock, bool reuseport) {
    if (!sock) return -1;
    
#ifndef VOX_OS_WINDOWS
    /* SO_REUSEPORT 仅在非Windows平台可用 */
#ifdef SO_REUSEPORT
    int opt = reuseport ? 1 : 0;
    if (setsockopt(sock->fd, SOL_SOCKET, SO_REUSEPORT, 
                   (const char*)&opt, sizeof(opt)) == SOCKET_ERROR) {
        return -1;
    }
    return 0;
#else
    /* 如果系统不支持 SO_REUSEPORT，返回错误 */
    (void)reuseport;
    return -1;
#endif
#else
    /* Windows 不支持 SO_REUSEPORT */
    (void)reuseport;  /* 避免未使用参数警告 */
    return -1;
#endif
}

int vox_socket_set_recv_buffer_size(vox_socket_t* sock, int size) {
    if (!sock) return -1;
    
    if (setsockopt(sock->fd, SOL_SOCKET, SO_RCVBUF, 
                   (const char*)&size, sizeof(size)) == SOCKET_ERROR) {
        return -1;
    }
    
    return 0;
}

int vox_socket_set_send_buffer_size(vox_socket_t* sock, int size) {
    if (!sock) return -1;
    
    if (setsockopt(sock->fd, SOL_SOCKET, SO_SNDBUF, 
                   (const char*)&size, sizeof(size)) == SOCKET_ERROR) {
        return -1;
    }
    
    return 0;
}

int vox_socket_set_keepalive(vox_socket_t* sock, bool keepalive) {
    if (!sock) return -1;
    
    int opt = keepalive ? 1 : 0;
    if (setsockopt(sock->fd, SOL_SOCKET, SO_KEEPALIVE, 
                   (const char*)&opt, sizeof(opt)) == SOCKET_ERROR) {
        return -1;
    }
    
    return 0;
}

int vox_socket_set_tcp_nodelay(vox_socket_t* sock, bool nodelay) {
    if (!sock) return -1;
    
    /* 仅对TCP socket有效 */
    if (sock->type != VOX_SOCKET_TCP) {
        return -1;
    }
    
    int opt = nodelay ? 1 : 0;
#ifdef VOX_OS_WINDOWS
    if (setsockopt(sock->fd, IPPROTO_TCP, TCP_NODELAY, 
                   (const char*)&opt, sizeof(opt)) == SOCKET_ERROR) {
        return -1;
    }
#else
    if (setsockopt(sock->fd, IPPROTO_TCP, TCP_NODELAY, 
                   &opt, sizeof(opt)) == SOCKET_ERROR) {
        return -1;
    }
#endif
    return 0;
}

int vox_socket_set_linger(vox_socket_t* sock, bool enable, int seconds) {
    if (!sock) return -1;
    
#ifdef VOX_OS_WINDOWS
    struct linger linger_opt;
    linger_opt.l_onoff = enable ? 1 : 0;
    linger_opt.l_linger = (unsigned short)seconds;
    if (setsockopt(sock->fd, SOL_SOCKET, SO_LINGER, 
                   (const char*)&linger_opt, sizeof(linger_opt)) == SOCKET_ERROR) {
        return -1;
    }
#else
    struct linger linger_opt;
    linger_opt.l_onoff = enable ? 1 : 0;
    linger_opt.l_linger = seconds;
    if (setsockopt(sock->fd, SOL_SOCKET, SO_LINGER, 
                   &linger_opt, sizeof(linger_opt)) == SOCKET_ERROR) {
        return -1;
    }
#endif
    return 0;
}

int vox_socket_set_recv_timeout(vox_socket_t* sock, int timeout_ms) {
    if (!sock) return -1;
    
#ifdef VOX_OS_WINDOWS
    DWORD timeout = (DWORD)timeout_ms;
    if (setsockopt(sock->fd, SOL_SOCKET, SO_RCVTIMEO, 
                   (const char*)&timeout, sizeof(timeout)) == SOCKET_ERROR) {
        return -1;
    }
#else
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    if (setsockopt(sock->fd, SOL_SOCKET, SO_RCVTIMEO, 
                   &tv, sizeof(tv)) == SOCKET_ERROR) {
        return -1;
    }
#endif
    return 0;
}

int vox_socket_set_send_timeout(vox_socket_t* sock, int timeout_ms) {
    if (!sock) return -1;
    
#ifdef VOX_OS_WINDOWS
    DWORD timeout = (DWORD)timeout_ms;
    if (setsockopt(sock->fd, SOL_SOCKET, SO_SNDTIMEO, 
                   (const char*)&timeout, sizeof(timeout)) == SOCKET_ERROR) {
        return -1;
    }
#else
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    if (setsockopt(sock->fd, SOL_SOCKET, SO_SNDTIMEO, 
                   &tv, sizeof(tv)) == SOCKET_ERROR) {
        return -1;
    }
#endif
    return 0;
}

int vox_socket_set_broadcast(vox_socket_t* sock, bool broadcast) {
    if (!sock) return -1;
    
    /* 仅对UDP socket有效 */
    if (sock->type != VOX_SOCKET_UDP) {
        return -1;
    }
    
    int opt = broadcast ? 1 : 0;
    if (setsockopt(sock->fd, SOL_SOCKET, SO_BROADCAST, 
                   (const char*)&opt, sizeof(opt)) == SOCKET_ERROR) {
        return -1;
    }
    
    return 0;
}

int vox_socket_set_ip_tos(vox_socket_t* sock, int tos) {
    if (!sock) return -1;
    
    /* 仅对IPv4 socket有效 */
    if (sock->family != VOX_AF_INET) {
        return -1;
    }
    
    if (tos < 0 || tos > 255) {
        return -1;
    }
    
#ifdef VOX_OS_WINDOWS
    int opt = tos;
    if (setsockopt(sock->fd, IPPROTO_IP, IP_TOS, 
                   (const char*)&opt, sizeof(opt)) == SOCKET_ERROR) {
        return -1;
    }
#else
    int opt = tos;
    if (setsockopt(sock->fd, IPPROTO_IP, IP_TOS, 
                   &opt, sizeof(opt)) == SOCKET_ERROR) {
        return -1;
    }
#endif
    return 0;
}

int vox_socket_set_ipv6_tclass(vox_socket_t* sock, int tclass) {
    if (!sock) return -1;
    
    /* 仅对IPv6 socket有效 */
    if (sock->family != VOX_AF_INET6) {
        return -1;
    }
    
    if (tclass < 0 || tclass > 255) {
        return -1;
    }
    
#ifndef VOX_OS_WINDOWS
#ifdef IPV6_TCLASS
    int opt = tclass;
    if (setsockopt(sock->fd, IPPROTO_IPV6, IPV6_TCLASS, 
                   &opt, sizeof(opt)) == SOCKET_ERROR) {
        return -1;
    }
    return 0;
#else
    /* 系统不支持IPV6_TCLASS */
    (void)tclass;
    return -1;
#endif
#else
    /* Windows 对 IPv6 TCLASS 的支持有限 */
#ifdef IPV6_TCLASS
    int opt = tclass;
    if (setsockopt(sock->fd, IPPROTO_IPV6, IPV6_TCLASS, 
                   (const char*)&opt, sizeof(opt)) == SOCKET_ERROR) {
        return -1;
    }
    return 0;
#else
    /* Windows不支持IPV6_TCLASS */
    (void)tclass;
    return -1;
#endif
#endif
}

/* ===== 地址操作 ===== */

int vox_socket_parse_address(const char* addr_str, uint16_t port, vox_socket_addr_t* addr) {
    if (!addr_str || !addr) return -1;
    
    memset(addr, 0, sizeof(*addr));
    
    /* 尝试IPv4 */
    struct sockaddr_in sin;
    if (inet_pton(AF_INET, addr_str, &sin.sin_addr) == 1) {
        addr->family = VOX_AF_INET;
        addr->u.ipv4.addr = sin.sin_addr.s_addr;
        addr->u.ipv4.port = htons(port);
        return 0;
    }
    
    /* 尝试IPv6 */
    struct sockaddr_in6 sin6;
    if (inet_pton(AF_INET6, addr_str, &sin6.sin6_addr) == 1) {
        addr->family = VOX_AF_INET6;
        memcpy(addr->u.ipv6.addr, &sin6.sin6_addr, 16);
        addr->u.ipv6.port = htons(port);
        return 0;
    }
    
    return -1;
}

int vox_socket_address_to_string(const vox_socket_addr_t* addr, char* buf, size_t size) {
    if (!addr || !buf || size == 0) return -1;
    
    if (addr->family == VOX_AF_INET) {
        struct in_addr in;
        in.s_addr = addr->u.ipv4.addr;
        const char* str = inet_ntop(AF_INET, &in, buf, size);
        if (!str) return -1;
        return (int)strlen(buf);
    } else {
        struct in6_addr in6;
        memcpy(&in6, addr->u.ipv6.addr, 16);
        const char* str = inet_ntop(AF_INET6, &in6, buf, size);
        if (!str) return -1;
        return (int)strlen(buf);
    }
}

uint16_t vox_socket_get_port(const vox_socket_addr_t* addr) {
    if (!addr) return 0;
    
    if (addr->family == VOX_AF_INET) {
        return ntohs(addr->u.ipv4.port);
    } else {
        return ntohs(addr->u.ipv6.port);
    }
}

void vox_socket_set_port(vox_socket_addr_t* addr, uint16_t port) {
    if (!addr) return;
    if (addr->family == VOX_AF_INET) {
        addr->u.ipv4.port = htons(port);
    } else {
        addr->u.ipv6.port = htons(port);
    }
}

/* ===== TCP操作 ===== */

int vox_socket_bind(vox_socket_t* sock, const vox_socket_addr_t* addr) {
    if (!sock || !addr) return -1;
    
    struct sockaddr_storage sa;
    socklen_t sa_len;
    convert_to_sockaddr(addr, (struct sockaddr*)&sa, &sa_len);
    
    if (bind(sock->fd, (struct sockaddr*)&sa, sa_len) == SOCKET_ERROR) {
        return -1;
    }
    
    return 0;
}

int vox_socket_listen(vox_socket_t* sock, int backlog) {
    if (!sock) return -1;
    
    if (listen(sock->fd, backlog) == SOCKET_ERROR) {
        return -1;
    }
    
    return 0;
}

int vox_socket_accept(vox_socket_t* sock, vox_socket_t* client_sock, vox_socket_addr_t* client_addr) {
    if (!sock || !client_sock) {
        return -1;
    }
    
    /* 使用传统的accept */
    struct sockaddr_storage sa;
    socklen_t sa_len = sizeof(sa);
    
    vox_socket_fd_t client_fd = accept(sock->fd, (struct sockaddr*)&sa, &sa_len);
    if (client_fd == VOX_INVALID_SOCKET) {
        return -1;
    }
    
    client_sock->fd = client_fd;
    client_sock->type = sock->type;
    client_sock->family = sock->family;
    client_sock->nonblock = false;  /* 新socket默认为阻塞模式 */
    
    /* 如果父socket是非阻塞的，需要对子socket也设置非阻塞 */
    if (sock->nonblock) {
        if (vox_socket_set_nonblock(client_sock, true) != 0) {
            vox_socket_destroy(client_sock);
            return -1;
        }
    }
    
    if (client_addr) {
        convert_from_sockaddr((struct sockaddr*)&sa, client_addr);
    }
    
    return 0;
}

int vox_socket_connect(vox_socket_t* sock, const vox_socket_addr_t* addr) {
    if (!sock || !addr) return -1;
    
    struct sockaddr_storage sa;
    socklen_t sa_len;
    convert_to_sockaddr(addr, (struct sockaddr*)&sa, &sa_len);
    
    if (connect(sock->fd, (struct sockaddr*)&sa, sa_len) == SOCKET_ERROR) {
#ifdef VOX_OS_WINDOWS
        int err = WSAGetLastError();
        if (err != WSAEWOULDBLOCK) {
            return -1;
        }
#else
        if (errno != EINPROGRESS) {
            return -1;
        }
#endif
    }
    
    return 0;
}

/* ===== 数据收发 ===== */

int64_t vox_socket_send(vox_socket_t* sock, const void* buf, size_t len) {
    if (!sock || !buf) return -1;
    
#ifdef VOX_OS_WINDOWS
    int sent = send(sock->fd, (const char*)buf, (int)len, 0);
#else
    ssize_t sent = send(sock->fd, buf, len, 0);
#endif
    if (sent == SOCKET_ERROR) {
        return -1;
    }
    
    return (int64_t)sent;
}

int64_t vox_socket_recv(vox_socket_t* sock, void* buf, size_t len) {
    if (!sock || !buf) return -1;
    if (len == 0) return 0;
    
#ifdef VOX_OS_WINDOWS
    int received = recv(sock->fd, (char*)buf, (int)len, 0);
#else
    ssize_t received = recv(sock->fd, buf, len, 0);
#endif
    if (received == SOCKET_ERROR) {
        return -1;
    }
    
    if (received == 0) {
        /* 注意：对于TCP socket，返回0表示连接关闭
         * 对于UDP socket，返回0表示接收到0字节的数据包（不常见） */
        return 0;
    }
    
    return (int64_t)received;
}

int vox_socket_sendfile(vox_socket_t* sock, intptr_t file_fd_or_handle,
                        int64_t offset, size_t count, size_t* out_sent) {
    if (!sock || sock->type != VOX_SOCKET_TCP) return -1;
    if (file_fd_or_handle == (intptr_t)-1) return -1;
    if (sock->fd == VOX_INVALID_SOCKET) return -1;
    if (out_sent) *out_sent = 0;
    if (count == 0) return 0;

#ifdef VOX_OS_WINDOWS
    {
        HANDLE hFile = (HANDLE)file_fd_or_handle;
        SOCKET s = sock->fd;
        LARGE_INTEGER li;
        li.QuadPart = offset;
        if (SetFilePointerEx(hFile, li, NULL, FILE_BEGIN) == 0) return -1;

        /* 优先使用 TransmitFile 零拷贝（mswsock.dll）；失败或不可用时回退到 ReadFile+send */
        if (count <= 0xFFFFFFFFu) {
            typedef BOOL (WINAPI *TransmitFile_t)(SOCKET, HANDLE, DWORD, DWORD, void*, void*, DWORD);
            static int transmitfile_loaded = 0;
            static TransmitFile_t pTransmitFile = NULL;
            if (!transmitfile_loaded) {
                HMODULE h = GetModuleHandleA("mswsock.dll");
                pTransmitFile = h ? (TransmitFile_t)GetProcAddress(h, "TransmitFile") : NULL;
                transmitfile_loaded = 1;
            }
            if (pTransmitFile && pTransmitFile(s, hFile, (DWORD)count, 0, NULL, NULL, 0)) {
                if (out_sent) *out_sent = count;
                return 0;
            }
        }

        char buf[65536];
        size_t total = 0;
        while (total < count) {
            DWORD to_read = (DWORD)((count - total) > sizeof(buf) ? sizeof(buf) : (count - total));
            DWORD nread = 0;
            if (!ReadFile(hFile, buf, to_read, &nread, NULL) || nread == 0) break;
            int nsent = send(s, buf, (int)nread, 0);
            if (nsent == SOCKET_ERROR || nsent <= 0) break;
            total += (size_t)nsent;
            if ((size_t)nsent < nread) break;
        }
        if (out_sent) *out_sent = total;
        return (total > 0) ? 0 : -1;
    }
#elif defined(VOX_OS_LINUX)
    {
        off_t off = (off_t)offset;
        size_t total = 0;
        int out_fd = (int)sock->fd;
        int in_fd = (int)file_fd_or_handle;
        while (total < count) {
            size_t chunk = count - total;
            ssize_t n = sendfile(out_fd, in_fd, &off, chunk);
            if (n <= 0) break;
            total += (size_t)n;
        }
        if (out_sent) *out_sent = total;
        return (total > 0) ? 0 : -1;
    }
#elif defined(VOX_OS_MACOS) || defined(VOX_OS_FREEBSD)
    {
        int fd = (int)file_fd_or_handle;
        int s = (int)sock->fd;
        off_t len = (off_t)count;
        int r = sendfile(fd, s, offset, &len, NULL, 0);
        if (out_sent) *out_sent = (size_t)(r == 0 ? len : 0);
        return (r == 0 && len > 0) ? 0 : -1;
    }
#else
    (void)offset;
    (void)count;
    return -1;
#endif
}

int64_t vox_socket_sendto(vox_socket_t* sock, const void* buf, size_t len, 
                          const vox_socket_addr_t* addr) {
    if (!sock || !buf || !addr) return -1;
    
    struct sockaddr_storage sa;
    socklen_t sa_len;
    convert_to_sockaddr(addr, (struct sockaddr*)&sa, &sa_len);
    
#ifdef VOX_OS_WINDOWS
    int sent = sendto(sock->fd, (const char*)buf, (int)len, 0, 
                      (struct sockaddr*)&sa, sa_len);
#else
    ssize_t sent = sendto(sock->fd, buf, len, 0, 
                          (struct sockaddr*)&sa, sa_len);
#endif
    if (sent == SOCKET_ERROR) {
        return -1;
    }
    
    return (int64_t)sent;
}

int64_t vox_socket_recvfrom(vox_socket_t* sock, void* buf, size_t len, 
                            vox_socket_addr_t* addr) {
    if (!sock || !buf) return -1;
    
    struct sockaddr_storage sa;
    socklen_t sa_len = sizeof(sa);
    
#ifdef VOX_OS_WINDOWS
    int received = recvfrom(sock->fd, (char*)buf, (int)len, 0, 
                            (struct sockaddr*)&sa, &sa_len);
#else
    ssize_t received = recvfrom(sock->fd, buf, len, 0, 
                                (struct sockaddr*)&sa, &sa_len);
#endif
    if (received == SOCKET_ERROR) {
        return -1;
    }
    
    if (addr) {
        convert_from_sockaddr((struct sockaddr*)&sa, addr);
    }
    
    return (int64_t)received;
}

/* ===== 地址信息 ===== */

int vox_socket_get_local_addr(vox_socket_t* sock, vox_socket_addr_t* addr) {
    if (!sock || !addr) return -1;
    
    struct sockaddr_storage sa;
    socklen_t sa_len = sizeof(sa);
    
    if (getsockname(sock->fd, (struct sockaddr*)&sa, &sa_len) == SOCKET_ERROR) {
        return -1;
    }
    
    convert_from_sockaddr((struct sockaddr*)&sa, addr);
    return 0;
}

int vox_socket_get_peer_addr(vox_socket_t* sock, vox_socket_addr_t* addr) {
    if (!sock || !addr) return -1;
    
    struct sockaddr_storage sa;
    socklen_t sa_len = sizeof(sa);
    
    if (getpeername(sock->fd, (struct sockaddr*)&sa, &sa_len) == SOCKET_ERROR) {
        return -1;
    }
    
    convert_from_sockaddr((struct sockaddr*)&sa, addr);
    return 0;
}

/* ===== 错误处理 ===== */

int vox_socket_get_error(void) {
#ifdef VOX_OS_WINDOWS
    return WSAGetLastError();
#else
    return errno;
#endif
}

int vox_socket_error_string(int error_code, char* buf, size_t size) {
    if (!buf || size == 0) return -1;
    
#ifdef VOX_OS_WINDOWS
    DWORD result = FormatMessageA(
        FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        NULL, error_code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        buf, (DWORD)size, NULL);
    if (result == 0) {
        snprintf(buf, size, "Error code: %d", error_code);
        return (int)strlen(buf);
    }
    return (int)result;
#else
    const char* str = strerror(error_code);
    if (!str) {
        snprintf(buf, size, "Error code: %d", error_code);
        return (int)strlen(buf);
    }
    strncpy(buf, str, size - 1);
    buf[size - 1] = '\0';
    return (int)strlen(buf);
#endif
}
