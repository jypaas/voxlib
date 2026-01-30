/*
 * vox_backend.c - 平台抽象层实现
 * 根据平台自动选择 io_uring/epoll/kqueue/IOCP/select
 */

#include "vox_backend.h"
#include "vox_mpool.h"
#include "vox_os.h"
#include "vox_select.h"
#include "vox_log.h"
#include <string.h>
#include <stdlib.h>

/* 包含平台特定的实现 */
#ifdef VOX_OS_LINUX
    #include "vox_epoll.h"
    #ifdef VOX_USE_IOURING
        #include "vox_uring.h"
    #endif
#elif defined(VOX_OS_MACOS) || defined(VOX_OS_BSD)
    #include "vox_kqueue.h"
#elif defined(VOX_OS_WINDOWS)
    #include "vox_iocp.h"
#endif

/* Backend 结构 */
struct vox_backend {
    void* impl;  /* 平台特定的实现指针 */
    const char* name;
    vox_backend_type_t type;      /* Backend 类型 */
    vox_mpool_t* mpool;           /* 内存池 */
    bool own_mpool;                /* 是否拥有内存池 */
    vox_backend_event_cb event_cb;  /* 临时存储回调函数 */
    void* event_user_data;          /* 临时存储用户数据 */
};

/* 创建 backend */
vox_backend_t* vox_backend_create(vox_mpool_t* mpool) {
    vox_backend_config_t config = {0};
    config.mpool = mpool;
    config.max_events = 0;
    return vox_backend_create_with_config(&config);
}

