/*
 * vox_uring.c - Linux io_uring backend 实现（高性能优化版）
 *
 * 优化特性：
 * - 使用 multishot poll 避免每次事件后重新注册
 * - 批量提交 SQE 减少系统调用
 * - 使用 io_uring_submit_and_wait 合并提交和等待
 */

#ifdef VOX_OS_LINUX
#ifdef VOX_USE_IOURING

#include "vox_uring.h"
#include "vox_backend.h"
#include "vox_mpool.h"
#include "vox_htable.h"
#include "vox_log.h"
#include <liburing.h>
#include <poll.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>

/* 默认配置 */
#define VOX_URING_DEFAULT_MAX_EVENTS 4096
#define VOX_URING_DEFAULT_SQ_ENTRIES 4096

/* Multishot poll 标志（Linux 5.13+） */
#ifndef IORING_POLL_ADD_MULTI
#define IORING_POLL_ADD_MULTI (1U << 0)
#endif

/* 文件描述符信息 */
typedef struct {
    int fd;
    uint32_t events;
    void* user_data;
    bool multishot_active;  /* multishot poll 是否活跃 */
} vox_uring_fd_info_t;

/* io_uring 结构 */
struct vox_uring {
    struct io_uring ring;              /* io_uring 实例 */
    int wakeup_fd[2];                  /* 用于唤醒的管道 [0]=read, [1]=write */
    vox_uring_fd_info_t* wakeup_info;  /* 唤醒管道的信息 */
    size_t max_events;                 /* 每次处理的最大事件数 */
    vox_htable_t* fd_map;              /* fd -> fd_info 映射 */
    vox_mpool_t* mpool;                /* 内存池 */
    bool own_mpool;                    /* 是否拥有内存池 */
    bool initialized;                  /* 是否已初始化 */
    bool use_multishot;                /* 是否支持 multishot poll */
};

/* 注册 poll 操作（支持 multishot） */
static int uring_add_poll(vox_uring_t* uring, vox_uring_fd_info_t* info) {
    struct io_uring_sqe* sqe = io_uring_get_sqe(&uring->ring);
    if (!sqe) {
        VOX_LOG_ERROR("Failed to get SQE from io_uring ring");
        return -1;
    }

    uint32_t poll_mask = 0;
    if (info->events & VOX_BACKEND_READ) poll_mask |= POLLIN;
    if (info->events & VOX_BACKEND_WRITE) poll_mask |= POLLOUT;

    io_uring_prep_poll_add(sqe, info->fd, poll_mask);
    io_uring_sqe_set_data(sqe, info);

    /* 使用 multishot poll（如果支持） */
    if (uring->use_multishot) {
        sqe->len |= IORING_POLL_ADD_MULTI;
        info->multishot_active = true;
    }

    return 0;
}

/* 创建 io_uring backend */
vox_uring_t* vox_uring_create(const vox_uring_config_t* config) {
    vox_mpool_t* mpool = config ? config->mpool : NULL;
    bool own_mpool = false;

    if (!mpool) {
        mpool = vox_mpool_create();
        if (!mpool) {
            VOX_LOG_ERROR("Failed to create memory pool for io_uring");
            return NULL;
        }
        own_mpool = true;
    }

    vox_uring_t* uring = (vox_uring_t*)vox_mpool_alloc(mpool, sizeof(vox_uring_t));
    if (!uring) {
        VOX_LOG_ERROR("Failed to allocate io_uring structure");
        if (own_mpool) {
            vox_mpool_destroy(mpool);
        }
        return NULL;
    }

    memset(uring, 0, sizeof(vox_uring_t));
    uring->wakeup_fd[0] = -1;
    uring->wakeup_fd[1] = -1;
    uring->max_events = VOX_URING_DEFAULT_MAX_EVENTS;
    uring->mpool = mpool;
    uring->own_mpool = own_mpool;
    uring->use_multishot = true;  /* 默认尝试使用 multishot */

    if (config && config->max_events > 0) {
        uring->max_events = config->max_events;
    }

    vox_htable_config_t htable_config = {0};
    uring->fd_map = vox_htable_create_with_config(uring->mpool, &htable_config);
    if (!uring->fd_map) {
        VOX_LOG_ERROR("Failed to create fd map for io_uring");
        vox_mpool_free(uring->mpool, uring);
        if (own_mpool) {
            vox_mpool_destroy(mpool);
        }
        return NULL;
    }

    return uring;
}

