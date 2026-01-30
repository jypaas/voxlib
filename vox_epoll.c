/*
 * vox_epoll.c - Linux epoll backend 实现
 */

#ifdef VOX_OS_LINUX

#include "vox_epoll.h"
#include "vox_backend.h"
#include "vox_mpool.h"
#include "vox_htable.h"
#include "vox_log.h"
#include <sys/epoll.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>

/* 默认最大事件数 - 优化为4096以支持高并发场景 */
#define VOX_EPOLL_DEFAULT_MAX_EVENTS 4096

/* 文件描述符信息 */
typedef struct {
    int fd;
    uint32_t events;
    void* user_data;
} vox_epoll_fd_info_t;

/* epoll 结构 */
struct vox_epoll {
    int epoll_fd;                    /* epoll 文件描述符 */
    int wakeup_fd[2];                /* 用于唤醒的管道 [0]=read, [1]=write */
    size_t max_events;               /* 每次 epoll_wait 的最大事件数 */
    struct epoll_event* events;      /* 事件数组 */
    vox_htable_t* fd_map;            /* fd -> fd_info 映射 */
    vox_mpool_t* mpool;              /* 内存池 */
    bool own_mpool;                  /* 是否拥有内存池 */
    bool initialized;                /* 是否已初始化 */
};

/* 创建 epoll backend */
vox_epoll_t* vox_epoll_create(const vox_epoll_config_t* config) {
    vox_mpool_t* mpool = config ? config->mpool : NULL;
    bool own_mpool = false;
    
    /* 如果没有提供内存池，创建默认的 */
    if (!mpool) {
        mpool = vox_mpool_create();
        if (!mpool) {
            VOX_LOG_ERROR("Failed to create memory pool for epoll");
            return NULL;
        }
        own_mpool = true;
    }
    
    /* 从内存池分配 epoll 结构 */
    vox_epoll_t* epoll = (vox_epoll_t*)vox_mpool_alloc(mpool, sizeof(vox_epoll_t));
    if (!epoll) {
        VOX_LOG_ERROR("Failed to allocate epoll structure");
        if (own_mpool) {
            vox_mpool_destroy(mpool);
        }
        return NULL;
    }
    
    memset(epoll, 0, sizeof(vox_epoll_t));
    epoll->epoll_fd = -1;
    epoll->wakeup_fd[0] = -1;
    epoll->wakeup_fd[1] = -1;
    epoll->max_events = VOX_EPOLL_DEFAULT_MAX_EVENTS;
    epoll->mpool = mpool;
    epoll->own_mpool = own_mpool;
    
    /* 应用配置 */
    if (config && config->max_events > 0) {
        epoll->max_events = config->max_events;
    }
    
    /* 创建 fd 映射表 */
    epoll->fd_map = vox_htable_create(epoll->mpool);
    if (!epoll->fd_map) {
        VOX_LOG_ERROR("Failed to create fd map for epoll");
        vox_mpool_free(epoll->mpool, epoll);
        if (own_mpool) {
            vox_mpool_destroy(mpool);
        }
        return NULL;
    }
    
    /* 分配事件数组 */
    epoll->events = (struct epoll_event*)vox_mpool_alloc(
        epoll->mpool, 
        epoll->max_events * sizeof(struct epoll_event)
    );
    if (!epoll->events) {
        VOX_LOG_ERROR("Failed to allocate events array for epoll");
        vox_htable_destroy(epoll->fd_map);
        vox_mpool_free(epoll->mpool, epoll);
        if (own_mpool) {
            vox_mpool_destroy(mpool);
        }
        return NULL;
    }
    
    return epoll;
}