/* 使用配置创建 backend */
vox_backend_t* vox_backend_create_with_config(const vox_backend_config_t* config) {
    vox_mpool_t* mpool = config ? config->mpool : NULL;
    bool own_mpool = false;
    
    /* 如果没有提供内存池，创建默认的 */
    if (!mpool) {
        mpool = vox_mpool_create();
        if (!mpool) {
            VOX_LOG_ERROR("Failed to create memory pool for backend");
            return NULL;
        }
        own_mpool = true;
    }
    
    /* 从内存池分配 backend 结构 */
    vox_backend_t* backend = (vox_backend_t*)vox_mpool_alloc(mpool, sizeof(vox_backend_t));
    if (!backend) {
        VOX_LOG_ERROR("Failed to allocate backend structure");
        if (own_mpool) {
            vox_mpool_destroy(mpool);
        }
        return NULL;
    }
    
    memset(backend, 0, sizeof(vox_backend_t));
    backend->mpool = mpool;
    backend->own_mpool = own_mpool;
    
    /* 确定要使用的 backend 类型 */
    vox_backend_type_t backend_type = config ? config->type : VOX_BACKEND_TYPE_AUTO;
    backend->type = backend_type;
    
    /* 如果明确指定使用 select，直接创建 */
    if (backend_type == VOX_BACKEND_TYPE_SELECT) {
        vox_select_config_t select_config;
        select_config.mpool = mpool;
        select_config.max_events = config ? config->max_events : 0;
        backend->impl = vox_select_create(&select_config);
        backend->name = "select";
        if (!backend->impl) {
            if (own_mpool) {
                vox_mpool_destroy(mpool);
            }
            return NULL;
        }
        return backend;
    }
    
    /* 根据平台和配置创建具体实现 */
#ifdef VOX_OS_LINUX
    if (backend_type == VOX_BACKEND_TYPE_AUTO) {
        /* 自动选择：优先尝试 io_uring，如果失败则使用 epoll */
        #ifdef VOX_USE_IOURING
            vox_uring_config_t uring_config;
            uring_config.mpool = mpool;
            uring_config.max_events = config ? config->max_events : 0;
            backend->impl = vox_uring_create(&uring_config);
            if (backend->impl) {
                backend->name = "io_uring";
                backend->type = VOX_BACKEND_TYPE_IOURING;
            } else {
                /* io_uring 失败，回退到 epoll */
                vox_epoll_config_t epoll_config;
                epoll_config.mpool = mpool;
                epoll_config.max_events = config ? config->max_events : 0;
                backend->impl = vox_epoll_create(&epoll_config);
                if (backend->impl) {
                    backend->name = "epoll";
                    backend->type = VOX_BACKEND_TYPE_EPOLL;
                } else {
                    /* epoll 也失败，回退到 select */
                    vox_select_config_t select_config;
                    select_config.mpool = mpool;
                    select_config.max_events = config ? config->max_events : 0;
                    backend->impl = vox_select_create(&select_config);
                    backend->name = "select";
                    backend->type = VOX_BACKEND_TYPE_SELECT;
                }
            }
        #else
            vox_epoll_config_t epoll_config;
            epoll_config.mpool = mpool;
            epoll_config.max_events = config ? config->max_events : 0;
            backend->impl = vox_epoll_create(&epoll_config);
            if (backend->impl) {
                backend->name = "epoll";
                backend->type = VOX_BACKEND_TYPE_EPOLL;
            } else {
                /* epoll 失败，回退到 select */
                vox_select_config_t select_config;
                select_config.mpool = mpool;
                select_config.max_events = config ? config->max_events : 0;
                backend->impl = vox_select_create(&select_config);
                backend->name = "select";
                backend->type = VOX_BACKEND_TYPE_SELECT;
            }
        #endif
    } else if (backend_type == VOX_BACKEND_TYPE_EPOLL) {
        vox_epoll_config_t epoll_config;
        epoll_config.mpool = mpool;
        epoll_config.max_events = config ? config->max_events : 0;
        backend->impl = vox_epoll_create(&epoll_config);
        if (backend->impl) {
            backend->name = "epoll";
        } else {
            /* epoll 失败，回退到 select */
            vox_select_config_t select_config;
            select_config.mpool = mpool;
            select_config.max_events = config ? config->max_events : 0;
            backend->impl = vox_select_create(&select_config);
            backend->name = "select";
        }
    } else if (backend_type == VOX_BACKEND_TYPE_IOURING) {
        #ifdef VOX_USE_IOURING
            vox_uring_config_t uring_config;
            uring_config.mpool = mpool;
            uring_config.max_events = config ? config->max_events : 0;
            backend->impl = vox_uring_create(&uring_config);
            if (backend->impl) {
                backend->name = "io_uring";
                backend->type = VOX_BACKEND_TYPE_IOURING;
            } else {
                /* io_uring 失败，回退到 epoll */
                vox_epoll_config_t epoll_config;
                epoll_config.mpool = mpool;
                epoll_config.max_events = config ? config->max_events : 0;
                backend->impl = vox_epoll_create(&epoll_config);
                if (backend->impl) {
                    backend->name = "epoll";
                    backend->type = VOX_BACKEND_TYPE_EPOLL;
                } else {
                    /* epoll 也失败，回退到 select */
                    vox_select_config_t select_config;
                    select_config.mpool = mpool;
                    select_config.max_events = config ? config->max_events : 0;
                    backend->impl = vox_select_create(&select_config);
                    backend->name = "select";
                    backend->type = VOX_BACKEND_TYPE_SELECT;
                }
            }
        #else
            /* io_uring 不可用，回退到 epoll */
            vox_epoll_config_t epoll_config;
            epoll_config.mpool = mpool;
            epoll_config.max_events = config ? config->max_events : 0;
            backend->impl = vox_epoll_create(&epoll_config);
            if (backend->impl) {
                backend->name = "epoll";
                backend->type = VOX_BACKEND_TYPE_EPOLL;
            } else {
                /* epoll 失败，回退到 select */
                vox_select_config_t select_config;
                select_config.mpool = mpool;
                select_config.max_events = config ? config->max_events : 0;
                backend->impl = vox_select_create(&select_config);
                backend->name = "select";
                backend->type = VOX_BACKEND_TYPE_SELECT;
            }
        #endif
    } else {
        /* 不支持的 backend 类型 */
        if (own_mpool) {
            vox_mpool_destroy(mpool);
        }
        return NULL;
    }
#elif defined(VOX_OS_MACOS) || defined(VOX_OS_BSD)
    if (backend_type == VOX_BACKEND_TYPE_AUTO) {
        /* 自动选择：优先尝试 kqueue，如果失败则使用 select */
        vox_kqueue_config_t kqueue_config;
        kqueue_config.mpool = mpool;
        kqueue_config.max_events = config ? config->max_events : 0;
        backend->impl = vox_kqueue_create(&kqueue_config);
        if (backend->impl) {
            backend->name = "kqueue";
            backend->type = VOX_BACKEND_TYPE_KQUEUE;
        } else {
            /* kqueue 失败，回退到 select */
            vox_select_config_t select_config;
            select_config.mpool = mpool;
            select_config.max_events = config ? config->max_events : 0;
            backend->impl = vox_select_create(&select_config);
            if (backend->impl) {
                backend->name = "select";
                backend->type = VOX_BACKEND_TYPE_SELECT;
            }
        }
    } else if (backend_type == VOX_BACKEND_TYPE_KQUEUE) {
        /* 用户明确指定 kqueue，使用 kqueue */
        vox_kqueue_config_t platform_config;
        platform_config.mpool = mpool;
        platform_config.max_events = config ? config->max_events : 0;
        backend->impl = vox_kqueue_create(&platform_config);
        if (backend->impl) {
            backend->name = "kqueue";
        } else {
            /* kqueue 失败，回退到 select */
            vox_select_config_t select_config;
            select_config.mpool = mpool;
            select_config.max_events = config ? config->max_events : 0;
            backend->impl = vox_select_create(&select_config);
            backend->name = "select";
            backend->type = VOX_BACKEND_TYPE_SELECT;
        }
    } else {
        /* 不支持的 backend 类型 */
        VOX_LOG_ERROR("Unsupported backend type on macOS/BSD: %d", backend_type);
        if (own_mpool) {
            vox_mpool_destroy(mpool);
        }
        return NULL;
    }
#elif defined(VOX_OS_WINDOWS)
    if (backend_type == VOX_BACKEND_TYPE_AUTO) {
        /* 自动选择：优先尝试 IOCP（高性能异步 IO），如果失败则使用 select */
        vox_iocp_config_t iocp_config;
        iocp_config.mpool = mpool;
        iocp_config.max_events = config ? config->max_events : 0;
        backend->impl = vox_iocp_create(&iocp_config);
        if (backend->impl) {
            backend->name = "iocp";
            backend->type = VOX_BACKEND_TYPE_IOCP;
        } else {
            /* IOCP 失败，回退到 select */
            vox_select_config_t select_config;
            select_config.mpool = mpool;
            select_config.max_events = config ? config->max_events : 0;
            backend->impl = vox_select_create(&select_config);
            if (backend->impl) {
                backend->name = "select";
                backend->type = VOX_BACKEND_TYPE_SELECT;
            }
        }
    } else if (backend_type == VOX_BACKEND_TYPE_IOCP) {
        /* 用户明确指定 IOCP，使用 IOCP（但注意：需要异步 IO 才能发挥性能） */
        vox_iocp_config_t platform_config;
        platform_config.mpool = mpool;
        platform_config.max_events = config ? config->max_events : 0;
        backend->impl = vox_iocp_create(&platform_config);
        if (backend->impl) {
            backend->name = "iocp";
        } else {
            /* IOCP 失败，回退到 select */
            vox_select_config_t select_config;
            select_config.mpool = mpool;
            select_config.max_events = config ? config->max_events : 0;
            backend->impl = vox_select_create(&select_config);
            backend->name = "select";
            backend->type = VOX_BACKEND_TYPE_SELECT;
        }
    } else {
        /* 不支持的 backend 类型 */
        VOX_LOG_ERROR("Unsupported backend type on Windows: %d", backend_type);
        if (own_mpool) {
            vox_mpool_destroy(mpool);
        }
        return NULL;
    }
#else
    /* 未知平台，使用 select 作为兜底 */
    vox_select_config_t select_config;
    select_config.mpool = mpool;
    select_config.max_events = config ? config->max_events : 0;
    backend->impl = vox_select_create(&select_config);
    backend->name = "select";
#endif
    
    if (!backend->impl) {
        VOX_LOG_ERROR("Failed to create backend implementation");
        if (own_mpool) {
            vox_mpool_destroy(mpool);
        }
        return NULL;
    }
    
    return backend;
}

