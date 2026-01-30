/*
 * vox_http_router.h - 高性能路由
 * 目标：支持精确匹配 + :param 参数 + group 前缀（通过上层在注册时拼接）
 */

#ifndef VOX_HTTP_ROUTER_H
#define VOX_HTTP_ROUTER_H

#include "../vox_os.h"
#include "../vox_mpool.h"
#include "../vox_string.h"
#include "vox_http_parser.h"
#include "vox_http_middleware.h"
#include "vox_http_context.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct vox_http_router vox_http_router_t;

typedef struct {
    vox_http_handler_cb* handlers;
    size_t handler_count;
    vox_http_param_t* params;
    size_t param_count;
} vox_http_route_match_t;

vox_http_router_t* vox_http_router_create(vox_mpool_t* mpool);
void vox_http_router_destroy(vox_http_router_t* router);

/* 注册路由：path 支持静态与 :param（不支持 *wildcard） */
int vox_http_router_add(vox_http_router_t* router,
                        vox_http_method_t method,
                        const char* path,
                        vox_http_handler_cb* handlers,
                        size_t handler_count);

/* 匹配路由：path 必须是纯 path（不含 query） */
int vox_http_router_match(vox_http_router_t* router,
                          vox_http_method_t method,
                          const char* path,
                          size_t path_len,
                          vox_mpool_t* mpool,
                          vox_http_route_match_t* out);

#ifdef __cplusplus
}
#endif

#endif /* VOX_HTTP_ROUTER_H */

