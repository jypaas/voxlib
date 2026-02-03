/*
 * vox.h - VoxLib 统一头文件
 * 外部只需包含本头文件即可使用整个项目提供的 API
 */

#ifndef VOX_H
#define VOX_H

#ifdef __cplusplus
extern "C" {
#endif

/* ===== 基础与平台 ===== */
#include "vox_os.h"

/* ===== 核心数据结构 ===== */
#include "vox_list.h"
#include "vox_mpool.h"
#include "vox_mheap.h"
#include "vox_vector.h"
#include "vox_string.h"
#include "vox_queue.h"
#include "vox_htable.h"
#include "vox_rbtree.h"

/* ===== 文件与时间 ===== */
#include "vox_file.h"
#include "vox_time.h"
#include "vox_log.h"

/* ===== 线程与并发 ===== */
#include "vox_thread.h"
#include "vox_mutex.h"
#include "vox_atomic.h"
#include "vox_tpool.h"

/* ===== 网络基础 ===== */
#include "vox_socket.h"
#include "vox_crypto.h"

/* ===== 文本与解析 ===== */
#include "vox_scanner.h"
#include "vox_regex.h"
#include "vox_json.h"
#include "vox_xml.h"
#include "vox_toml.h"
#include "vox_ini.h"

/* ===== 平台 Backend（按平台条件编译）===== */
#include "vox_select.h"
#include "vox_epoll.h"
#include "vox_kqueue.h"
#include "vox_uring.h"
#include "vox_iocp.h"

/* ===== 进程 ===== */
#include "vox_process.h"

/* ===== 异步 I/O 核心 ===== */
#include "vox_backend.h"
#include "vox_loop.h"
#include "vox_timer.h"
#include "vox_handle.h"
#include "vox_tcp.h"
#include "vox_udp.h"
#include "vox_dns.h"
#include "vox_fs.h"

/* ===== SSL/TLS ===== */
#include "ssl/vox_ssl.h"
#include "vox_tls.h"
#include "vox_dtls.h"

/* ===== HTTP ===== */
#include "http/vox_http_parser.h"
#include "http/vox_http_multipart_parser.h"
#include "http/vox_http_context.h"
#include "http/vox_http_router.h"
#include "http/vox_http_engine.h"
#include "http/vox_http_server.h"
#include "http/vox_http_ws.h"
#include "http/vox_http_client.h"
#include "http/vox_http_gzip.h"
#include "http/vox_http_mime.h"
#include "http/vox_http_middleware.h"

/* ===== WebSocket ===== */
#include "websocket/vox_websocket.h"
#include "websocket/vox_websocket_server.h"
#include "websocket/vox_websocket_client.h"

/* ===== Redis ===== */
#include "redis/vox_redis_parser.h"
#include "redis/vox_redis_client.h"
#include "redis/vox_redis_pool.h"

/* ===== 数据库 ===== */
#include "db/vox_db.h"
#include "db/vox_db_pool.h"
#include "db/vox_orm.h"

/* ===== 协程 ===== */
#include "coroutine/vox_coroutine_context.h"
#include "coroutine/vox_coroutine_promise.h"
#include "coroutine/vox_coroutine_pool.h"
#include "coroutine/vox_coroutine_scheduler.h"
#include "coroutine/vox_coroutine.h"
#include "coroutine/vox_coroutine_db.h"
#include "coroutine/vox_coroutine_dns.h"
#include "coroutine/vox_coroutine_fs.h"
#include "coroutine/vox_coroutine_redis.h"
#include "coroutine/vox_coroutine_http.h"
#include "coroutine/vox_coroutine_ws.h"

/*
 * 初始化 VoxLib（可多次调用，内部引用计数）
 */
void vox_init(void);

/*
 * 反初始化 VoxLib（与 vox_init 配对，引用计数归零后生效）
 */
void vox_fini(void);

/*
 * 返回库版本字符串，如 "1.0.0"
 */
const char* vox_version(void);

/* ===== 启动模式：多线程 / 多进程（适合 nginx 风格网络服务）===== */

/*
 * 常见高性能网络服务器启动模式（与 VoxLib 接口对应）：
 *
 * 1. 多进程 Reactor（nginx 风格）
 *    - 模型：1 个 master + N 个 worker 进程，每个 worker 独立事件循环，同一端口 bind+listen（SO_REUSEPORT），内核分发新连接。
 *    - 适用：Unix/Linux 高并发、进程隔离；Windows 无 SO_REUSEPORT 需单监听或改用单监听+worker。
 *    - 接口：vox_start(argc, argv, &params)，params.options.mode=VOX_START_MODE_PROCESS，params.worker_func_ex 赋值；worker 内 vox_loop_run(loop)。
 *
 * 2. 多线程 Reactor（每线程一 loop + 同端口监听）
 *    - 模型：N 个线程，每线程一个事件循环 + 监听 socket，同一端口 bind+listen（SO_REUSEPORT），内核分发连接。
 *    - 适用：Unix/Linux 多核、共享内存；Windows 无 SO_REUSEPORT 时需用单监听+worker。
 *    - 接口：vox_start(..., &params)，params.options.mode=VOX_START_MODE_THREAD，params.worker_func_ex 赋值；worker_func_ex 内创建 loop、bind、listen、vox_loop_run。
 *
 * 3. 单监听 + Worker 线程池（Half-Sync/Half-Async）
 *    - 模型：1 个线程跑事件循环 + 单监听 socket，accept 后将连接投递到 N 个 worker 线程池处理。
 *    - 适用：Windows 无 SO_REUSEPORT 时的高性能方案（单监听 + IOCP）；Unix 上也可用。
 *    - 接口：vox_start(..., &params)，params.options.mode=VOX_START_MODE_LISTENER_WORKERS，params.listener_func、params.worker_task 赋值；listener_func 内 bind+listen，connection_cb 里 vox_tpool_submit(worker_pool, worker_task, client_ctx)。
 *
 * 4. 单线程 Reactor
 *    - 模型：单进程单线程，一个事件循环处理监听与所有连接。
 *    - 适用：连接数/请求量不大、逻辑简单；无需多 worker 时。
 *    - 接口：不经过 vox_start，直接 vox_loop_create()、vox_tcp_listen()、vox_loop_run(loop)。
 *
 * 5. 多进程 + 单监听（由单进程 accept 再分发）
 *    - 模型：单进程 accept，通过 IPC 将连接分发给多个 worker 进程；VoxLib 未内置，需自行实现 IPC。
 *
 * 选择建议：Unix 多核高并发优先 1 或 2；Windows 高并发优先 3；轻量服务用 4。
 */

/** 启动运行模式：多线程、多进程或单监听+worker 多线程（与 vox_loop.h 的 vox_run_mode_t 区分） */
typedef enum vox_start_mode {
    VOX_START_MODE_THREAD = 0,         /**< 多线程（线程池/工作线程） */
    VOX_START_MODE_PROCESS = 1,        /**< 多进程（子进程池，类似 nginx master+worker） */
    VOX_START_MODE_LISTENER_WORKERS = 2 /**< 单监听 + worker 多线程：1 个线程跑事件循环+监听，worker_count 个线程处理连接（适合 Windows 无 SO_REUSEPORT 时高性能） */
} vox_start_mode_t;

/** 启动选项：由参数解析或调用方填充，决定以多线程还是多进程运行 */
typedef struct vox_start_options {
    vox_start_mode_t mode;       /**< 运行模式，默认 VOX_START_MODE_THREAD */
    uint32_t worker_count;     /**< 工作单元数量，0 表示自动（如 CPU 核心数） */
    bool daemon;               /**< 是否以守护进程运行（仅 Unix 多进程有效：fork 脱离终端后再 fork workers） */
    bool respawn_workers;      /**< 仅 Unix PROCESS 有效：master 常驻，SIGCHLD 时 respawn 死掉的 worker（类似 nginx） */
} vox_start_options_t;

/** 工作函数类型：每个线程或子进程执行的入口，返回值作为退出码（进程模式） */
typedef int (*vox_worker_func_t)(void* user_data);

/** 带 worker 下标的工作函数：适合 nginx 风格（每个 worker 跑事件循环，需知自己是第几个 worker） */
typedef int (*vox_worker_func_ex_t)(uint32_t worker_index, void* user_data);

/** 单监听+worker 模式：监听线程入口。在 loop 上创建监听、在 connection_cb 里用 vox_tpool_submit(worker_pool, worker_task, 连接上下文) 投递给 worker 线程池，最后调用 vox_loop_run(loop)。 */
typedef void (*vox_listener_func_t)(vox_loop_t* loop, vox_tpool_t* worker_pool,
                                    vox_tpool_task_func_t worker_task, void* user_data);

/** master 就绪回调：仅 Unix respawn 模式，master 进入主循环前调用一次（可写 pid 文件等） */
typedef void (*vox_on_master_ready_t)(void* user_data);

/** 统一启动参数：解析后按 mode 选用对应回调，未用到的指针可置 NULL */
typedef struct vox_start_params {
    vox_start_options_t options;   /**< 启动选项（可由 vox_start 内根据 argv 覆盖） */
    void* user_data;               /**< 透传给各回调 */
    vox_worker_func_t worker_func;           /**< thread/process 简单模式用，与 worker_func_ex 二选一 */
    vox_worker_func_ex_t worker_func_ex;     /**< thread/process 带 worker_index 用，优先于 worker_func */
    vox_listener_func_t listener_func;       /**< listener_workers 模式用 */
    vox_tpool_task_func_t worker_task;       /**< listener_workers 模式用（listener 内投递连接） */
    vox_on_master_ready_t on_master_ready;   /**< 仅 Unix process respawn：master 就绪时调用一次，可写 pid 文件 */
} vox_start_params_t;

/**
 * 统一启动入口：解析 argc/argv 写入 params->options，再按 mode 分发到多进程/多线程/单监听+worker 实现。
 * main 只需填充 vox_start_params（含 options 与对应回调），调用本函数一次即可。
 */
int vox_start(int argc, char** argv, vox_start_params_t* params);

/**
 * 获取当前 worker 下标（仅在 worker 线程/进程内有效）
 * 多进程/多线程网络服务中，每个 worker 内调用可得 0..worker_count-1。
 * @return 当前 worker 下标，非 worker 上下文返回 VOX_START_INVALID_WORKER_INDEX
 */
#define VOX_START_INVALID_WORKER_INDEX ((uint32_t)-1)
uint32_t vox_start_worker_index(void);

#ifdef __cplusplus
}
#endif

#endif /* VOX_H */
