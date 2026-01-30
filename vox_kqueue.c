/*
 * vox_kqueue.c - macOS/BSD kqueue backend 实现
 */

#if defined(VOX_OS_MACOS) || defined(VOX_OS_BSD)

#include "vox_kqueue.h"
#include "vox_backend.h"
#include "vox_mpool.h"
#include "vox_htable.h"
#include "vox_log.h"
#include <sys/event.h>
#include <sys/time.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>

/* 默认最大事件数 - 优化为4096以支持高并发场景 */
#define VOX_KQUEUE_DEFAULT_MAX_EVENTS 4096

/* 文件描述符信息 */
typedef struct {
    int fd;
    uint32_t events;
    void* user_data;
} vox_kqueue_fd_info_t;

/* kqueue 结构 */
struct vox_kqueue {
    int kqueue_fd;                    /* kqueue 文件描述符 */
    int wakeup_fd[2];                 /* 用于唤醒的管道 [0]=read, [1]=write */
    size_t max_events;                /* 每次 kevent 的最大事件数 */
    struct kevent* events;            /* 事件数组 */
    vox_htable_t* fd_map;             /* fd -> fd_info 映射 */
    vox_mpool_t* mpool;               /* 内存池 */
    bool own_mpool;                   /* 是否拥有内存池 */
    bool initialized;                 /* 是否已初始化 */
};

/* 创建 kqueue backend */
vox_kqueue_t* vox_kqueue_create(const vox_kqueue_config_t* config) {
    vox_mpool_t* mpool = config ? config->mpool : NULL;
    bool own_mpool = false;
    
    /* 如果没有提供内存池，创建默认的 */
    if (!mpool) {
        mpool = vox_mpool_create();
        if (!mpool) {
            VOX_LOG_ERROR("Failed to create memory pool for kqueue");
            return NULL;
        }
        own_mpool = true;
    }
    
    /* 从内存池分配 kqueue 结构 */
    vox_kqueue_t* kq = (vox_kqueue_t*)vox_mpool_alloc(mpool, sizeof(vox_kqueue_t));
    if (!kq) {
        VOX_LOG_ERROR("Failed to allocate kqueue structure");
        if (own_mpool) {
            vox_mpool_destroy(mpool);
        }
        return NULL;
    }
    
    memset(kq, 0, sizeof(vox_kqueue_t));
    kq->kqueue_fd = -1;
    kq->wakeup_fd[0] = -1;
    kq->wakeup_fd[1] = -1;
    kq->max_events = VOX_KQUEUE_DEFAULT_MAX_EVENTS;
    kq->mpool = mpool;
    kq->own_mpool = own_mpool;
    
    /* 应用配置 */
    if (config && config->max_events > 0) {
        kq->max_events = config->max_events;
    }
    
    /* 创建 fd 映射表 */
    kq->fd_map = vox_htable_create(kq->mpool);
    if (!kq->fd_map) {
        VOX_LOG_ERROR("Failed to create fd map for kqueue");
        vox_mpool_free(kq->mpool, kq);
        if (own_mpool) {
            vox_mpool_destroy(mpool);
        }
        return NULL;
    }
    
    /* 分配事件数组 */
    kq->events = (struct kevent*)vox_mpool_alloc(
        kq->mpool, 
        kq->max_events * sizeof(struct kevent)
    );
    if (!kq->events) {
        VOX_LOG_ERROR("Failed to allocate events array for kqueue");
        vox_htable_destroy(kq->fd_map);
        vox_mpool_free(kq->mpool, kq);
        if (own_mpool) {
            vox_mpool_destroy(mpool);
        }
        return NULL;
    }
    
    return kq;
}

/* 初始化 kqueue */
int vox_kqueue_init(vox_kqueue_t* kq) {
    if (!kq || kq->initialized) {
        VOX_LOG_ERROR("Invalid kqueue or already initialized");
        return -1;
    }
    
    /* 创建 kqueue 实例 */
    kq->kqueue_fd = kqueue();
    if (kq->kqueue_fd < 0) {
        VOX_LOG_ERROR("Failed to create kqueue instance: errno=%d", errno);
        return -1;
    }
    
    /* 设置 close-on-exec */
    fcntl(kq->kqueue_fd, F_SETFD, FD_CLOEXEC);
    
    /* 创建唤醒管道 */
    if (pipe(kq->wakeup_fd) < 0) {
        VOX_LOG_ERROR("Failed to create wakeup pipe: errno=%d", errno);
        close(kq->kqueue_fd);
        kq->kqueue_fd = -1;
        return -1;
    }
    
    /* 设置非阻塞 */
    fcntl(kq->wakeup_fd[0], F_SETFL, O_NONBLOCK);
    fcntl(kq->wakeup_fd[1], F_SETFL, O_NONBLOCK);
    
    /* 将唤醒管道的读端添加到 kqueue */
    /* 为唤醒管道创建特殊标记的 info */
    vox_kqueue_fd_info_t* wakeup_info = (vox_kqueue_fd_info_t*)vox_mpool_alloc(
        kq->mpool, sizeof(vox_kqueue_fd_info_t));
    if (!wakeup_info) {
        close(kq->wakeup_fd[0]);
        close(kq->wakeup_fd[1]);
        close(kq->kqueue_fd);
        kq->wakeup_fd[0] = -1;
        kq->wakeup_fd[1] = -1;
        kq->kqueue_fd = -1;
        return -1;
    }
    wakeup_info->fd = kq->wakeup_fd[0];
    wakeup_info->events = 0;
    wakeup_info->user_data = NULL;
    
    struct kevent ev;
    EV_SET(&ev, kq->wakeup_fd[0], EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, wakeup_info);
    if (kevent(kq->kqueue_fd, &ev, 1, NULL, 0, NULL) < 0) {
        VOX_LOG_ERROR("Failed to add wakeup pipe to kqueue: errno=%d", errno);
        close(kq->wakeup_fd[0]);
        close(kq->wakeup_fd[1]);
        close(kq->kqueue_fd);
        kq->kqueue_fd = -1;
        kq->wakeup_fd[0] = -1;
        kq->wakeup_fd[1] = -1;
        return -1;
    }
    
    kq->initialized = true;
    return 0;
}

