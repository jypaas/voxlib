/*
 * vox_select.c - select backend 实现（跨平台兜底方案）
 */

#include "vox_select.h"
#include "vox_os.h"
#include "vox_backend.h"
#include "vox_mpool.h"
#include "vox_htable.h"
#include "vox_socket.h"
#include "vox_log.h"
#include <string.h>
#include <stdlib.h>

#ifdef VOX_OS_WINDOWS
    /* Windows 上 FD_SETSIZE 默认为 64，需要在使用前定义更大的值 */
    #ifndef FD_SETSIZE
        #define FD_SETSIZE 1024
    #elif FD_SETSIZE < 1024
        #undef FD_SETSIZE
        #define FD_SETSIZE 1024
    #endif
    #define close closesocket
    #define read(s, b, n) recv(s, b, n, 0)
    #define write(s, b, n) send(s, b, n, 0)
    #ifndef ssize_t
        typedef SSIZE_T ssize_t;
    #endif
#else
    #include <sys/select.h>
    #include <unistd.h>
    #include <fcntl.h>
    #include <sys/time.h>
    #include <sys/types.h>
    /* 默认最大文件描述符数（受 FD_SETSIZE 限制） */
    #ifndef FD_SETSIZE
        #define FD_SETSIZE 1024
    #endif
#endif

/* 文件描述符信息 */
typedef struct {
    int fd;
    uint32_t events;
    void* user_data;
} vox_select_fd_info_t;

/* select 结构 */
struct vox_select {
    int wakeup_fd[2];                /* 用于唤醒的 socket [0]=read, [1]=write */
    vox_socket_t wakeup_sock[2];     /* 唤醒 socket 结构（用于 vox_socket API） */
    int max_fd;                      /* 当前最大的文件描述符 */
    fd_set read_fds;                 /* 读文件描述符集合 */
    fd_set write_fds;                /* 写文件描述符集合 */
    fd_set error_fds;                /* 错误文件描述符集合 */
    vox_htable_t* fd_map;            /* fd -> fd_info 映射 */
    vox_mpool_t* mpool;              /* 内存池 */
    bool own_mpool;                  /* 是否拥有内存池 */
    bool initialized;                /* 是否已初始化 */
};

/* 事件处理上下文 */
typedef struct {
    vox_select_t* select_impl;
    int wakeup_fd_read;              /* 唤醒 socket 的读端 fd */
    vox_socket_t* wakeup_sock;       /* 唤醒 socket 结构（用于接收数据） */
    fd_set* read_fds;
    fd_set* write_fds;
    fd_set* error_fds;
    int nfds;
    vox_select_event_cb event_cb;
    int* event_count;
} vox_select_poll_ctx_t;

/* 查找最大 fd 的上下文结构 */
typedef struct {
    int max_fd;
} find_max_fd_ctx_t;

/* 查找最大 fd 的回调函数 */
static void find_max_fd_cb(const void* key, size_t key_len, void* value, void* user_data) {
    (void)key_len;
    (void)value;
    int current_fd = *(const int*)key;
    find_max_fd_ctx_t* ctx = (find_max_fd_ctx_t*)user_data;
    if (current_fd > ctx->max_fd) {
        ctx->max_fd = current_fd;
    }
}

