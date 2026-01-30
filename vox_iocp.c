/*
 * vox_iocp.c - Windows IOCP backend 实现
 */

#ifdef VOX_OS_WINDOWS

#ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
#endif

#include "vox_iocp.h"
#include "vox_os.h"
#include "vox_backend.h"
#include "vox_mpool.h"
#include "vox_htable.h"
#include "vox_log.h"
#include <mswsock.h>  /* 用于 AcceptEx, ConnectEx 等 */
#include <string.h>
#include <stdlib.h>

/* 默认最大事件数 - 优化为4096以支持高并发场景 */
#define VOX_IOCP_DEFAULT_MAX_EVENTS 4096

/* IO 完成键（用于标识不同的 Socket） */
typedef struct {
    int fd;
    void* user_data;
} vox_iocp_key_t;

/* GetQueuedCompletionStatusEx 函数指针类型 */
typedef BOOL (WINAPI *LPFN_GETQUEUEDCOMPLETIONSTATUSEX)(
    HANDLE CompletionPort,
    LPOVERLAPPED_ENTRY lpCompletionPortEntries,
    ULONG ulCount,
    PULONG ulNumEntriesRemoved,
    DWORD dwMilliseconds,
    BOOL fAlertable
);

/* IOCP 结构 */
struct vox_iocp {
    HANDLE iocp;                     /* IOCP 句柄 */
    HANDLE wakeup_event;             /* 用于唤醒的事件对象 */
    size_t max_events;               /* 每次处理的最大事件数 */
    OVERLAPPED_ENTRY* entries;       /* 完成端口条目数组 */
    vox_htable_t* key_map;           /* fd -> key 映射 */
    vox_mpool_t* mpool;              /* 内存池 */
    bool own_mpool;                  /* 是否拥有内存池 */
    bool initialized;                /* 是否已初始化 */
    LPFN_GETQUEUEDCOMPLETIONSTATUSEX fnGetQueuedCompletionStatusEx;  /* 函数指针 */
    bool use_ex;                     /* 是否使用 GetQueuedCompletionStatusEx */
};

/* 创建 IOCP backend */
vox_iocp_t* vox_iocp_create(const vox_iocp_config_t* config) {
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
    
    /* 从内存池分配 IOCP 结构 */
    vox_iocp_t* iocp = (vox_iocp_t*)vox_mpool_alloc(mpool, sizeof(vox_iocp_t));
    if (!iocp) {
        if (own_mpool) {
            vox_mpool_destroy(mpool);
        }
        return NULL;
    }
    
    memset(iocp, 0, sizeof(vox_iocp_t));
    iocp->iocp = INVALID_HANDLE_VALUE;
    iocp->wakeup_event = INVALID_HANDLE_VALUE;
    iocp->max_events = VOX_IOCP_DEFAULT_MAX_EVENTS;
    iocp->mpool = mpool;
    iocp->own_mpool = own_mpool;
    iocp->fnGetQueuedCompletionStatusEx = NULL;
    iocp->use_ex = false;
    
    /* 应用配置 */
    if (config && config->max_events > 0) {
        iocp->max_events = config->max_events;
    }
    
    /* 创建 key 映射表 - 可选：根据预期连接数预分配容量以优化性能 */
    vox_htable_config_t htable_config = {0};
    /* 如果知道预期连接数，可以设置 initial_capacity = expected_connections * 4 / 3 */
    /* 当前使用默认配置，哈希表会自动扩容 */
    iocp->key_map = vox_htable_create_with_config(iocp->mpool, &htable_config);
    if (!iocp->key_map) {
        VOX_LOG_ERROR("Failed to create key map for IOCP");
        vox_mpool_free(iocp->mpool, iocp);
        if (own_mpool) {
            vox_mpool_destroy(mpool);
        }
        return NULL;
    }
    
    /* 分配条目数组 */
    iocp->entries = (OVERLAPPED_ENTRY*)vox_mpool_alloc(
        iocp->mpool, 
        iocp->max_events * sizeof(OVERLAPPED_ENTRY)
    );
    if (!iocp->entries) {
        VOX_LOG_ERROR("Failed to allocate entries array for IOCP");
        vox_htable_destroy(iocp->key_map);
        vox_mpool_free(iocp->mpool, iocp);
        if (own_mpool) {
            vox_mpool_destroy(mpool);
        }
        return NULL;
    }
    
    return iocp;
}