/* 销毁 kqueue */
void vox_kqueue_destroy(vox_kqueue_t* kq) {
    if (!kq) {
        return;
    }
    
    vox_mpool_t* mpool = kq->mpool;
    bool own_mpool = kq->own_mpool;
    
    if (kq->kqueue_fd >= 0) {
        close(kq->kqueue_fd);
    }
    
    if (kq->wakeup_fd[0] >= 0) {
        close(kq->wakeup_fd[0]);
    }
    
    if (kq->wakeup_fd[1] >= 0) {
        close(kq->wakeup_fd[1]);
    }
    
    if (kq->fd_map) {
        vox_htable_destroy(kq->fd_map);
    }
    
    /* 从内存池释放 kqueue 结构 */
    if (mpool) {
        vox_mpool_free(mpool, kq);
        if (own_mpool) {
            vox_mpool_destroy(mpool);
        }
    }
}

/* 将 backend 事件转换为 kqueue 过滤器 */
static void backend_to_kqueue_filters(int fd, uint32_t events, void* udata, struct kevent* evs, int* count) {
    *count = 0;
    
    if (events & VOX_BACKEND_READ) {
        EV_SET(&evs[*count], fd, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, udata);
        (*count)++;
    }
    if (events & VOX_BACKEND_WRITE) {
        EV_SET(&evs[*count], fd, EVFILT_WRITE, EV_ADD | EV_ENABLE, 0, 0, udata);
        (*count)++;
    }
}

/* 将 kqueue 事件转换为 backend 事件 */
static uint32_t kqueue_to_backend_events(const struct kevent* ev) {
    uint32_t events = 0;
    
    if (ev->filter == EVFILT_READ) {
        events |= VOX_BACKEND_READ;
    }
    if (ev->filter == EVFILT_WRITE) {
        events |= VOX_BACKEND_WRITE;
    }
    if (ev->flags & EV_ERROR) {
        events |= VOX_BACKEND_ERROR;
    }
    if (ev->flags & EV_EOF) {
        events |= VOX_BACKEND_HANGUP;
    }
    
    return events;
}