/* 事件处理回调（用于 htable foreach） */
static void process_events_cb(const void* key, size_t key_len, void* value, void* user_data) {
    (void)key_len;
    int fd = *(const int*)key;
    vox_select_fd_info_t* info = (vox_select_fd_info_t*)value;
    vox_select_poll_ctx_t* ctx = (vox_select_poll_ctx_t*)user_data;
    
    if (fd < 0 || fd >= ctx->nfds) {
        return;
    }
    
    uint32_t triggered_events = 0;
    
    /* 检查读事件 */
    if (FD_ISSET(fd, ctx->read_fds)) {
        triggered_events |= VOX_BACKEND_READ;
        
        /* 如果是唤醒 socket/pipe，清空数据 */
        if (fd == ctx->wakeup_fd_read) {
            char buf[256];
#ifdef VOX_OS_WINDOWS
            /* Windows 使用 vox_socket_recv */
            while (vox_socket_recv(ctx->wakeup_sock, buf, sizeof(buf)) > 0) {
                /* 清空唤醒数据 */
            }
#else
            /* Unix 使用 read（pipe 更快） */
            while (read(fd, buf, sizeof(buf)) > 0) {
                /* 清空唤醒数据 */
            }
#endif
            return;  /* 不回调唤醒 socket/pipe */
        }
    }
    
    /* 检查写事件 */
    if (FD_ISSET(fd, ctx->write_fds)) {
        triggered_events |= VOX_BACKEND_WRITE;
    }
    
    /* 检查错误事件 */
    if (FD_ISSET(fd, ctx->error_fds)) {
        triggered_events |= VOX_BACKEND_ERROR;
    }
    
    /* 如果有事件，调用回调 */
    if (triggered_events != 0) {
        ctx->event_cb(ctx->select_impl, fd, triggered_events, info->user_data);
        (*ctx->event_count)++;
    }
}

/* 创建 select backend */
vox_select_t* vox_select_create(const vox_select_config_t* config) {
    vox_mpool_t* mpool = config ? config->mpool : NULL;
    bool own_mpool = false;
    
    /* 如果没有提供内存池，创建默认的 */
    if (!mpool) {
        mpool = vox_mpool_create();
        if (!mpool) {
            return NULL;
        }
        own_mpool = true;
    }
    
    /* 从内存池分配 select 结构 */
    vox_select_t* select = (vox_select_t*)vox_mpool_alloc(mpool, sizeof(vox_select_t));
    if (!select) {
        if (own_mpool) {
            vox_mpool_destroy(mpool);
        }
        return NULL;
    }
    
    memset(select, 0, sizeof(vox_select_t));
    select->wakeup_fd[0] = -1;
    select->wakeup_fd[1] = -1;
    select->max_fd = -1;
    select->mpool = mpool;
    select->own_mpool = own_mpool;
    
    /* 初始化文件描述符集合 */
    FD_ZERO(&select->read_fds);
    FD_ZERO(&select->write_fds);
    FD_ZERO(&select->error_fds);
    
    /* 创建 fd 映射表 */
    select->fd_map = vox_htable_create(select->mpool);
    if (!select->fd_map) {
        vox_mpool_free(select->mpool, select);
        if (own_mpool) {
            vox_mpool_destroy(mpool);
        }
        return NULL;
    }
    
    return select;
}