/* 初始化 io_uring */
int vox_uring_init(vox_uring_t* uring) {
    if (!uring || uring->initialized) {
        VOX_LOG_ERROR("Invalid io_uring or already initialized");
        return -1;
    }

    /* 初始化 io_uring，使用 COOP_TASKRUN 减少内核开销 */
    struct io_uring_params params;
    memset(&params, 0, sizeof(params));

    /* COOP_TASKRUN: 减少内核中断，提高性能（Linux 5.19+） */
#ifdef IORING_SETUP_COOP_TASKRUN
    params.flags |= IORING_SETUP_COOP_TASKRUN;
#endif

    /* SINGLE_ISSUER: 单线程优化（Linux 6.0+） */
#ifdef IORING_SETUP_SINGLE_ISSUER
    params.flags |= IORING_SETUP_SINGLE_ISSUER;
#endif

    int ret = io_uring_queue_init_params(VOX_URING_DEFAULT_SQ_ENTRIES, &uring->ring, &params);
    if (ret < 0) {
        /* 回退到基本初始化 */
        ret = io_uring_queue_init(VOX_URING_DEFAULT_SQ_ENTRIES, &uring->ring, 0);
        if (ret < 0) {
            VOX_LOG_ERROR("Failed to initialize io_uring: ret=%d", ret);
            return -1;
        }
    }

    /* 检测 multishot poll 支持（通过 probe） */
    struct io_uring_probe* probe = io_uring_get_probe_ring(&uring->ring);
    if (probe) {
        /* 检查 POLL_ADD 是否支持（multishot 是 POLL_ADD 的扩展） */
        if (!io_uring_opcode_supported(probe, IORING_OP_POLL_ADD)) {
            uring->use_multishot = false;
        }
        io_uring_free_probe(probe);
    }

    /* 创建唤醒管道 */
    if (pipe(uring->wakeup_fd) < 0) {
        VOX_LOG_ERROR("Failed to create wakeup pipe: errno=%d", errno);
        io_uring_queue_exit(&uring->ring);
        return -1;
    }

    /* 设置非阻塞和 close-on-exec */
    for (int i = 0; i < 2; i++) {
        int flags = fcntl(uring->wakeup_fd[i], F_GETFD);
        if (flags >= 0) {
            fcntl(uring->wakeup_fd[i], F_SETFD, flags | FD_CLOEXEC);
        }
        flags = fcntl(uring->wakeup_fd[i], F_GETFL);
        if (flags >= 0) {
            fcntl(uring->wakeup_fd[i], F_SETFL, flags | O_NONBLOCK);
        }
    }

    /* 注册唤醒管道 */
    uring->wakeup_info = (vox_uring_fd_info_t*)vox_mpool_alloc(uring->mpool, sizeof(vox_uring_fd_info_t));
    if (!uring->wakeup_info) {
        VOX_LOG_ERROR("Failed to allocate wakeup info for io_uring");
        close(uring->wakeup_fd[0]);
        close(uring->wakeup_fd[1]);
        io_uring_queue_exit(&uring->ring);
        return -1;
    }

    uring->wakeup_info->fd = uring->wakeup_fd[0];
    uring->wakeup_info->events = VOX_BACKEND_READ;
    uring->wakeup_info->user_data = NULL;
    uring->wakeup_info->multishot_active = false;

    if (uring_add_poll(uring, uring->wakeup_info) != 0) {
        VOX_LOG_ERROR("Failed to register wakeup pipe with io_uring");
        vox_mpool_free(uring->mpool, uring->wakeup_info);
        close(uring->wakeup_fd[0]);
        close(uring->wakeup_fd[1]);
        io_uring_queue_exit(&uring->ring);
        return -1;
    }

    io_uring_submit(&uring->ring);

    uring->initialized = true;
    return 0;
}