/* 初始化 IOCP */
int vox_iocp_init(vox_iocp_t* iocp) {
    if (!iocp || iocp->initialized) {
        VOX_LOG_ERROR("Invalid IOCP or already initialized");
        return -1;
    }

    /* 尝试动态加载 GetQueuedCompletionStatusEx */
    HMODULE hKernel32 = GetModuleHandleA("kernel32.dll");
    if (hKernel32) {
        iocp->fnGetQueuedCompletionStatusEx = (LPFN_GETQUEUEDCOMPLETIONSTATUSEX)
            GetProcAddress(hKernel32, "GetQueuedCompletionStatusEx");
        if (iocp->fnGetQueuedCompletionStatusEx) {
            iocp->use_ex = true;
        }
    }

    /* 创建 IOCP 实例 */
    /* NumberOfConcurrentThreads = 1，因为 vox_loop 是单线程设计的 */
    iocp->iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 1);
    if (iocp->iocp == NULL) {
        DWORD error = GetLastError();
        VOX_LOG_ERROR("Failed to create IOCP: error=%lu", error);
        return -1;
    }

    iocp->initialized = true;
    return 0;
}

/* 销毁 IOCP */
void vox_iocp_destroy(vox_iocp_t* iocp) {
    if (!iocp) {
        return;
    }
    
    if (iocp->iocp != INVALID_HANDLE_VALUE) {
        CloseHandle(iocp->iocp);
    }
    
    if (iocp->wakeup_event != INVALID_HANDLE_VALUE) {
        CloseHandle(iocp->wakeup_event);
    }
    
    if (iocp->key_map) {
        vox_htable_destroy(iocp->key_map);
    }
    
    vox_mpool_t* mpool = iocp->mpool;
    bool own_mpool = iocp->own_mpool;
    
    /* 1. 释放 iocp 结构本身（在销毁内存池之前） */
    vox_mpool_free(mpool, iocp);
    
    /* 2. 如果拥有内存池，销毁它 */
    if (own_mpool) {
        vox_mpool_destroy(mpool);
    }
}

/* 添加文件描述符 */
int vox_iocp_add(vox_iocp_t* iocp, int fd, uint32_t events, void* user_data) {
    (void)events;  /* IOCP 不直接使用 events 参数 */
    if (!iocp || !iocp->initialized || fd == INVALID_SOCKET) {
        return -1;
    }
    
    /* 检查是否已存在 */
    int key_val = fd;
    if (vox_htable_contains(iocp->key_map, &key_val, sizeof(key_val))) {
        /* 已存在：更新 user_data */
        vox_iocp_key_t* existing_key = (vox_iocp_key_t*)vox_htable_get(
            iocp->key_map, &key_val, sizeof(key_val));
        if (existing_key) {
            existing_key->user_data = user_data;
            return 0;
        }
        return -1;
    }
    
    /* 创建完成键 */
    vox_iocp_key_t* key = (vox_iocp_key_t*)vox_mpool_alloc(
        iocp->mpool, 
        sizeof(vox_iocp_key_t)
    );
    if (!key) {
        return -1;
    }
    
    key->fd = fd;
    key->user_data = user_data;
    
    /* 添加到映射表（先添加到哈希表，以便失败时能回滚） */
    if (vox_htable_set(iocp->key_map, &key_val, sizeof(key_val), key) != 0) {
        vox_mpool_free(iocp->mpool, key);
        return -1;
    }
    
    /* 将 Socket 关联到 IOCP */
    HANDLE result = CreateIoCompletionPort((HANDLE)(ULONG_PTR)fd, iocp->iocp,
                                          (ULONG_PTR)key, 0);
    if (result == NULL) {
        DWORD error = GetLastError();
        if (error == ERROR_INVALID_PARAMETER) {
            /* Socket 已经关联到 IOCP，这是可以接受的 */
        } else {
            /* 关联失败，回滚哈希表 */
            VOX_LOG_ERROR("Failed to associate socket %d with IOCP: error=%lu", fd, error);
            vox_htable_delete(iocp->key_map, &key_val, sizeof(key_val));
            vox_mpool_free(iocp->mpool, key);
            return -1;
        }
    }
    
    /* 注意：IOCP 是事件驱动的，不需要像 epoll/kqueue 那样注册事件 */
    /* 实际的 IO 操作（AcceptEx, ConnectEx, WSARecv, WSASend）会自动完成 */
    
    return 0;
}

