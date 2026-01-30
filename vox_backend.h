/*
 * vox_backend.h - 平台抽象层接口
 * 提供统一的异步 IO backend 接口，支持 io_uring/epoll/kqueue/IOCP/select
 */

#ifndef VOX_BACKEND_H
#define VOX_BACKEND_H

#include "vox_os.h"
#include "vox_mpool.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Backend 不透明类型 */
#ifndef VOX_BACKEND_T_DEFINED
#define VOX_BACKEND_T_DEFINED
typedef struct vox_backend vox_backend_t;
#endif

/* Backend 类型 */
typedef enum {
    VOX_BACKEND_TYPE_AUTO = 0,  /* 自动选择（默认） */
    VOX_BACKEND_TYPE_EPOLL,     /* Linux epoll */
    VOX_BACKEND_TYPE_IOURING,   /* Linux io_uring */
    VOX_BACKEND_TYPE_KQUEUE,    /* macOS/BSD kqueue */
    VOX_BACKEND_TYPE_IOCP,      /* Windows IOCP */
    VOX_BACKEND_TYPE_SELECT     /* select（跨平台兜底方案） */
} vox_backend_type_t;

/* IO 事件类型 */
typedef enum {
    VOX_BACKEND_READ = 0x01,    /* 可读事件 */
    VOX_BACKEND_WRITE = 0x02,   /* 可写事件 */
    VOX_BACKEND_ERROR = 0x04,   /* 错误事件 */
    VOX_BACKEND_HANGUP = 0x08   /* 挂起事件 */
} vox_backend_event_t;

/* IO 事件回调函数类型
 * @param backend backend 指针
 * @param fd 文件描述符
 * @param events 事件类型
 * @param user_data 用户数据
 * @param overlapped OVERLAPPED 指针（仅 IOCP backend 有效，其他 backend 为 NULL）
 * @param bytes_transferred 传输的字节数（仅 IOCP backend 有效）
 */
typedef void (*vox_backend_event_cb)(vox_backend_t* backend,
                                      int fd,
                                      uint32_t events,
                                      void* user_data,
                                      void* overlapped,
                                      size_t bytes_transferred);

/* Backend 配置 */
typedef struct {
    vox_mpool_t* mpool;         /* 内存池，如果为NULL则内部创建 */
    size_t max_events;          /* 每次 poll 的最大事件数，0表示使用默认值 */
    vox_backend_type_t type;    /* Backend 类型，VOX_BACKEND_TYPE_AUTO 表示自动选择 */
} vox_backend_config_t;

/**
 * 创建 backend（使用默认配置）
 * @param mpool 内存池，如果为NULL则内部创建
 * @return 成功返回 backend 指针，失败返回 NULL
 */
vox_backend_t* vox_backend_create(vox_mpool_t* mpool);

/**
 * 使用配置创建 backend
 * @param config 配置结构体，NULL表示使用默认配置
 * @return 成功返回 backend 指针，失败返回 NULL
 */
vox_backend_t* vox_backend_create_with_config(const vox_backend_config_t* config);

/**
 * 初始化 backend
 * @param backend backend 指针
 * @return 成功返回0，失败返回-1
 */
int vox_backend_init(vox_backend_t* backend);

/**
 * 销毁 backend
 * @param backend backend 指针
 */
void vox_backend_destroy(vox_backend_t* backend);

/**
 * 添加文件描述符到 backend
 * @param backend backend 指针
 * @param fd 文件描述符
 * @param events 关注的事件（VOX_BACKEND_READ | VOX_BACKEND_WRITE 等）
 * @param user_data 用户数据
 * @return 成功返回0，失败返回-1
 */
int vox_backend_add(vox_backend_t* backend, int fd, uint32_t events, void* user_data);

/**
 * 修改文件描述符的关注事件
 * @param backend backend 指针
 * @param fd 文件描述符
 * @param events 新的事件（VOX_BACKEND_READ | VOX_BACKEND_WRITE 等）
 * @return 成功返回0，失败返回-1
 */
int vox_backend_modify(vox_backend_t* backend, int fd, uint32_t events);

/**
 * 从 backend 移除文件描述符
 * @param backend backend 指针
 * @param fd 文件描述符
 * @return 成功返回0，失败返回-1
 */
int vox_backend_remove(vox_backend_t* backend, int fd);

/**
 * 等待 IO 事件（poll）
 * @param backend backend 指针
 * @param timeout_ms 超时时间（毫秒），-1表示无限等待，0表示不等待
 * @param event_cb 事件回调函数
 * @return 成功返回处理的事件数量，失败返回-1
 */
int vox_backend_poll(vox_backend_t* backend, int timeout_ms, vox_backend_event_cb event_cb);

/**
 * 唤醒 backend（用于中断 poll 等待）
 * @param backend backend 指针
 * @return 成功返回0，失败返回-1
 */
int vox_backend_wakeup(vox_backend_t* backend);

/**
 * 获取 backend 名称（用于调试）
 * @param backend backend 指针
 * @return 返回 backend 名称字符串
 */
const char* vox_backend_name(const vox_backend_t* backend);

/**
 * 获取 backend 类型
 * @param backend backend 指针
 * @return 返回 backend 类型
 */
vox_backend_type_t vox_backend_get_type(const vox_backend_t* backend);

/**
 * 获取 IOCP 实例（仅用于 IOCP backend，用于特殊场景如 AcceptEx）
 * @param backend backend 指针
 * @return 返回 IOCP 实例指针，如果不是 IOCP backend 则返回 NULL
 */
void* vox_backend_get_iocp_impl(vox_backend_t* backend);

#ifdef __cplusplus
}
#endif

#endif /* VOX_BACKEND_H */
