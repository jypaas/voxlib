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

#ifdef __cplusplus
}
#endif

#endif /* VOX_H */
