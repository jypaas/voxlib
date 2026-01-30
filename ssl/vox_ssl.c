/*
 * vox_ssl.c - SSL/TLS Backend 抽象层实现
 * 根据编译选项选择具体的 SSL 实现（OpenSSL/WolfSSL/mbedTLS）
 */

#include "vox_ssl.h"
#include "../vox_log.h"
#include "../vox_os.h"

/* 根据编译选项包含具体的实现 */
#if defined(VOX_USE_OPENSSL)
    #include "vox_ssl_openssl.h"
#elif defined(VOX_USE_WOLFSSL)
    #include "vox_ssl_wolfssl.h"
#elif defined(VOX_USE_MBEDTLS)
    #include "vox_ssl_mbedtls.h"
#else
    #error "No SSL library selected. Please define VOX_USE_OPENSSL, VOX_USE_WOLFSSL, or VOX_USE_MBEDTLS"
#endif

/* ===== SSL Context API ===== */

vox_ssl_context_t* vox_ssl_context_create(vox_mpool_t* mpool, vox_ssl_mode_t mode) {
#if defined(VOX_USE_OPENSSL)
    return vox_ssl_openssl_context_create(mpool, mode);
#elif defined(VOX_USE_WOLFSSL)
    return vox_ssl_wolfssl_context_create(mpool, mode);
#elif defined(VOX_USE_MBEDTLS)
    return vox_ssl_mbedtls_context_create(mpool, mode);
#else
    (void)mpool;
    (void)mode;
    VOX_LOG_ERROR("No SSL library available");
    return NULL;
#endif
}

void vox_ssl_context_destroy(vox_ssl_context_t* ctx) {
    if (!ctx) {
        return;
    }
#if defined(VOX_USE_OPENSSL)
    vox_ssl_openssl_context_destroy(ctx);
#elif defined(VOX_USE_WOLFSSL)
    vox_ssl_wolfssl_context_destroy(ctx);
#elif defined(VOX_USE_MBEDTLS)
    vox_ssl_mbedtls_context_destroy(ctx);
#endif
}

int vox_ssl_context_configure(vox_ssl_context_t* ctx, const vox_ssl_config_t* config) {
    if (!ctx || !config) {
        return -1;
    }
#if defined(VOX_USE_OPENSSL)
    return vox_ssl_openssl_context_configure(ctx, config);
#elif defined(VOX_USE_WOLFSSL)
    return vox_ssl_wolfssl_context_configure(ctx, config);
#elif defined(VOX_USE_MBEDTLS)
    return vox_ssl_mbedtls_context_configure(ctx, config);
#else
    (void)ctx;
    (void)config;
    return -1;
#endif
}

/* ===== SSL Session API ===== */

vox_ssl_session_t* vox_ssl_session_create(vox_ssl_context_t* ctx, vox_mpool_t* mpool) {
    if (!ctx) {
        return NULL;
    }
#if defined(VOX_USE_OPENSSL)
    return vox_ssl_openssl_session_create(ctx, mpool);
#elif defined(VOX_USE_WOLFSSL)
    return vox_ssl_wolfssl_session_create(ctx, mpool);
#elif defined(VOX_USE_MBEDTLS)
    return vox_ssl_mbedtls_session_create(ctx, mpool);
#else
    (void)ctx;
    (void)mpool;
    return NULL;
#endif
}

void vox_ssl_session_destroy(vox_ssl_session_t* session) {
    if (!session) {
        return;
    }
#if defined(VOX_USE_OPENSSL)
    vox_ssl_openssl_session_destroy(session);
#elif defined(VOX_USE_WOLFSSL)
    vox_ssl_wolfssl_session_destroy(session);
#elif defined(VOX_USE_MBEDTLS)
    vox_ssl_mbedtls_session_destroy(session);
#endif
}

void* vox_ssl_session_get_rbio(vox_ssl_session_t* session) {
    if (!session) {
        return NULL;
    }
#if defined(VOX_USE_OPENSSL)
    return vox_ssl_openssl_session_get_rbio(session);
#elif defined(VOX_USE_WOLFSSL)
    return vox_ssl_wolfssl_session_get_rbio(session);
#elif defined(VOX_USE_MBEDTLS)
    return vox_ssl_mbedtls_session_get_rbio(session);
#else
    (void)session;
    return NULL;
#endif
}

void* vox_ssl_session_get_wbio(vox_ssl_session_t* session) {
    if (!session) {
        return NULL;
    }
#if defined(VOX_USE_OPENSSL)
    return vox_ssl_openssl_session_get_wbio(session);
#elif defined(VOX_USE_WOLFSSL)
    return vox_ssl_wolfssl_session_get_wbio(session);
#elif defined(VOX_USE_MBEDTLS)
    return vox_ssl_mbedtls_session_get_wbio(session);
#else
    (void)session;
    return NULL;
#endif
}

int vox_ssl_session_handshake(vox_ssl_session_t* session) {
    if (!session) {
        return -1;
    }
#if defined(VOX_USE_OPENSSL)
    return vox_ssl_openssl_session_handshake(session);
#elif defined(VOX_USE_WOLFSSL)
    return vox_ssl_wolfssl_session_handshake(session);
#elif defined(VOX_USE_MBEDTLS)
    return vox_ssl_mbedtls_session_handshake(session);
#else
    (void)session;
    return -1;
#endif
}