/* 初始化 select */
int vox_select_init(vox_select_t* select) {
    if (!select) {
        return -1;
    }
    
    if (select->initialized) {
        return -1;
    }
    
    /* 创建唤醒管道（性能优化：Unix 使用 pipe，Windows 使用 socket） */
#ifdef VOX_OS_WINDOWS
    /* Windows 没有 pipe，使用 TCP socket 对（通过 vox_socket 接口） */
    vox_socket_t listener_sock, client_sock, server_sock;
    vox_socket_addr_t listen_addr;
    
    /* 初始化 socket 库 */
    if (vox_socket_init() != 0) {
        return -1;
    }
    
    /* 创建监听 socket */
    if (vox_socket_create(&listener_sock, VOX_SOCKET_TCP, VOX_AF_INET) != 0) {
        return -1;
    }
    
    /* 设置地址重用 */
    if (vox_socket_set_reuseaddr(&listener_sock, true) != 0) {
        vox_socket_destroy(&listener_sock);
        return -1;
    }
    
    /* 绑定到本地回环地址，端口为0（自动分配） */
    if (vox_socket_parse_address("127.0.0.1", 0, &listen_addr) != 0) {
        vox_socket_destroy(&listener_sock);
        return -1;
    }
    
    if (vox_socket_bind(&listener_sock, &listen_addr) != 0) {
        vox_socket_destroy(&listener_sock);
        return -1;
    }
    
    if (vox_socket_listen(&listener_sock, 1) != 0) {
        vox_socket_destroy(&listener_sock);
        return -1;
    }
    
    /* 获取实际绑定的地址 */
    if (vox_socket_get_local_addr(&listener_sock, &listen_addr) != 0) {
        vox_socket_destroy(&listener_sock);
        return -1;
    }
    
    /* 创建客户端 socket 并连接 */
    if (vox_socket_create(&client_sock, VOX_SOCKET_TCP, VOX_AF_INET) != 0) {
        vox_socket_destroy(&listener_sock);
        return -1;
    }
    
    if (vox_socket_connect(&client_sock, &listen_addr) != 0) {
        vox_socket_destroy(&client_sock);
        vox_socket_destroy(&listener_sock);
        return -1;
    }
    
    /* 接受连接 */
    if (vox_socket_accept(&listener_sock, &server_sock, NULL) != 0) {
        vox_socket_destroy(&client_sock);
        vox_socket_destroy(&listener_sock);
        return -1;
    }
    
    /* 关闭监听 socket */
    vox_socket_destroy(&listener_sock);
    
    /* 设置非阻塞 */
    if (vox_socket_set_nonblock(&client_sock, true) != 0 ||
        vox_socket_set_nonblock(&server_sock, true) != 0) {
        vox_socket_destroy(&client_sock);
        vox_socket_destroy(&server_sock);
        return -1;
    }
    
    /* 保存 socket 结构和 fd */
    select->wakeup_sock[0] = client_sock;
    select->wakeup_sock[1] = server_sock;
    select->wakeup_fd[0] = (int)client_sock.fd;
    select->wakeup_fd[1] = (int)server_sock.fd;
    
    /* 注意：这里不调用 vox_socket_destroy，因为我们需要保留 fd */
    /* fd 会在 vox_select_destroy 中关闭 */
#else
    /* Unix 使用 pipe()，性能更好（单次系统调用，内核级，延迟更低） */
    if (pipe(select->wakeup_fd) < 0) {
        return -1;
    }
    
    /* 设置非阻塞和 close-on-exec */
    int flags;
    flags = fcntl(select->wakeup_fd[0], F_GETFD);
    if (flags >= 0) {
        fcntl(select->wakeup_fd[0], F_SETFD, flags | FD_CLOEXEC);
    }
    flags = fcntl(select->wakeup_fd[1], F_GETFD);
    if (flags >= 0) {
        fcntl(select->wakeup_fd[1], F_SETFD, flags | FD_CLOEXEC);
    }
    flags = fcntl(select->wakeup_fd[0], F_GETFL);
    if (flags >= 0) {
        fcntl(select->wakeup_fd[0], F_SETFL, flags | O_NONBLOCK);
    }
    flags = fcntl(select->wakeup_fd[1], F_GETFL);
    if (flags >= 0) {
        fcntl(select->wakeup_fd[1], F_SETFL, flags | O_NONBLOCK);
    }
    
    /* 对于 pipe，不需要保存 socket 结构，直接使用 fd */
    /* 但为了统一接口，我们仍然需要初始化 wakeup_sock（虽然不使用） */
    memset(&select->wakeup_sock[0], 0, sizeof(vox_socket_t));
    memset(&select->wakeup_sock[1], 0, sizeof(vox_socket_t));
    select->wakeup_sock[0].fd = (vox_socket_fd_t)select->wakeup_fd[0];
    select->wakeup_sock[1].fd = (vox_socket_fd_t)select->wakeup_fd[1];
#endif
    
    /* 将唤醒管道的读端添加到 select */
    vox_select_fd_info_t* wakeup_info = (vox_select_fd_info_t*)vox_mpool_alloc(
        select->mpool, sizeof(vox_select_fd_info_t));
    if (!wakeup_info) {
        VOX_LOG_ERROR("Failed to allocate wakeup_info");
#ifdef VOX_OS_WINDOWS
        vox_socket_destroy(&select->wakeup_sock[0]);
        vox_socket_destroy(&select->wakeup_sock[1]);
#else
        close(select->wakeup_fd[0]);
        close(select->wakeup_fd[1]);
#endif
        select->wakeup_fd[0] = -1;
        select->wakeup_fd[1] = -1;
        return -1;
    }
    
    wakeup_info->fd = select->wakeup_fd[0];
    wakeup_info->events = VOX_BACKEND_READ;
    wakeup_info->user_data = NULL;
    
    /* 添加到映射表 */
    int key = select->wakeup_fd[0];
    vox_htable_set(select->fd_map, &key, sizeof(key), wakeup_info);
    
    /* 添加到读集合 */
    FD_SET(select->wakeup_fd[0], &select->read_fds);
    if (select->wakeup_fd[0] > select->max_fd) {
        select->max_fd = select->wakeup_fd[0];
    }
    
    select->initialized = true;
    return 0;
}