/* 初始化 backend */
int vox_backend_init(vox_backend_t* backend) {
    if (!backend || !backend->impl) {
        VOX_LOG_ERROR("Invalid backend or implementation");
        return -1;
    }
    
    /* 根据 backend 类型初始化 */
    switch (backend->type) {
#ifdef VOX_OS_LINUX
        case VOX_BACKEND_TYPE_EPOLL:
            return vox_epoll_init(backend->impl);
        case VOX_BACKEND_TYPE_IOURING:
            #ifdef VOX_USE_IOURING
                return vox_uring_init(backend->impl);
            #else
                return -1;  /* io_uring 不可用 */
            #endif
        case VOX_BACKEND_TYPE_AUTO:
            /* 自动选择：根据实际创建的 backend 类型判断 */
            /* 注意：在 AUTO 模式下，backend->type 仍然是 AUTO，需要根据实际创建的 impl 类型判断 */
            /* 但由于我们已经知道实际类型，这里应该根据 backend->type 在创建时已设置的实际类型 */
            /* 实际上，在 AUTO 模式下创建时，如果成功创建了特定 backend，type 应该已经被设置 */
            /* 但为了兼容，我们仍然需要检查，这里简化处理：AUTO 模式下根据平台默认处理 */
            return vox_epoll_init(backend->impl);
#elif defined(VOX_OS_MACOS) || defined(VOX_OS_BSD)
        case VOX_BACKEND_TYPE_KQUEUE:
        case VOX_BACKEND_TYPE_AUTO:
            return vox_kqueue_init(backend->impl);
#elif defined(VOX_OS_WINDOWS)
        case VOX_BACKEND_TYPE_IOCP:
        case VOX_BACKEND_TYPE_AUTO:
            return vox_iocp_init(backend->impl);
#else
        case VOX_BACKEND_TYPE_AUTO:
            /* 未知平台，使用 select */
            return vox_select_init(backend->impl);
#endif
        case VOX_BACKEND_TYPE_SELECT:
            return vox_select_init(backend->impl);
        default:
            VOX_LOG_ERROR("Unknown backend type: %d", backend->type);
            return -1;
    }
}

