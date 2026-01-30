/*
 * vox_ssl_openssl.h - OpenSSL Memory BIO 实现
 * 使用 Memory BIO (rbio/wbio) 实现跨平台 TLS
 */

#ifndef VOX_SSL_OPENSSL_H
#define VOX_SSL_OPENSSL_H

#include "vox_ssl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* OpenSSL 内部实现函数（供 vox_ssl.c 调用） */

/* Context API */
vox_ssl_context_t* vox_ssl_openssl_context_create(vox_mpool_t* mpool, vox_ssl_mode_t mode);
void vox_ssl_openssl_context_destroy(vox_ssl_context_t* ctx);
int vox_ssl_openssl_context_configure(vox_ssl_context_t* ctx, const vox_ssl_config_t* config);

/* Session API */
vox_ssl_session_t* vox_ssl_openssl_session_create(vox_ssl_context_t* ctx, vox_mpool_t* mpool);
void vox_ssl_openssl_session_destroy(vox_ssl_session_t* session);
void* vox_ssl_openssl_session_get_rbio(vox_ssl_session_t* session);
void* vox_ssl_openssl_session_get_wbio(vox_ssl_session_t* session);
int vox_ssl_openssl_session_handshake(vox_ssl_session_t* session);
ssize_t vox_ssl_openssl_session_read(vox_ssl_session_t* session, void* buf, size_t len);
ssize_t vox_ssl_openssl_session_write(vox_ssl_session_t* session, const void* buf, size_t len);
int vox_ssl_openssl_session_shutdown(vox_ssl_session_t* session);
vox_ssl_state_t vox_ssl_openssl_session_get_state(vox_ssl_session_t* session);
vox_ssl_error_t vox_ssl_openssl_session_get_error(vox_ssl_session_t* session);
int vox_ssl_openssl_session_get_error_string(vox_ssl_session_t* session, char* buf, size_t len);

/* BIO 操作 API */
size_t vox_ssl_openssl_bio_pending(vox_ssl_session_t* session, vox_ssl_bio_type_t bio_type);
ssize_t vox_ssl_openssl_bio_read(vox_ssl_session_t* session, vox_ssl_bio_type_t bio_type, void* buf, size_t len);
ssize_t vox_ssl_openssl_bio_write(vox_ssl_session_t* session, vox_ssl_bio_type_t bio_type, const void* buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* VOX_SSL_OPENSSL_H */