/* 销毁 select */
void vox_select_destroy(vox_select_t* select) {
    if (!select) {
        return;
    }
    
    vox_mpool_t* mpool = select->mpool;
    
    /* 关闭唤醒 socket/pipe */
#ifdef VOX_OS_WINDOWS
    /* Windows 使用 vox_socket_destroy */
    if (select->wakeup_fd[0] >= 0) {
        vox_socket_destroy(&select->wakeup_sock[0]);
    }
    if (select->wakeup_fd[1] >= 0) {
        vox_socket_destroy(&select->wakeup_sock[1]);
    }
#else
    /* Unix 使用 close（pipe） */
    if (select->wakeup_fd[0] >= 0) {
        close(select->wakeup_fd[0]);
    }
    if (select->wakeup_fd[1] >= 0) {
        close(select->wakeup_fd[1]);
    }
#endif
    
    /* 销毁映射表 */
    if (select->fd_map) {
        vox_htable_destroy(select->fd_map);
    }
    
    /* 释放内存 */
    if (select->own_mpool) {
        vox_mpool_destroy(mpool);
    } else {
        vox_mpool_free(mpool, select);
    }
}

/* 添加文件描述符 */
int vox_select_add(vox_select_t* select, int fd, uint32_t events, void* user_data) {
    if (!select) {
        return -1;
    }
    
    if (!select->initialized) {
        return -1;
    }
    
    if (fd < 0) {
        return -1;
    }
    
    /* 检查是否已存在 */
    int key = fd;
    vox_select_fd_info_t* info = (vox_select_fd_info_t*)vox_htable_get(select->fd_map, &key, sizeof(key));
    if (info) {
        /* 已存在，修改事件 */
        return vox_select_modify(select, fd, events);
    }
    
    /* 检查 FD_SETSIZE 限制 */
    if (fd >= FD_SETSIZE) {
        VOX_LOG_ERROR("fd (%d) >= FD_SETSIZE (%d)", fd, FD_SETSIZE);
        return -1;
    }
    
    /* 创建新的 fd 信息 */
    info = (vox_select_fd_info_t*)vox_mpool_alloc(select->mpool, sizeof(vox_select_fd_info_t));
    if (!info) {
        return -1;
    }
    
    info->fd = fd;
    info->events = events;
    info->user_data = user_data;
    
    /* 添加到映射表 */
    vox_htable_set(select->fd_map, &key, sizeof(key), info);
    
    /* 添加到相应的文件描述符集合 */
    if (events & VOX_BACKEND_READ) {
        FD_SET(fd, &select->read_fds);
    }
    if (events & VOX_BACKEND_WRITE) {
        FD_SET(fd, &select->write_fds);
    }
    if (events & (VOX_BACKEND_ERROR | VOX_BACKEND_HANGUP)) {
        FD_SET(fd, &select->error_fds);
    }
    
    /* 更新最大文件描述符 */
    if (fd > select->max_fd) {
        select->max_fd = fd;
    }
    
    return 0;
}

/* 修改文件描述符 */
int vox_select_modify(vox_select_t* select, int fd, uint32_t events) {
    if (!select || !select->initialized || fd < 0) {
        return -1;
    }
    
    int key = fd;
    vox_select_fd_info_t* info = (vox_select_fd_info_t*)vox_htable_get(select->fd_map, &key, sizeof(key));
    if (!info) {
        return -1;
    }
    
    /* 从所有集合中移除 */
    FD_CLR(fd, &select->read_fds);
    FD_CLR(fd, &select->write_fds);
    FD_CLR(fd, &select->error_fds);
    
    /* 更新事件 */
    info->events = events;
    
    /* 添加到相应的文件描述符集合 */
    if (events & VOX_BACKEND_READ) {
        FD_SET(fd, &select->read_fds);
    }
    if (events & VOX_BACKEND_WRITE) {
        FD_SET(fd, &select->write_fds);
    }
    if (events & (VOX_BACKEND_ERROR | VOX_BACKEND_HANGUP)) {
        FD_SET(fd, &select->error_fds);
    }
    
    return 0;
}