/* 初始化 epoll */
int vox_epoll_init(vox_epoll_t* epoll) {
    if (!epoll || epoll->initialized) {
        VOX_LOG_ERROR("Invalid epoll or already initialized");
        return -1;
    }
    
    /* 创建 epoll 实例 */
    epoll->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (epoll->epoll_fd < 0) {
        VOX_LOG_ERROR("Failed to create epoll instance: errno=%d", errno);
        return -1;
    }
    
    /* 创建唤醒管道 */
    /* 使用 pipe + fcntl 以确保兼容性（pipe2 需要 _GNU_SOURCE） */
    if (pipe(epoll->wakeup_fd) < 0) {
        VOX_LOG_ERROR("Failed to create wakeup pipe: errno=%d", errno);
        close(epoll->epoll_fd);
        epoll->epoll_fd = -1;
        return -1;
    }
    /* 设置非阻塞和 close-on-exec */
    int flags;
    flags = fcntl(epoll->wakeup_fd[0], F_GETFD);
    if (flags >= 0) {
        fcntl(epoll->wakeup_fd[0], F_SETFD, flags | FD_CLOEXEC);
    }
    flags = fcntl(epoll->wakeup_fd[1], F_GETFD);
    if (flags >= 0) {
        fcntl(epoll->wakeup_fd[1], F_SETFD, flags | FD_CLOEXEC);
    }
    flags = fcntl(epoll->wakeup_fd[0], F_GETFL);
    if (flags >= 0) {
        fcntl(epoll->wakeup_fd[0], F_SETFL, flags | O_NONBLOCK);
    }
    flags = fcntl(epoll->wakeup_fd[1], F_GETFL);
    if (flags >= 0) {
        fcntl(epoll->wakeup_fd[1], F_SETFL, flags | O_NONBLOCK);
    }
    
    /* 将唤醒管道的读端添加到 epoll */
    /* 为唤醒管道创建特殊标记的 info */
    vox_epoll_fd_info_t* wakeup_info = (vox_epoll_fd_info_t*)vox_mpool_alloc(
        epoll->mpool, sizeof(vox_epoll_fd_info_t));
    if (!wakeup_info) {
        close(epoll->wakeup_fd[0]);
        close(epoll->wakeup_fd[1]);
        close(epoll->epoll_fd);
        epoll->wakeup_fd[0] = -1;
        epoll->wakeup_fd[1] = -1;
        epoll->epoll_fd = -1;
        return -1;
    }
    wakeup_info->fd = epoll->wakeup_fd[0];
    wakeup_info->events = 0;
    wakeup_info->user_data = NULL;
    
    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.ptr = wakeup_info;  /* 使用 ptr 存储指针 */
    if (epoll_ctl(epoll->epoll_fd, EPOLL_CTL_ADD, epoll->wakeup_fd[0], &ev) < 0) {
        VOX_LOG_ERROR("Failed to add wakeup pipe to epoll: errno=%d", errno);
        close(epoll->wakeup_fd[0]);
        close(epoll->wakeup_fd[1]);
        close(epoll->epoll_fd);
        epoll->epoll_fd = -1;
        epoll->wakeup_fd[0] = -1;
        epoll->wakeup_fd[1] = -1;
        return -1;
    }
    
    epoll->initialized = true;
    return 0;
}

/* 销毁 epoll */
void vox_epoll_destroy(vox_epoll_t* epoll) {
    if (!epoll) {
        return;
    }
    
    if (epoll->epoll_fd >= 0) {
        close(epoll->epoll_fd);
    }
    
    if (epoll->wakeup_fd[0] >= 0) {
        close(epoll->wakeup_fd[0]);
    }
    
    if (epoll->wakeup_fd[1] >= 0) {
        close(epoll->wakeup_fd[1]);
    }
    
    vox_mpool_t* mpool = epoll->mpool;
    bool own_mpool = epoll->own_mpool;
    
    if (epoll->fd_map) {
        vox_htable_destroy(epoll->fd_map);
    }
    
    /* 从内存池释放 epoll 结构 */
    if (mpool) {
        vox_mpool_free(mpool, epoll);
        if (own_mpool) {
            vox_mpool_destroy(mpool);
        }
    }
}