/* 销毁 io_uring */
void vox_uring_destroy(vox_uring_t* uring) {
    if (!uring) {
        return;
    }

    vox_mpool_t* mpool = uring->mpool;
    bool own_mpool = uring->own_mpool;

    if (uring->initialized) {
        io_uring_queue_exit(&uring->ring);
    }

    if (uring->wakeup_fd[0] >= 0) {
        close(uring->wakeup_fd[0]);
    }
    if (uring->wakeup_fd[1] >= 0) {
        close(uring->wakeup_fd[1]);
    }

    if (uring->fd_map) {
        vox_htable_destroy(uring->fd_map);
    }

    if (uring->wakeup_info) {
        vox_mpool_free(uring->mpool, uring->wakeup_info);
    }

    if (mpool) {
        vox_mpool_free(mpool, uring);
        if (own_mpool) {
            vox_mpool_destroy(mpool);
        }
    }
}

/* 添加文件描述符 */
int vox_uring_add(vox_uring_t* uring, int fd, uint32_t events, void* user_data) {
    if (!uring || !uring->initialized || fd < 0) {
        return -1;
    }

    int key = fd;
    if (vox_htable_contains(uring->fd_map, &key, sizeof(key))) {
        return -1;
    }

    vox_uring_fd_info_t* info = (vox_uring_fd_info_t*)vox_mpool_alloc(
        uring->mpool, sizeof(vox_uring_fd_info_t));
    if (!info) {
        VOX_LOG_ERROR("Failed to allocate fd info for io_uring");
        return -1;
    }

    info->fd = fd;
    info->events = events;
    info->user_data = user_data;
    info->multishot_active = false;

    if (uring_add_poll(uring, info) != 0) {
        vox_mpool_free(uring->mpool, info);
        return -1;
    }

    if (vox_htable_set(uring->fd_map, &key, sizeof(key), info) != 0) {
        VOX_LOG_ERROR("Failed to add fd %d to io_uring fd map", fd);
        vox_mpool_free(uring->mpool, info);
        return -1;
    }

    /* 不立即提交，让 poll 批量提交 */
    return 0;
}

/* 修改文件描述符 */
int vox_uring_modify(vox_uring_t* uring, int fd, uint32_t events) {
    if (!uring || !uring->initialized || fd < 0) {
        return -1;
    }

    int key = fd;
    vox_uring_fd_info_t* info = (vox_uring_fd_info_t*)vox_htable_get(
        uring->fd_map, &key, sizeof(key));
    if (!info) {
        return -1;
    }

    /* 如果事件相同，无需修改 */
    if (info->events == events) {
        return 0;
    }

    /* 取消现有的 poll */
    if (info->multishot_active) {
        struct io_uring_sqe* sqe = io_uring_get_sqe(&uring->ring);
        if (sqe) {
            io_uring_prep_poll_remove(sqe, (__u64)(uintptr_t)info);
        }
        info->multishot_active = false;
    }

    info->events = events;

    /* 添加新的 poll */
    if (uring_add_poll(uring, info) != 0) {
        return -1;
    }

    return 0;
}

/* 移除文件描述符 */
int vox_uring_remove(vox_uring_t* uring, int fd) {
    if (!uring || !uring->initialized || fd < 0) {
        return -1;
    }

    int key = fd;
    vox_uring_fd_info_t* info = (vox_uring_fd_info_t*)vox_htable_get(
        uring->fd_map, &key, sizeof(key));
    if (info) {
        /* 取消 poll */
        if (info->multishot_active) {
            struct io_uring_sqe* sqe = io_uring_get_sqe(&uring->ring);
            if (sqe) {
                io_uring_prep_poll_remove(sqe, (__u64)(uintptr_t)info);
            }
        }

        vox_htable_delete(uring->fd_map, &key, sizeof(key));
        vox_mpool_free(uring->mpool, info);
    }

    return 0;
}

/* 将 poll 事件转换为 backend 事件 */
static inline uint32_t uring_to_backend_events(int32_t res) {
    uint32_t events = 0;
    if (res & POLLIN)  events |= VOX_BACKEND_READ;
    if (res & POLLOUT) events |= VOX_BACKEND_WRITE;
    if (res & POLLERR) events |= VOX_BACKEND_ERROR;
    if (res & POLLHUP) events |= VOX_BACKEND_HANGUP;
    return events;
}