ssize_t vox_ssl_session_read(vox_ssl_session_t* session, void* buf, size_t len) {
    if (!session || !buf || len == 0) {
        return -1;
    }
#if defined(VOX_USE_OPENSSL)
    return vox_ssl_openssl_session_read(session, buf, len);
#elif defined(VOX_USE_WOLFSSL)
    return vox_ssl_wolfssl_session_read(session, buf, len);
#elif defined(VOX_USE_MBEDTLS)
    return vox_ssl_mbedtls_session_read(session, buf, len);
#else
    (void)session;
    (void)buf;
    (void)len;
    return -1;
#endif
}

ssize_t vox_ssl_session_write(vox_ssl_session_t* session, const void* buf, size_t len) {
    if (!session || !buf || len == 0) {
        return -1;
    }
#if defined(VOX_USE_OPENSSL)
    return vox_ssl_openssl_session_write(session, buf, len);
#elif defined(VOX_USE_WOLFSSL)
    return vox_ssl_wolfssl_session_write(session, buf, len);
#elif defined(VOX_USE_MBEDTLS)
    return vox_ssl_mbedtls_session_write(session, buf, len);
#else
    (void)session;
    (void)buf;
    (void)len;
    return -1;
#endif
}

int vox_ssl_session_shutdown(vox_ssl_session_t* session) {
    if (!session) {
        return -1;
    }
#if defined(VOX_USE_OPENSSL)
    return vox_ssl_openssl_session_shutdown(session);
#elif defined(VOX_USE_WOLFSSL)
    return vox_ssl_wolfssl_session_shutdown(session);
#elif defined(VOX_USE_MBEDTLS)
    return vox_ssl_mbedtls_session_shutdown(session);
#else
    (void)session;
    return -1;
#endif
}

vox_ssl_state_t vox_ssl_session_get_state(vox_ssl_session_t* session) {
    if (!session) {
        return VOX_SSL_STATE_CLOSED;
    }
#if defined(VOX_USE_OPENSSL)
    return vox_ssl_openssl_session_get_state(session);
#elif defined(VOX_USE_WOLFSSL)
    return vox_ssl_wolfssl_session_get_state(session);
#elif defined(VOX_USE_MBEDTLS)
    return vox_ssl_mbedtls_session_get_state(session);
#else
    (void)session;
    return VOX_SSL_STATE_CLOSED;
#endif
}

vox_ssl_error_t vox_ssl_session_get_error(vox_ssl_session_t* session) {
    if (!session) {
        return VOX_SSL_ERROR_INVALID_STATE;
    }
#if defined(VOX_USE_OPENSSL)
    return vox_ssl_openssl_session_get_error(session);
#elif defined(VOX_USE_WOLFSSL)
    return vox_ssl_wolfssl_session_get_error(session);
#elif defined(VOX_USE_MBEDTLS)
    return vox_ssl_mbedtls_session_get_error(session);
#else
    (void)session;
    return VOX_SSL_ERROR_INVALID_STATE;
#endif
}

int vox_ssl_session_get_error_string(vox_ssl_session_t* session, char* buf, size_t len) {
    if (!session || !buf || len == 0) {
        return -1;
    }
#if defined(VOX_USE_OPENSSL)
    return vox_ssl_openssl_session_get_error_string(session, buf, len);
#elif defined(VOX_USE_WOLFSSL)
    return vox_ssl_wolfssl_session_get_error_string(session, buf, len);
#elif defined(VOX_USE_MBEDTLS)
    return vox_ssl_mbedtls_session_get_error_string(session, buf, len);
#else
    (void)session;
    (void)buf;
    (void)len;
    return -1;
#endif
}

/* ===== BIO 操作 API ===== */

size_t vox_ssl_bio_pending(vox_ssl_session_t* session, vox_ssl_bio_type_t bio_type) {
    if (!session) {
        return 0;
    }
#if defined(VOX_USE_OPENSSL)
    return vox_ssl_openssl_bio_pending(session, bio_type);
#elif defined(VOX_USE_WOLFSSL)
    return vox_ssl_wolfssl_bio_pending(session, bio_type);
#elif defined(VOX_USE_MBEDTLS)
    return vox_ssl_mbedtls_bio_pending(session, bio_type);
#else
    (void)session;
    (void)bio_type;
    return 0;
#endif
}

ssize_t vox_ssl_bio_read(vox_ssl_session_t* session, vox_ssl_bio_type_t bio_type, void* buf, size_t len) {
    if (!session || !buf || len == 0) {
        return -1;
    }
#if defined(VOX_USE_OPENSSL)
    return vox_ssl_openssl_bio_read(session, bio_type, buf, len);
#elif defined(VOX_USE_WOLFSSL)
    return vox_ssl_wolfssl_bio_read(session, bio_type, buf, len);
#elif defined(VOX_USE_MBEDTLS)
    return vox_ssl_mbedtls_bio_read(session, bio_type, buf, len);
#else
    (void)session;
    (void)bio_type;
    (void)buf;
    (void)len;
    return -1;
#endif
}

ssize_t vox_ssl_bio_write(vox_ssl_session_t* session, vox_ssl_bio_type_t bio_type, const void* buf, size_t len) {
    if (!session || !buf || len == 0) {
        return -1;
    }
#if defined(VOX_USE_OPENSSL)
    return vox_ssl_openssl_bio_write(session, bio_type, buf, len);
#elif defined(VOX_USE_WOLFSSL)
    return vox_ssl_wolfssl_bio_write(session, bio_type, buf, len);
#elif defined(VOX_USE_MBEDTLS)
    return vox_ssl_mbedtls_bio_write(session, bio_type, buf, len);
#else
    (void)session;
    (void)bio_type;
    (void)buf;
    (void)len;
    return -1;
#endif
}
