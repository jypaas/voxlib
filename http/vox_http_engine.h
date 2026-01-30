/*
 * vox_http_engine.h - HTTP Engine（类似 Gin Engine）
 * - 管理路由树与全局中间件
 * - 提供 group 前缀与组内中间件
 */

#ifndef VOX_HTTP_ENGINE_H
#define VOX_HTTP_ENGINE_H

#include "../vox_os.h"
#include "../vox_loop.h"
#include "../vox_mpool.h"
#include "../vox_vector.h"
#include "vox_http_router.h"
#include "vox_http_middleware.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct vox_http_engine vox_http_engine_t;
typedef struct vox_http_group vox_http_group_t;

vox_http_engine_t* vox_http_engine_create(vox_loop_t* loop);
void vox_http_engine_destroy(vox_http_engine_t* engine);

/* 全局中间件 */
int vox_http_engine_use(vox_http_engine_t* engine, vox_http_handler_cb handler);

/* 路由组 */
vox_http_group_t* vox_http_engine_group(vox_http_engine_t* engine, const char* prefix);
int vox_http_group_use(vox_http_group_t* group, vox_http_handler_cb handler);

/* 添加路由（handlers 为最终链，按注册顺序执行） */
int vox_http_engine_add_route(vox_http_engine_t* engine,
                              vox_http_method_t method,
                              const char* path,
                              vox_http_handler_cb* handlers,
                              size_t handler_count);

int vox_http_group_add_route(vox_http_group_t* group,
                             vox_http_method_t method,
                             const char* path,
                             vox_http_handler_cb* handlers,
                             size_t handler_count);

/* 便捷方法 */
int vox_http_engine_get(vox_http_engine_t* engine, const char* path, vox_http_handler_cb* handlers, size_t handler_count);
int vox_http_engine_post(vox_http_engine_t* engine, const char* path, vox_http_handler_cb* handlers, size_t handler_count);
int vox_http_group_get(vox_http_group_t* group, const char* path, vox_http_handler_cb* handlers, size_t handler_count);
int vox_http_group_post(vox_http_group_t* group, const char* path, vox_http_handler_cb* handlers, size_t handler_count);

/* 内部：访问 router 与全局 middleware（server 模块会用到） */
vox_http_router_t* vox_http_engine_get_router(vox_http_engine_t* engine);
vox_vector_t* vox_http_engine_get_global_middleware(vox_http_engine_t* engine);
vox_mpool_t* vox_http_engine_get_mpool(vox_http_engine_t* engine);
vox_loop_t* vox_http_engine_get_loop(vox_http_engine_t* engine);

#ifdef __cplusplus
}
#endif

#endif /* VOX_HTTP_ENGINE_H */