/* 添加文件描述符 */
int vox_kqueue_add(vox_kqueue_t* kq, int fd, uint32_t events, void* user_data) {
    if (!kq || !kq->initialized || fd < 0) {
        return -1;
    }
    
    /* 创建 fd 信息 */
    vox_kqueue_fd_info_t* info = (vox_kqueue_fd_info_t*)vox_mpool_alloc(
        kq->mpool, 
        sizeof(vox_kqueue_fd_info_t)
    );
    if (!info) {
        VOX_LOG_ERROR("Failed to allocate fd info for kqueue");
        return -1;
    }
    
    info->fd = fd;
    info->events = events;
    info->user_data = user_data;
    
    /* 添加到 kqueue - 使用 udata 直接存储 info 指针，避免哈希表查找 */
    struct kevent evs[2];
    int ev_count = 0;
    backend_to_kqueue_filters(fd, events, info, evs, &ev_count);
    
    /* 直接尝试添加 */
    if (kevent(kq->kqueue_fd, evs, ev_count, NULL, 0, NULL) < 0) {
        VOX_LOG_ERROR("Failed to add fd %d to kqueue: errno=%d", fd, errno);
        vox_mpool_free(kq->mpool, info);
        return -1;
    }
    
    /* 添加到映射表（用于 modify/remove 操作） */
    int key = fd;
    if (vox_htable_set(kq->fd_map, &key, sizeof(key), info) != 0) {
        /* 哈希表添加失败，回滚 kqueue 添加 */
        VOX_LOG_ERROR("Failed to add fd %d to kqueue fd map", fd);
        struct kevent del_evs[2];
        EV_SET(&del_evs[0], fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
        EV_SET(&del_evs[1], fd, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
        kevent(kq->kqueue_fd, del_evs, 2, NULL, 0, NULL);
        vox_mpool_free(kq->mpool, info);
        return -1;
    }
    
    return 0;
}

/* 修改文件描述符 */
int vox_kqueue_modify(vox_kqueue_t* kq, int fd, uint32_t events) {
    if (!kq || !kq->initialized || fd < 0) {
        return -1;
    }
    
    /* 查找 fd 信息 - 仍然需要哈希表查找（因为需要更新） */
    int key = fd;
    vox_kqueue_fd_info_t* info = (vox_kqueue_fd_info_t*)vox_htable_get(
        kq->fd_map, 
        &key, 
        sizeof(key)
    );
    if (!info) {
        return -1;  /* 不存在 */
    }
    
    /* 更新事件前先删除旧的过滤器。
     * 注意：kqueue 是增量式的，所以我们需要显式删除不再需要的过滤器。
     * 简单起见，我们删除所有可能存在的过滤器，然后添加新的。 */
    struct kevent evs[2];
    EV_SET(&evs[0], fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
    EV_SET(&evs[1], fd, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
    /* 忽略删除失败，因为某些过滤器之前可能并未启用 */
    kevent(kq->kqueue_fd, evs, 2, NULL, 0, NULL);
    
    /* 更新信息中的事件 */
    info->events = events;
    
    /* 添加新的过滤器 */
    int ev_count = 0;
    backend_to_kqueue_filters(fd, events, info, evs, &ev_count);
    
    if (ev_count > 0 && kevent(kq->kqueue_fd, evs, ev_count, NULL, 0, NULL) < 0) {
        VOX_LOG_ERROR("Failed to modify fd %d in kqueue: errno=%d", fd, errno);
        return -1;
    }
    
    return 0;
}

/* 移除文件描述符 */
int vox_kqueue_remove(vox_kqueue_t* kq, int fd) {
    if (!kq || !kq->initialized || fd < 0) {
        return -1;
    }
    
    /* 从 kqueue 移除。对于 kqueue，显式删除所有可能的过滤器 */
    struct kevent evs[2];
    EV_SET(&evs[0], fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
    EV_SET(&evs[1], fd, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
    /* 即使失败也继续，可能 FD 已关闭导致自动移除 */
    kevent(kq->kqueue_fd, evs, 2, NULL, 0, NULL);
    
    /* 从映射表移除并释放内存 */
    int key = fd;
    vox_kqueue_fd_info_t* info = (vox_kqueue_fd_info_t*)vox_htable_get(
        kq->fd_map, 
        &key, 
        sizeof(key)
    );
    if (info) {
        vox_htable_delete(kq->fd_map, &key, sizeof(key));
        vox_mpool_free(kq->mpool, info);
    }
    
    return 0;
}

/* 等待 IO 事件 */
int vox_kqueue_poll(vox_kqueue_t* kq, int timeout_ms, vox_kqueue_event_cb event_cb) {
    if (!kq || !kq->initialized || !event_cb) {
        return -1;
    }
    
    /* 转换超时时间 */
    struct timespec timeout;
    struct timespec* timeout_ptr = NULL;
    
    if (timeout_ms >= 0) {
        timeout.tv_sec = timeout_ms / 1000;
        timeout.tv_nsec = (timeout_ms % 1000) * 1000000;
        timeout_ptr = &timeout;
    }
    
    /* 等待事件 */
    int nfds = kevent(kq->kqueue_fd, NULL, 0, kq->events, 
                      (int)kq->max_events, timeout_ptr);
    if (nfds < 0) {
        if (errno == EINTR) {
            return 0;  /* 被信号中断，不算错误 */
        }
        VOX_LOG_ERROR("kevent failed: errno=%d", errno);
        return -1;
    }
    
    /* 处理事件 - 直接从 udata 获取 info，避免哈希表查找 */
    int processed = 0;
    for (int i = 0; i < nfds; i++) {
        vox_kqueue_fd_info_t* info = (vox_kqueue_fd_info_t*)kq->events[i].udata;
        
        if (!info) {
            continue;  /* 无效指针，跳过 */
        }
        
        int fd = info->fd;
        
        /* 检查是否是唤醒管道 */
        if (fd == kq->wakeup_fd[0]) {
            /* 读取唤醒字节 */
            char buf[256];
            while (read(kq->wakeup_fd[0], buf, sizeof(buf)) > 0) {
                /* 继续读取直到清空 */
            }
            continue;
        }
        
        /* 转换事件并调用回调 */
        uint32_t events = kqueue_to_backend_events(&kq->events[i]);
        event_cb(kq, fd, events, info->user_data);
        processed++;
    }
    
    return processed;
}

/* 唤醒 kqueue */
int vox_kqueue_wakeup(vox_kqueue_t* kq) {
    if (!kq || !kq->initialized) {
        return -1;
    }
    
    /* 写入一个字节到唤醒管道 */
    char byte = 1;
    if (write(kq->wakeup_fd[1], &byte, 1) < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return 0;  /* 管道已满，说明已经唤醒 */
        }
        VOX_LOG_ERROR("Failed to write to wakeup pipe: errno=%d", errno);
        return -1;
    }
    
    return 0;
}

#endif /* VOX_OS_MACOS || VOX_OS_BSD */