/* 销毁 backend */
void vox_backend_destroy(vox_backend_t* backend) {
    if (!backend) {
        return;
    }
    
    vox_mpool_t* mpool = backend->mpool;
    bool own_mpool = backend->own_mpool;
    
    if (backend->impl) {
        switch (backend->type) {
#ifdef VOX_OS_LINUX
            case VOX_BACKEND_TYPE_IOURING:
                #ifdef VOX_USE_IOURING
                    vox_uring_destroy(backend->impl);
                #endif
                break;
            case VOX_BACKEND_TYPE_EPOLL:
            case VOX_BACKEND_TYPE_AUTO:
                vox_epoll_destroy(backend->impl);
                break;
#elif defined(VOX_OS_MACOS) || defined(VOX_OS_BSD)
            case VOX_BACKEND_TYPE_KQUEUE:
            case VOX_BACKEND_TYPE_AUTO:
                vox_kqueue_destroy(backend->impl);
                break;
#elif defined(VOX_OS_WINDOWS)
            case VOX_BACKEND_TYPE_IOCP:
            case VOX_BACKEND_TYPE_AUTO:
                vox_iocp_destroy(backend->impl);
                break;
#endif
            case VOX_BACKEND_TYPE_SELECT:
                vox_select_destroy(backend->impl);
                break;
            default:
                break;
        }
    }
    
    /* 从内存池释放 backend 结构 */
    if (mpool) {
        vox_mpool_free(mpool, backend);
        if (own_mpool) {
            vox_mpool_destroy(mpool);
        }
    }
}