/* 将 backend 事件转换为 epoll 事件 */
static uint32_t backend_to_epoll_events(uint32_t events) {
    uint32_t epoll_events = 0;
    
    if (events & VOX_BACKEND_READ) {
        epoll_events |= EPOLLIN;
#ifdef EPOLLRDHUP
        epoll_events |= EPOLLRDHUP;
#endif
    }
    if (events & VOX_BACKEND_WRITE) {
        epoll_events |= EPOLLOUT;
    }
    if (events & VOX_BACKEND_ERROR) {
        epoll_events |= EPOLLERR;
    }
    if (events & VOX_BACKEND_HANGUP) {
        epoll_events |= EPOLLHUP;
#ifdef EPOLLRDHUP
        epoll_events |= EPOLLRDHUP;
#endif
    }
    
    return epoll_events;
}

/* 将 epoll 事件转换为 backend 事件 */
static uint32_t epoll_to_backend_events(uint32_t epoll_events) {
    uint32_t events = 0;
    
    if (epoll_events & EPOLLIN) {
        events |= VOX_BACKEND_READ;
    }
    if (epoll_events & EPOLLOUT) {
        events |= VOX_BACKEND_WRITE;
    }
    if (epoll_events & EPOLLERR) {
        events |= VOX_BACKEND_ERROR;
    }
    if (epoll_events & EPOLLHUP) {
        events |= VOX_BACKEND_HANGUP;
    }
#ifdef EPOLLRDHUP
    if (epoll_events & EPOLLRDHUP) {
        events |= VOX_BACKEND_HANGUP;
    }
#endif
    
    return events;
}

/* 添加文件描述符 */
int vox_epoll_add(vox_epoll_t* epoll, int fd, uint32_t events, void* user_data) {
    if (!epoll || !epoll->initialized || fd < 0) {
        return -1;
    }
    
    /* 创建 fd 信息 */
    vox_epoll_fd_info_t* info = (vox_epoll_fd_info_t*)vox_mpool_alloc(
        epoll->mpool, 
        sizeof(vox_epoll_fd_info_t)
    );
    if (!info) {
        VOX_LOG_ERROR("Failed to allocate fd info for epoll");
        return -1;
    }
    
    info->fd = fd;
    info->events = events;
    info->user_data = user_data;
    
    /* 添加到 epoll - 使用 data.ptr 直接存储 info 指针，避免哈希表查找 */
    struct epoll_event ev;
    ev.events = backend_to_epoll_events(events);
    ev.data.ptr = info;  /* 直接存储指针，优化性能 */
    
    /* 直接尝试添加，如果已存在 epoll_ctl 会返回 EEXIST */
    if (epoll_ctl(epoll->epoll_fd, EPOLL_CTL_ADD, fd, &ev) < 0) {
        if (errno == EEXIST) {
            /* 已存在，释放 info 并返回错误 */
            vox_mpool_free(epoll->mpool, info);
            return -1;
        }
        /* 其他错误，释放 info */
        VOX_LOG_ERROR("Failed to add fd %d to epoll: errno=%d", fd, errno);
        vox_mpool_free(epoll->mpool, info);
        return -1;
    }
    
    /* 添加到映射表（用于 modify/remove 操作） */
    int key = fd;
    if (vox_htable_set(epoll->fd_map, &key, sizeof(key), info) != 0) {
        /* 哈希表添加失败，回滚 epoll 添加 */
        VOX_LOG_ERROR("Failed to add fd %d to epoll fd map", fd);
        epoll_ctl(epoll->epoll_fd, EPOLL_CTL_DEL, fd, NULL);
        vox_mpool_free(epoll->mpool, info);
        return -1;
    }
    
    return 0;
}

/* 修改文件描述符 */
int vox_epoll_modify(vox_epoll_t* epoll, int fd, uint32_t events) {
    if (!epoll || !epoll->initialized || fd < 0) {
        return -1;
    }
    
    /* 查找 fd 信息 */
    int key = fd;
    vox_epoll_fd_info_t* info = (vox_epoll_fd_info_t*)vox_htable_get(
        epoll->fd_map, 
        &key, 
        sizeof(key)
    );
    if (!info) {
        return -1;  /* 不存在 */
    }
    
    /* 更新事件 */
    info->events = events;
    
    /* 更新 epoll - 使用 data.ptr 直接存储 info 指针 */
    struct epoll_event ev;
    ev.events = backend_to_epoll_events(events);
    ev.data.ptr = info;  /* 直接存储指针，优化性能 */
    
    if (epoll_ctl(epoll->epoll_fd, EPOLL_CTL_MOD, fd, &ev) < 0) {
        VOX_LOG_ERROR("Failed to modify fd %d in epoll: errno=%d", fd, errno);
        return -1;
    }
    
    return 0;
}