/* 将 socket 关联到 IOCP（用于 AcceptEx 等需要预先关联的场景） */
int vox_iocp_associate_socket(vox_iocp_t* iocp, int fd, ULONG_PTR completion_key) {
    if (!iocp || !iocp->initialized || fd == INVALID_SOCKET) {
        return -1;
    }
    
    /* 直接关联 socket 到 IOCP，使用指定的 completion key */
    if (CreateIoCompletionPort((HANDLE)(ULONG_PTR)fd, iocp->iocp, completion_key, 0) == NULL) {
        return -1;
    }
    
    return 0;
}

/* 获取指定 fd 的 completion key */
ULONG_PTR vox_iocp_get_completion_key(vox_iocp_t* iocp, int fd) {
    if (!iocp || !iocp->initialized || fd == INVALID_SOCKET) {
        return 0;
    }
    
    /* 从 key_map 获取 key */
    int key_val = fd;
    vox_iocp_key_t* key = (vox_iocp_key_t*)vox_htable_get(
        iocp->key_map, &key_val, sizeof(key_val));
    
    if (key) {
        return (ULONG_PTR)key;
    }
    
    return 0;
}

/* 修改文件描述符 */
int vox_iocp_modify(vox_iocp_t* iocp, int fd, uint32_t events) {
    if (!iocp || !iocp->initialized || fd == INVALID_SOCKET) {
        return -1;
    }
    
    /* IOCP 不需要修改事件，因为它是基于完成端口的 */
    /* 只需要更新 user_data */
    int key_val = fd;
    vox_iocp_key_t* key = (vox_iocp_key_t*)vox_htable_get(
        iocp->key_map, 
        &key_val, 
        sizeof(key_val)
    );
    if (!key) {
        return -1;  /* 不存在 */
    }
    
    /* 可以在这里更新 user_data 或其他信息 */
    (void)events;  /* IOCP 不直接使用 events */
    
    return 0;
}

/* 移除文件描述符 */
int vox_iocp_remove(vox_iocp_t* iocp, int fd) {
    if (!iocp || !iocp->initialized || (SOCKET)fd == INVALID_SOCKET) {
        return -1;
    }
    
    /* 从映射表移除并释放内存 */
    int key_val = fd;
    vox_iocp_key_t* key = (vox_iocp_key_t*)vox_htable_get(
        iocp->key_map, 
        &key_val, 
        sizeof(key_val)
    );
    if (key) {
        vox_htable_delete(iocp->key_map, &key_val, sizeof(key_val));
        vox_mpool_free(iocp->mpool, key);
    }
    
    /* 注意：在 IOCP 中，无法直接“取消”与完成端口的关联。
     * 关联会一直持续到 Handle 被关闭。此处我们仅移除内部映射。
     * 实际的 closesocket(fd) 应该由上层句柄（如 vox_tcp_t）负责。 */
    
    return 0;
}

/* 检测 socket 状态的上下文 */
typedef struct {
    vox_iocp_t* iocp;
    vox_iocp_event_cb event_cb;
    int processed;
    fd_set* read_fds;
    fd_set* write_fds;
    fd_set* error_fds;
} check_socket_ctx_t;

/* 添加 socket 到 fd_set 的回调 */
static void add_socket_to_fdset_cb(const void* key, size_t key_len, void* value, void* user_data) {
    (void)key;
    (void)key_len;
    check_socket_ctx_t* ctx = (check_socket_ctx_t*)user_data;
    vox_iocp_key_t* iocp_key = (vox_iocp_key_t*)value;
    
    if (iocp_key) {
        SOCKET sock = (SOCKET)iocp_key->fd;
        FD_SET(sock, ctx->read_fds);
        FD_SET(sock, ctx->write_fds);
        FD_SET(sock, ctx->error_fds);
    }
}