/* 添加文件描述符 */
int vox_backend_add(vox_backend_t* backend, int fd, uint32_t events, void* user_data) {
    if (!backend || !backend->impl) {
        return -1;
    }
    
    switch (backend->type) {
#ifdef VOX_OS_LINUX
        case VOX_BACKEND_TYPE_IOURING:
            #ifdef VOX_USE_IOURING
                return vox_uring_add(backend->impl, fd, events, user_data);
            #endif
            return -1;
        case VOX_BACKEND_TYPE_EPOLL:
        case VOX_BACKEND_TYPE_AUTO:
            return vox_epoll_add(backend->impl, fd, events, user_data);
#elif defined(VOX_OS_MACOS) || defined(VOX_OS_BSD)
        case VOX_BACKEND_TYPE_KQUEUE:
        case VOX_BACKEND_TYPE_AUTO:
            return vox_kqueue_add(backend->impl, fd, events, user_data);
#elif defined(VOX_OS_WINDOWS)
        case VOX_BACKEND_TYPE_IOCP:
        case VOX_BACKEND_TYPE_AUTO:
            return vox_iocp_add(backend->impl, fd, events, user_data);
#endif
        case VOX_BACKEND_TYPE_SELECT:
            return vox_select_add(backend->impl, fd, events, user_data);
        default:
            return -1;
    }
}

/* 修改文件描述符 */
int vox_backend_modify(vox_backend_t* backend, int fd, uint32_t events) {
    if (!backend || !backend->impl) {
        return -1;
    }
    
    switch (backend->type) {
#ifdef VOX_OS_LINUX
        case VOX_BACKEND_TYPE_IOURING:
            #ifdef VOX_USE_IOURING
                return vox_uring_modify(backend->impl, fd, events);
            #endif
            return -1;
        case VOX_BACKEND_TYPE_EPOLL:
        case VOX_BACKEND_TYPE_AUTO:
            return vox_epoll_modify(backend->impl, fd, events);
#elif defined(VOX_OS_MACOS) || defined(VOX_OS_BSD)
        case VOX_BACKEND_TYPE_KQUEUE:
        case VOX_BACKEND_TYPE_AUTO:
            return vox_kqueue_modify(backend->impl, fd, events);
#elif defined(VOX_OS_WINDOWS)
        case VOX_BACKEND_TYPE_IOCP:
        case VOX_BACKEND_TYPE_AUTO:
            return vox_iocp_modify(backend->impl, fd, events);
#endif
        case VOX_BACKEND_TYPE_SELECT:
            return vox_select_modify(backend->impl, fd, events);
        default:
            return -1;
    }
}

/* 移除文件描述符 */
int vox_backend_remove(vox_backend_t* backend, int fd) {
    if (!backend || !backend->impl) {
        return -1;
    }
    
    switch (backend->type) {
#ifdef VOX_OS_LINUX
        case VOX_BACKEND_TYPE_IOURING:
            #ifdef VOX_USE_IOURING
                return vox_uring_remove(backend->impl, fd);
            #endif
            return -1;
        case VOX_BACKEND_TYPE_EPOLL:
        case VOX_BACKEND_TYPE_AUTO:
            return vox_epoll_remove(backend->impl, fd);
#elif defined(VOX_OS_MACOS) || defined(VOX_OS_BSD)
        case VOX_BACKEND_TYPE_KQUEUE:
        case VOX_BACKEND_TYPE_AUTO:
            return vox_kqueue_remove(backend->impl, fd);
#elif defined(VOX_OS_WINDOWS)
        case VOX_BACKEND_TYPE_IOCP:
        case VOX_BACKEND_TYPE_AUTO:
            return vox_iocp_remove(backend->impl, fd);
#endif
        case VOX_BACKEND_TYPE_SELECT:
            return vox_select_remove(backend->impl, fd);
        default:
            return -1;
    }
}

