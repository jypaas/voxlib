/*
 * vox_http_context.h - HTTP Context（类似 Gin Context）
 * - 封装 request/response
 * - 提供 middleware chain 控制：next()/abort()
 */

#ifndef VOX_HTTP_CONTEXT_H
#define VOX_HTTP_CONTEXT_H

#include "../vox_os.h"
#include "../vox_mpool.h"
#include "../vox_loop.h"
#include "../vox_string.h"
#include "vox_http_parser.h"
#include "vox_http_middleware.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 前向声明，避免与 vox_http_engine.h -> vox_http_router.h -> vox_http_context.h 循环依赖 */
struct vox_http_engine;

typedef struct vox_http_request vox_http_request_t;
typedef struct vox_http_response vox_http_response_t;

/* 公开：Context 不透明类型
 * 说明：vox_http_context_t 已在 vox_http_middleware.h 前向声明并 typedef，
 * 这里避免重复 typedef（Linux -Wpedantic 会告警）。 */

/* ===== 公共数据结构（只读/只追加） ===== */

typedef struct {
    vox_strview_t name;
    vox_strview_t value;
} vox_http_header_t;

typedef struct {
    vox_strview_t name;
    vox_strview_t value;
} vox_http_param_t;

struct vox_http_request {
    vox_http_method_t method;
    int http_major;
    int http_minor;

    vox_strview_t raw_url; /* 原始 URL：/path?x=1 */
    vox_strview_t path;    /* 仅 path：/path */
    vox_strview_t query;   /* query：x=1 */

    /* element: vox_http_header_t*（header name/value 视图，大小写保持原样） */
    void* headers; /* vox_vector_t*，为避免在头文件引入 vox_vector.h 用 void* */

    /* body 缓冲（默认在内存中累计） */
    vox_string_t* body;

    bool is_upgrade;
};

struct vox_http_response {
    int status;
    /* element: vox_http_header_t* */
    void* headers; /* vox_vector_t* */
    vox_string_t* body;
};

/* ===== middleware chain ===== */
void vox_http_context_next(vox_http_context_t* ctx);
void vox_http_context_abort(vox_http_context_t* ctx);
bool vox_http_context_is_aborted(const vox_http_context_t* ctx);
/** 当前 handler 链下标（defer 前保存，用于异步回调后恢复执行） */
size_t vox_http_context_get_index(const vox_http_context_t* ctx);
/** handler 链长度（用于校验 resume_at 的 at_index 合法性） */
size_t vox_http_context_get_handler_count(const vox_http_context_t* ctx);
/** 恢复执行链：置 aborted=false 并设 index，之后可调用 next() 继续执行后续 handler */
void vox_http_context_resume_at(vox_http_context_t* ctx, size_t at_index);

/**
 * 派发到指定的 handler 链（用于 vhost 等场景：先按 Host 选 vhost，再执行该 vhost 的路由链）
 * 会设置 ctx 的 handlers/params 并立即执行该链（等价于设置后调用 next）。
 * @param ctx HTTP 上下文
 * @param handlers 要执行的 handler 数组（可为 NULL 表示无 handler）
 * @param handler_count handler 数量
 * @param params 路径参数（可为 NULL）
 * @param param_count 参数数量
 */
void vox_http_context_dispatch(vox_http_context_t* ctx,
                               vox_http_handler_cb* handlers,
                               size_t handler_count,
                               vox_http_param_t* params,
                               size_t param_count);

/* ===== async/defer response ===== */
/**
 * 标记当前请求为“延迟响应”：
 * - handler 返回后 server 不会立即 build+write 响应
 * - 连接读取会暂停，直到你调用 vox_http_context_finish()
 *
 * 注意：finish 应在 loop 线程调用（例如配合 VOX_DB_CALLBACK_LOOP）
 */
void vox_http_context_defer(vox_http_context_t* ctx);
bool vox_http_context_is_deferred(const vox_http_context_t* ctx);

/**
 * 触发发送当前 ctx 的响应（用于 defer 模式）
 * @return 成功返回0，失败返回-1
 */
int vox_http_context_finish(vox_http_context_t* ctx);

/* ===== request/response 访问 ===== */
const vox_http_request_t* vox_http_context_request(const vox_http_context_t* ctx);
vox_http_response_t* vox_http_context_response(vox_http_context_t* ctx);

/* ===== params ===== */
vox_strview_t vox_http_context_param(const vox_http_context_t* ctx, const char* name);

/* ===== request helpers ===== */
/**
 * 获取请求头值（大小写不敏感）
 * @param ctx HTTP上下文
 * @param name 请求头名称
 * @return 返回请求头值的视图，如果不存在返回空视图
 */
vox_strview_t vox_http_context_get_header(const vox_http_context_t* ctx, const char* name);

/**
 * 获取查询字符串参数值
 * @param ctx HTTP上下文
 * @param name 参数名称
 * @return 返回参数值的视图，如果不存在返回空视图
 */
vox_strview_t vox_http_context_get_query(const vox_http_context_t* ctx, const char* name);

/* ===== response builder ===== */
int vox_http_context_status(vox_http_context_t* ctx, int status);
int vox_http_context_header(vox_http_context_t* ctx, const char* name, const char* value);
int vox_http_context_write(vox_http_context_t* ctx, const void* data, size_t len);
int vox_http_context_write_cstr(vox_http_context_t* ctx, const char* cstr);

/**
 * 使用 sendfile 发送文件体（仅非 TLS 连接；TLS 时回退为读入 body）
 * 调用后不得关闭 file，由框架在发送完成后关闭。
 * @param ctx HTTP 上下文
 * @param file vox_file_open 打开的文件（只读），可为 NULL 表示不使用 sendfile
 * @param offset 文件起始偏移（字节）
 * @param count 要发送的字节数
 * @return 成功返回0，失败返回-1
 */
int vox_http_context_send_file(vox_http_context_t* ctx, void* file, int64_t offset, size_t count);

/* 构建 HTTP/1.x 响应报文到 out（包含 status line/headers/body） */
int vox_http_context_build_response(const vox_http_context_t* ctx, vox_string_t* out);

/* 用户数据（便于上层绑定业务对象） */
void* vox_http_context_get_user_data(const vox_http_context_t* ctx);
void vox_http_context_set_user_data(vox_http_context_t* ctx, void* user_data);

/* 便捷：获取 loop/mpool（用于 defer 场景分配状态对象） */
vox_loop_t* vox_http_context_get_loop(const vox_http_context_t* ctx);
vox_mpool_t* vox_http_context_get_mpool(const vox_http_context_t* ctx);

/** 获取当前请求所属的 engine（用于 vhost 等按 engine 区分上下文的场景）。调用方需包含 vox_http_engine.h 后将返回值转为 vox_http_engine_t*。 */
struct vox_http_engine* vox_http_context_get_engine(const vox_http_context_t* ctx);

#ifdef __cplusplus
}
#endif

#endif /* VOX_HTTP_CONTEXT_H */

