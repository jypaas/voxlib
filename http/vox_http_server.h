/*
 * vox_http_server.h - HTTP/HTTPS Server 入口
 */

#ifndef VOX_HTTP_SERVER_H
#define VOX_HTTP_SERVER_H

#include "../vox_os.h"
#include "../vox_socket.h"
#include "../vox_loop.h"
#include "../vox_tcp.h"
#include "../vox_tls.h"
#include "../ssl/vox_ssl.h"
#include "vox_http_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct vox_http_server vox_http_server_t;

vox_http_server_t* vox_http_server_create(vox_http_engine_t* engine);
void vox_http_server_destroy(vox_http_server_t* server);

/* 监听 HTTP */
int vox_http_server_listen_tcp(vox_http_server_t* server, const vox_socket_addr_t* addr, int backlog);

/* 监听 HTTPS（WSS 同理，通过 ws upgrade） */
int vox_http_server_listen_tls(vox_http_server_t* server, vox_ssl_context_t* ssl_ctx, const vox_socket_addr_t* addr, int backlog);

/* 停止并关闭所有连接 */
void vox_http_server_close(vox_http_server_t* server);

#ifdef __cplusplus
}
#endif

#endif /* VOX_HTTP_SERVER_H */

