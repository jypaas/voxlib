/*
 * vox_iocp.h - Windows IOCP backend 实现
 * 提供基于 IOCP 的高性能异步 IO
 */

#ifndef VOX_IOCP_H
#define VOX_IOCP_H

#ifdef VOX_OS_WINDOWS

#ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
#endif

#include "vox_os.h"
#include "vox_mpool.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* IOCP backend 不透明类型 */
typedef struct vox_iocp vox_iocp_t;

/* IO 事件回调函数类型
 * @param iocp IOCP 指针
 * @param fd 文件描述符
 * @param events 事件类型
 * @param user_data 用户数据
 * @param overlapped OVERLAPPED 指针
 * @param bytes_transferred 传输的字节数
 */
typedef void (*vox_iocp_event_cb)(vox_iocp_t* iocp,
                                   int fd,
                                   uint32_t events,
                                   void* user_data,
                                   void* overlapped,
                                   size_t bytes_transferred);

/* IOCP 配置 */
typedef struct {
    vox_mpool_t* mpool;         /* 内存池，如果为NULL则内部创建 */
    size_t max_events;          /* 每次 GetQueuedCompletionStatus 的最大事件数，0表示使用默认值256 */
} vox_iocp_config_t;

/**
 * 创建 IOCP backend
 * @param config 配置结构体，NULL表示使用默认配置
 * @return 成功返回 IOCP 指针，失败返回 NULL
 */
vox_iocp_t* vox_iocp_create(const vox_iocp_config_t* config);

/**
 * 初始化 IOCP
 * @param iocp IOCP 指针
 * @return 成功返回0，失败返回-1
 */
int vox_iocp_init(vox_iocp_t* iocp);

/**
 * 销毁 IOCP
 * @param iocp IOCP 指针
 */
void vox_iocp_destroy(vox_iocp_t* iocp);

/**
 * 添加文件描述符（Socket）
 * @param iocp IOCP 指针
 * @param fd Socket 句柄
 * @param events 关注的事件
 * @param user_data 用户数据
 * @return 成功返回0，失败返回-1
 */
int vox_iocp_add(vox_iocp_t* iocp, int fd, uint32_t events, void* user_data);

/**
 * 修改文件描述符
 * @param iocp IOCP 指针
 * @param fd Socket 句柄
 * @param events 新的事件
 * @return 成功返回0，失败返回-1
 */
int vox_iocp_modify(vox_iocp_t* iocp, int fd, uint32_t events);

/**
 * 移除文件描述符
 * @param iocp IOCP 指针
 * @param fd Socket 句柄
 * @return 成功返回0，失败返回-1
 */
int vox_iocp_remove(vox_iocp_t* iocp, int fd);

/**
 * 等待 IO 事件
 * @param iocp IOCP 指针
 * @param timeout_ms 超时时间（毫秒）
 * @param event_cb 事件回调函数
 * @return 成功返回处理的事件数量，失败返回-1
 */
int vox_iocp_poll(vox_iocp_t* iocp, int timeout_ms, vox_iocp_event_cb event_cb);

/**
 * 唤醒 IOCP（用于中断 GetQueuedCompletionStatus）
 * @param iocp IOCP 指针
 * @return 成功返回0，失败返回-1
 */
int vox_iocp_wakeup(vox_iocp_t* iocp);

/**
 * 将 socket 关联到 IOCP（用于 AcceptEx 等需要预先关联的场景）
 * @param iocp IOCP 指针
 * @param fd Socket 句柄
 * @param completion_key Completion key（通常是另一个 socket 的 key）
 * @return 成功返回0，失败返回-1
 */
int vox_iocp_associate_socket(vox_iocp_t* iocp, int fd, ULONG_PTR completion_key);

/**
 * 获取指定 fd 的 completion key（用于 AcceptEx 等场景）
 * @param iocp IOCP 指针
 * @param fd Socket 句柄
 * @return 成功返回 completion key，失败返回 NULL
 */
ULONG_PTR vox_iocp_get_completion_key(vox_iocp_t* iocp, int fd);

#ifdef __cplusplus
}
#endif

#endif /* VOX_OS_WINDOWS */

#endif /* VOX_IOCP_H */