/* 等待 IO 事件 */
int vox_iocp_poll(vox_iocp_t* iocp, int timeout_ms, vox_iocp_event_cb event_cb) {
    if (!iocp || !iocp->initialized || !event_cb) {
        return -1;
    }

    /* 检查 IOCP 句柄是否有效 */
    if (iocp->iocp == NULL || iocp->iocp == INVALID_HANDLE_VALUE) {
        return -1;
    }

    if (iocp->entries == NULL || iocp->max_events == 0) {
        return -1;
    }
    
    /* 转换超时时间 */
    DWORD timeout = timeout_ms < 0 ? INFINITE : (DWORD)timeout_ms;
    
    /* 等待完成端口 */
    ULONG num_removed = 0;
    BOOL success = FALSE;
    DWORD error = 0;
    
    /* 如果支持 GetQueuedCompletionStatusEx，使用它 */
    if (iocp->use_ex && iocp->fnGetQueuedCompletionStatusEx) {
        success = iocp->fnGetQueuedCompletionStatusEx(
            iocp->iocp,
            iocp->entries,
            (ULONG)iocp->max_events,
            &num_removed,
            timeout,
            FALSE
        );

        if (!success) {
            error = GetLastError();
            /* 如果错误是 ERROR_INVALID_HANDLE (998)，回退到单次调用 */
            if (error == 998) {
                iocp->use_ex = false;
            }
        }
    }
    
    /* 如果不使用 Ex 版本或 Ex 版本失败，使用单次调用 */
    if (!iocp->use_ex || !success) {
        /* 使用 GetQueuedCompletionStatus 单次调用 */
        DWORD bytes_transferred = 0;
        ULONG_PTR completion_key = 0;
        LPOVERLAPPED overlapped = NULL;
        
        success = GetQueuedCompletionStatus(
            iocp->iocp,
            &bytes_transferred,
            &completion_key,
            &overlapped,
            timeout
        );
        
        if (success && overlapped) {
            /* 转换为 OVERLAPPED_ENTRY 格式 */
            num_removed = 1;
            iocp->entries[0].lpCompletionKey = (ULONG_PTR)completion_key;
            iocp->entries[0].lpOverlapped = overlapped;
            iocp->entries[0].dwNumberOfBytesTransferred = bytes_transferred;
            iocp->entries[0].Internal = 0;
        } else {
            error = GetLastError();
            num_removed = 0;
        }
    }
    
    int processed = 0;
    
    if (!success) {
        if (error == WAIT_TIMEOUT) {
            return 0;  /* 超时，没有事件 */
        }
        VOX_LOG_ERROR("IOCP poll failed: error=%lu", error);
        return -1;
    }

    /* 处理完成端口事件 */
    for (ULONG i = 0; i < num_removed; i++) {
        vox_iocp_key_t* key = (vox_iocp_key_t*)iocp->entries[i].lpCompletionKey;
        OVERLAPPED* overlapped = iocp->entries[i].lpOverlapped;
        DWORD bytes_transferred = iocp->entries[i].dwNumberOfBytesTransferred;

        /* 检查是否是唤醒信号 */
        if (key == NULL && overlapped == NULL) {
            continue;
        }

        if (overlapped) {
            /* 异步 IO 操作完成
             *
             * 重要：IOCP 模式下，我们通过 OVERLAPPED 指针来识别操作类型和对应的句柄。
             * TCP/UDP 层使用扩展的 OVERLAPPED 结构，其中包含了操作类型和句柄指针。
             * 这里我们直接传递 OVERLAPPED 指针，让 TCP/UDP 层通过 VOX_CONTAINING_RECORD 宏
             * 来获取扩展结构，从而识别操作类型和对应的句柄。
             *
             * 事件类型（events）在 IOCP 模式下不再重要，因为 TCP/UDP 层会通过
             * OVERLAPPED 指针来确定操作类型。我们传递一个通用事件，让上层处理。
             */
            uint32_t events = VOX_BACKEND_READ | VOX_BACKEND_WRITE;

            /* 传递 OVERLAPPED 指针和 bytes_transferred，让 TCP/UDP 层通过
             * OVERLAPPED 指针来识别操作类型和对应的句柄 */
            event_cb(iocp, key ? key->fd : -1, events,
                     key ? key->user_data : NULL,
                     overlapped,
                     (size_t)bytes_transferred);
            processed++;
        } else if (key != NULL) {
            /* 没有 OVERLAPPED，可能是通过 PostQueuedCompletionStatus 发送的关闭信号 */
            event_cb(iocp, key->fd, VOX_BACKEND_HANGUP, key->user_data, NULL, 0);
            processed++;
        }
    }
    
    return processed;
}

/* 唤醒 IOCP */
int vox_iocp_wakeup(vox_iocp_t* iocp) {
    if (!iocp || !iocp->initialized) {
        return -1;
    }
    
    /* 发送一个特殊的完成通知到 IOCP 以唤醒工作线程。
     * lpCompletionKey 设置为 NULL 表示这是一个唤醒信号。 */
    if (!PostQueuedCompletionStatus(iocp->iocp, 0, 0, NULL)) {
        DWORD error = GetLastError();
        VOX_LOG_ERROR("Failed to wakeup IOCP: error=%lu", error);
        return -1;
    }
    
    return 0;
}

#endif /* VOX_OS_WINDOWS */