/* 等待 IO 事件 */
int vox_uring_poll(vox_uring_t* uring, int timeout_ms, vox_uring_event_cb event_cb) {
    if (!uring || !uring->initialized || !event_cb) {
        return -1;
    }

    /* 使用 submit_and_wait 合并提交和等待 */
    struct __kernel_timespec ts;
    struct __kernel_timespec* ts_ptr = NULL;

    if (timeout_ms >= 0) {
        ts.tv_sec = timeout_ms / 1000;
        ts.tv_nsec = (timeout_ms % 1000) * 1000000L;
        ts_ptr = &ts;
    }

    /* 提交所有待处理的 SQE 并等待事件 */
    int ret;
    struct io_uring_cqe* cqe;

    if (timeout_ms == 0) {
        /* 非阻塞：先提交，再 peek */
        io_uring_submit(&uring->ring);
        ret = io_uring_peek_cqe(&uring->ring, &cqe);
    } else {
        /* 阻塞：使用 submit_and_wait_timeout */
        ret = io_uring_submit_and_wait_timeout(&uring->ring, &cqe, 1, ts_ptr, NULL);
    }

    if (ret < 0) {
        if (ret == -ETIME || ret == -EINTR || ret == -EAGAIN) {
            return 0;
        }
        VOX_LOG_ERROR("io_uring poll failed: ret=%d", ret);
        return -1;
    }

    /* 处理所有完成事件 */
    int processed = 0;
    unsigned head;
    unsigned cqe_count = 0;

    io_uring_for_each_cqe(&uring->ring, head, cqe) {
        cqe_count++;
        vox_uring_fd_info_t* info = (vox_uring_fd_info_t*)io_uring_cqe_get_data(cqe);

        if (!info) {
            continue;
        }

        int fd = info->fd;
        int32_t res = cqe->res;

        /* 检查 CQE 标志，判断 multishot 是否仍然活跃 */
        bool more = (cqe->flags & IORING_CQE_F_MORE) != 0;

        if (res < 0) {
            /* 错误或取消 */
            if (res != -ECANCELED) {
                event_cb(uring, fd, VOX_BACKEND_ERROR, info->user_data);
                processed++;
            }
            /* multishot 被取消，需要重新注册 */
            info->multishot_active = false;
            int key = fd;
            if (vox_htable_contains(uring->fd_map, &key, sizeof(key))) {
                uring_add_poll(uring, info);
            }
        } else if (fd == uring->wakeup_fd[0]) {
            /* 唤醒事件：清空管道 */
            char buf[256];
            while (read(uring->wakeup_fd[0], buf, sizeof(buf)) > 0);

            /* 如果不是 multishot 或 multishot 结束，重新注册 */
            if (!more) {
                info->multishot_active = false;
                uring_add_poll(uring, info);
            }
        } else {
            /* 正常 IO 事件 */
            uint32_t events = uring_to_backend_events(res);
            event_cb(uring, fd, events, info->user_data);
            processed++;

            /* 如果 multishot 结束（没有 MORE 标志），需要重新注册 */
            if (!more) {
                info->multishot_active = false;
                int key = fd;
                if (vox_htable_contains(uring->fd_map, &key, sizeof(key))) {
                    uring_add_poll(uring, info);
                }
            }
        }

        if (processed >= (int)uring->max_events) {
            break;
        }
    }

    /* 批量标记 CQE 已处理 */
    io_uring_cq_advance(&uring->ring, cqe_count);

    return processed;
}

/* 唤醒 io_uring */
int vox_uring_wakeup(vox_uring_t* uring) {
    if (!uring || !uring->initialized) {
        return -1;
    }

    char byte = 1;
    if (write(uring->wakeup_fd[1], &byte, 1) < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return 0;
        }
        /* EBADF: pipe 已关闭，通常为 loop 已销毁、工作线程稍晚调用 wakeup 的竞态，不记错误 */
        if (errno == EBADF) {
            return -1;
        }
        VOX_LOG_ERROR("Failed to write to wakeup pipe: errno=%d", errno);
        return -1;
    }

    return 0;
}

#endif /* VOX_USE_IOURING */
#endif /* VOX_OS_LINUX */
