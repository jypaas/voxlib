/*
 * https_server_example.c - 基本 HTTPS Server 示例
 * - 复用 vox_http_engine / vox_http_server（底层监听使用 vox_tls）
 *
 * 默认监听 0.0.0.0:8443，证书/私钥默认使用 cert/server.crt / cert/server.key
 */

#include "../vox_loop.h"
#include "../vox_socket.h"
#include "../vox_log.h"
#include "../ssl/vox_ssl.h"

#include "../http/vox_http_engine.h"
#include "../http/vox_http_server.h"
#include "../http/vox_http_context.h"

#include <stdio.h>
#include <string.h>

static void hello_handler(vox_http_context_t* ctx) {
    vox_http_context_status(ctx, 200);
    vox_http_context_write_cstr(ctx, "hello https\n");
}

int main(int argc, char** argv) {
    const char* cert_file = (argc > 1) ? argv[1] : "cert/server.crt";
    const char* key_file = (argc > 2) ? argv[2] : "cert/server.key";

    if (vox_socket_init() != 0) {
        fprintf(stderr, "vox_socket_init failed\n");
        return 1;
    }

    vox_log_set_level(VOX_LOG_INFO);

    vox_loop_t* loop = vox_loop_create();
    if (!loop) return 1;

    vox_mpool_t* mpool = vox_loop_get_mpool(loop);
    if (!mpool) return 1;

    vox_ssl_context_t* ssl_ctx = vox_ssl_context_create(mpool, VOX_SSL_MODE_SERVER);
    if (!ssl_ctx) {
        fprintf(stderr, "vox_ssl_context_create failed\n");
        return 1;
    }

    vox_ssl_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.cert_file = cert_file;
    cfg.key_file = key_file;
    if (vox_ssl_context_configure(ssl_ctx, &cfg) != 0) {
        fprintf(stderr, "vox_ssl_context_configure failed\n");
        return 1;
    }

    vox_http_engine_t* engine = vox_http_engine_create(loop);
    if (!engine) return 1;

    vox_http_handler_cb hs[] = { hello_handler };
    vox_http_engine_get(engine, "/hello", hs, sizeof(hs) / sizeof(hs[0]));

    vox_http_server_t* server = vox_http_server_create(engine);
    if (!server) return 1;

    vox_socket_addr_t addr;
    if (vox_socket_parse_address("0.0.0.0", 8443, &addr) != 0) {
        fprintf(stderr, "vox_socket_parse_address failed\n");
        return 1;
    }

    if (vox_http_server_listen_tls(server, ssl_ctx, &addr, 128) != 0) {
        fprintf(stderr, "listen tls failed\n");
        return 1;
    }

    VOX_LOG_INFO("HTTPS server listening on 0.0.0.0:8443");
    VOX_LOG_INFO("cert=%s key=%s", cert_file, key_file);
    return vox_loop_run(loop, VOX_RUN_DEFAULT);
}