/* 临时存储当前 poll 的 backend（用于事件回调包装器） */
static vox_backend_t* g_current_poll_backend = NULL;

/* 事件回调包装函数（平台特定） */
static void select_event_wrapper(vox_select_t* select, int fd, uint32_t events, void* user_data) {
    (void)select;  /* 未使用的参数 */
    /* user_data 是从 select 传递过来的实际用户数据（TCP/UDP 内部数据） */
    if (g_current_poll_backend && g_current_poll_backend->event_cb) {
        /* select 不使用 OVERLAPPED，传递 NULL 和 0 */
        g_current_poll_backend->event_cb(g_current_poll_backend, fd, events, user_data, NULL, 0);
    }
}

#ifdef VOX_OS_LINUX

static void epoll_event_wrapper(vox_epoll_t* epoll, int fd, uint32_t events, void* user_data) {
    (void)epoll;  /* 未使用的参数 */
    /* user_data 是从 epoll 传递过来的实际用户数据（TCP/UDP 内部数据） */
    /* 使用临时存储的 backend */
    if (g_current_poll_backend && g_current_poll_backend->event_cb) {
        /* epoll 不使用 OVERLAPPED，传递 NULL 和 0 */
        g_current_poll_backend->event_cb(g_current_poll_backend, fd, events, user_data, NULL, 0);
    }
}
#ifdef VOX_USE_IOURING
static void uring_event_wrapper(vox_uring_t* uring, int fd, uint32_t events, void* user_data) {
    (void)uring;  /* 未使用的参数 */
    /* user_data 是从 uring 传递过来的实际用户数据（TCP/UDP 内部数据） */
    if (g_current_poll_backend && g_current_poll_backend->event_cb) {
        /* io_uring 不使用 OVERLAPPED，传递 NULL 和 0 */
        g_current_poll_backend->event_cb(g_current_poll_backend, fd, events, user_data, NULL, 0);
    }
}
#endif
#elif defined(VOX_OS_MACOS) || defined(VOX_OS_BSD)
static void kqueue_event_wrapper(vox_kqueue_t* kqueue, int fd, uint32_t events, void* user_data) {
    (void)kqueue;  /* 未使用的参数 */
    /* user_data 是从 kqueue 传递过来的实际用户数据（TCP/UDP 内部数据） */
    if (g_current_poll_backend && g_current_poll_backend->event_cb) {
        /* kqueue 不使用 OVERLAPPED，传递 NULL 和 0 */
        g_current_poll_backend->event_cb(g_current_poll_backend, fd, events, user_data, NULL, 0);
    }
}
#elif defined(VOX_OS_WINDOWS)
static void iocp_event_wrapper(vox_iocp_t* iocp, int fd, uint32_t events, void* user_data, void* overlapped, size_t bytes_transferred) {
    (void)iocp;  /* 未使用的参数 */
    /* user_data 是从 iocp 传递过来的实际用户数据（TCP/UDP 内部数据） */
    /* overlapped 和 bytes_transferred 从 IOCP 传递 */
    if (g_current_poll_backend && g_current_poll_backend->event_cb) {
        g_current_poll_backend->event_cb(g_current_poll_backend, fd, events, user_data, overlapped, bytes_transferred);
    }
}
#endif