/* 移除文件描述符 */
int vox_epoll_remove(vox_epoll_t* epoll, int fd) {
    if (!epoll || !epoll->initialized || fd < 0) {
        return -1;
    }
    
    /* 1. 从 epoll 移除。忽略常见错误（如 FD 已关闭导致 DEL 失败），以防止内存泄漏 */
    if (epoll_ctl(epoll->epoll_fd, EPOLL_CTL_DEL, fd, NULL) < 0) {
        /* 仅忽略不影响清理的错误：
         * ENOENT: 已经不在监控中
         * EBADF: FD 已关闭
         * EPERM: FD 不支持 epoll (如普通文件) */
        if (errno != ENOENT && errno != EBADF && errno != EPERM) {
            /* 其他情况可能需要关注，但我们仍然需要清理内部内存 */
        }
    }
    
    /* 2. 无论 epoll_ctl 结果如何，都尝试从映射表移除并释放 info 内存 */
    int key = fd;
    vox_epoll_fd_info_t* info = (vox_epoll_fd_info_t*)vox_htable_get(
        epoll->fd_map, 
        &key, 
        sizeof(key)
    );
    if (info) {
        vox_htable_delete(epoll->fd_map, &key, sizeof(key));
        vox_mpool_free(epoll->mpool, info);
    }
    
    return 0;
}

/* 等待 IO 事件 */
int vox_epoll_poll(vox_epoll_t* epoll, int timeout_ms, vox_epoll_event_cb event_cb) {
    if (!epoll || !epoll->initialized || !event_cb) {
        return -1;
    }
    
    /* 转换超时时间 */
    int timeout = timeout_ms;
    if (timeout_ms < 0) {
        timeout = -1;  /* 无限等待 */
    }
    
    /* 等待事件 */
    int nfds = epoll_wait(epoll->epoll_fd, epoll->events, (int)epoll->max_events, timeout);
    if (nfds < 0) {
        if (errno == EINTR) {
            return 0;  /* 被信号中断，不算错误 */
        }
        VOX_LOG_ERROR("epoll_wait failed: errno=%d", errno);
        return -1;
    }
    
    /* 处理事件 - 直接从 data.ptr 获取 info，避免哈希表查找 */
    int processed = 0;
    for (int i = 0; i < nfds; i++) {
        vox_epoll_fd_info_t* info = (vox_epoll_fd_info_t*)epoll->events[i].data.ptr;
        uint32_t epoll_events = epoll->events[i].events;
        
        if (!info) {
            continue;  /* 无效指针，跳过 */
        }
        
        int fd = info->fd;
        
        /* 检查是否是唤醒管道 */
        if (fd == epoll->wakeup_fd[0]) {
            /* 读取唤醒字节 */
            char buf[256];
            while (read(epoll->wakeup_fd[0], buf, sizeof(buf)) > 0) {
                /* 继续读取直到清空 */
            }
            continue;
        }
        
        /* 直接使用 info，无需哈希表查找 */
        uint32_t events = epoll_to_backend_events(epoll_events);
        event_cb(epoll, fd, events, info->user_data);
        processed++;
    }
    
    return processed;
}

/* 唤醒 epoll */
int vox_epoll_wakeup(vox_epoll_t* epoll) {
    if (!epoll || !epoll->initialized) {
        return -1;
    }
    
    /* 写入一个字节到唤醒管道 */
    char byte = 1;
    if (write(epoll->wakeup_fd[1], &byte, 1) < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return 0;  /* 管道已满，说明已经唤醒 */
        }
        VOX_LOG_ERROR("Failed to write to wakeup pipe: errno=%d", errno);
        return -1;
    }
    
    return 0;
}

#endif /* VOX_OS_LINUX */