/* 移除文件描述符 */
int vox_select_remove(vox_select_t* select, int fd) {
    if (!select || !select->initialized || fd < 0) {
        return -1;
    }
    
    int key = fd;
    vox_select_fd_info_t* info = (vox_select_fd_info_t*)vox_htable_get(select->fd_map, &key, sizeof(key));
    if (!info) {
        return -1;
    }
    
    /* 从所有集合中移除 */
    FD_CLR(fd, &select->read_fds);
    FD_CLR(fd, &select->write_fds);
    FD_CLR(fd, &select->error_fds);
    
    /* 从映射表中移除 */
    vox_htable_delete(select->fd_map, &key, sizeof(key));
    
    /* 更新最大文件描述符（需要重新计算） */
    if (fd == select->max_fd) {
        select->max_fd = -1;
        /* 遍历映射表找到新的最大 fd */
        find_max_fd_ctx_t ctx = { .max_fd = -1 };
        
        vox_htable_foreach(select->fd_map, find_max_fd_cb, &ctx);
        
        select->max_fd = ctx.max_fd;
    }
    
    return 0;
}

/* 等待 IO 事件 */
int vox_select_poll(vox_select_t* select_impl, int timeout_ms, vox_select_event_cb event_cb) {
    if (!select_impl || !select_impl->initialized || !event_cb) {
        return -1;
    }
    
    /* 准备文件描述符集合（需要复制，因为 select 会修改它们） */
    fd_set read_fds = select_impl->read_fds;
    fd_set write_fds = select_impl->write_fds;
    fd_set error_fds = select_impl->error_fds;
    
    /* 准备超时结构 */
    struct timeval timeout;
    struct timeval* timeout_ptr = NULL;
    
    if (timeout_ms >= 0) {
        timeout.tv_sec = timeout_ms / 1000;
        timeout.tv_usec = (timeout_ms % 1000) * 1000;
        timeout_ptr = &timeout;
    }
    
    /* 调用 select 系统调用 */
    int nfds = select_impl->max_fd + 1;
    int result = select(nfds, &read_fds, &write_fds, &error_fds, timeout_ptr);
    
    if (result < 0) {
#ifdef VOX_OS_WINDOWS
        int err = WSAGetLastError();
        if (err == WSAEINTR) {
            /* 被中断，不算错误 */
            return 0;
        }
#else
        if (errno == EINTR) {
            /* 被信号中断，不算错误 */
            return 0;
        }
#endif
        return -1;
    }
    
    if (result == 0) {
        /* 超时 */
        return 0;
    }
    
    /* 处理事件 */
    int event_count = 0;
    
    /* 准备上下文 */
    vox_select_poll_ctx_t ctx = {
        .select_impl = select_impl,
        .wakeup_fd_read = select_impl->wakeup_fd[0],
        .wakeup_sock = &select_impl->wakeup_sock[0],
        .read_fds = &read_fds,
        .write_fds = &write_fds,
        .error_fds = &error_fds,
        .nfds = nfds,
        .event_cb = event_cb,
        .event_count = &event_count
    };
    
    /* 遍历所有文件描述符，检查是否有事件 */
    vox_htable_foreach(select_impl->fd_map, process_events_cb, &ctx);
    
    return event_count;
}

/* 唤醒 select */
int vox_select_wakeup(vox_select_t* select) {
    if (!select || !select->initialized || select->wakeup_fd[1] < 0) {
        return -1;
    }
    
#ifdef VOX_OS_WINDOWS
    /* Windows 使用 vox_socket_send */
    char byte = 1;
    int64_t n = vox_socket_send(&select->wakeup_sock[1], &byte, 1);
    return (n == 1) ? 0 : -1;
#else
    /* Unix 使用 write（pipe 更快） */
    char byte = 1;
    ssize_t n = write(select->wakeup_fd[1], &byte, 1);
    return (n == 1) ? 0 : -1;
#endif
}