/* 等待 IO 事件 */
int vox_backend_poll(vox_backend_t* backend, int timeout_ms, vox_backend_event_cb event_cb) {
    if (!backend || !backend->impl || !event_cb) {
        return -1;
    }
    
    /* 保存回调信息 */
    backend->event_cb = event_cb;
    backend->event_user_data = backend;  /* 传递 backend 指针 */
    
    switch (backend->type) {
#ifdef VOX_OS_LINUX
        case VOX_BACKEND_TYPE_IOURING:
            #ifdef VOX_USE_IOURING
                g_current_poll_backend = backend;
                {
                    int ret = vox_uring_poll(backend->impl, timeout_ms, uring_event_wrapper);
                    g_current_poll_backend = NULL;
                    return ret;
                }
            #endif
            return -1;
        case VOX_BACKEND_TYPE_EPOLL:
        case VOX_BACKEND_TYPE_AUTO:
            g_current_poll_backend = backend;
            {
                int ret = vox_epoll_poll(backend->impl, timeout_ms, epoll_event_wrapper);
                g_current_poll_backend = NULL;
                return ret;
            }
#elif defined(VOX_OS_MACOS) || defined(VOX_OS_BSD)
        case VOX_BACKEND_TYPE_KQUEUE:
        case VOX_BACKEND_TYPE_AUTO:
            g_current_poll_backend = backend;
            {
                int ret = vox_kqueue_poll(backend->impl, timeout_ms, kqueue_event_wrapper);
                g_current_poll_backend = NULL;
                return ret;
            }
#elif defined(VOX_OS_WINDOWS)
        case VOX_BACKEND_TYPE_IOCP:
        case VOX_BACKEND_TYPE_AUTO:
            g_current_poll_backend = backend;
            {
                int ret = vox_iocp_poll(backend->impl, timeout_ms, iocp_event_wrapper);
                g_current_poll_backend = NULL;
                return ret;
            }
#endif
        case VOX_BACKEND_TYPE_SELECT:
            g_current_poll_backend = backend;
            {
                int ret = vox_select_poll(backend->impl, timeout_ms, select_event_wrapper);
                g_current_poll_backend = NULL;
                return ret;
            }
        default:
            return -1;
    }
}

/* 唤醒 backend */
int vox_backend_wakeup(vox_backend_t* backend) {
    if (!backend || !backend->impl) {
        return -1;
    }
    
    switch (backend->type) {
#ifdef VOX_OS_LINUX
        case VOX_BACKEND_TYPE_IOURING:
            #ifdef VOX_USE_IOURING
                return vox_uring_wakeup(backend->impl);
            #endif
            return -1;
        case VOX_BACKEND_TYPE_EPOLL:
        case VOX_BACKEND_TYPE_AUTO:
            return vox_epoll_wakeup(backend->impl);
#elif defined(VOX_OS_MACOS) || defined(VOX_OS_BSD)
        case VOX_BACKEND_TYPE_KQUEUE:
        case VOX_BACKEND_TYPE_AUTO:
            return vox_kqueue_wakeup(backend->impl);
#elif defined(VOX_OS_WINDOWS)
        case VOX_BACKEND_TYPE_IOCP:
        case VOX_BACKEND_TYPE_AUTO:
            return vox_iocp_wakeup(backend->impl);
#endif
        case VOX_BACKEND_TYPE_SELECT:
            return vox_select_wakeup(backend->impl);
        default:
            return -1;
    }
}

/* 获取 backend 名称 */
const char* vox_backend_name(const vox_backend_t* backend) {
    return backend ? backend->name : "unknown";
}

/* 获取 backend 类型 */
vox_backend_type_t vox_backend_get_type(const vox_backend_t* backend) {
    return backend ? backend->type : VOX_BACKEND_TYPE_AUTO;
}

/* 获取 IOCP 实例（仅用于 IOCP backend） */
void* vox_backend_get_iocp_impl(vox_backend_t* backend) {
    if (!backend || backend->type != VOX_BACKEND_TYPE_IOCP) {
        return NULL;
    }
    return backend->impl;
}
