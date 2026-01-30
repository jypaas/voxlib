/*
 * vox_ssl_openssl.c - OpenSSL Memory BIO 实现
 * 使用 Memory BIO (rbio/wbio) 实现跨平台 TLS
 */

#include "vox_ssl_openssl.h"
#include "../vox_log.h"
#include "../vox_mpool.h"
#include <string.h>

#ifdef VOX_USE_OPENSSL
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/bio.h>
#include <openssl/x509.h>
#include <openssl/pem.h>

/* OpenSSL Context 结构 */
struct vox_ssl_context {
    SSL_CTX* ctx;                /* OpenSSL context */
    vox_ssl_mode_t mode;         /* 客户端或服务器模式 */
    vox_mpool_t* mpool;          /* 内存池 */
    bool is_dtls;                /* 是否使用 DTLS（而不是 TLS） */
    int dtls_mtu;                /* DTLS 应用层 MTU（字节），0 表示使用默认值 */
};

/* OpenSSL Session 结构 */
struct vox_ssl_session {
    SSL* ssl;                    /* OpenSSL SSL 对象 */
    BIO* rbio;                   /* 读取 BIO（从 socket 读取后写入） */
    BIO* wbio;                   /* 写入 BIO（从 SSL 读取后写入 socket） */
    vox_ssl_context_t* ctx;      /* 关联的 context */
    vox_ssl_state_t state;       /* SSL 状态 */
    vox_ssl_error_t last_error;  /* 最后的错误码 */
    vox_mpool_t* mpool;          /* 内存池 */
};

/* 将 OpenSSL 错误码转换为 vox_ssl_error_t */
static vox_ssl_error_t openssl_error_to_vox_error(int ssl_error) {
    switch (ssl_error) {
        case SSL_ERROR_NONE:
            return VOX_SSL_ERROR_NONE;
        case SSL_ERROR_WANT_READ:
            return VOX_SSL_ERROR_WANT_READ;
        case SSL_ERROR_WANT_WRITE:
            return VOX_SSL_ERROR_WANT_WRITE;
        case SSL_ERROR_SYSCALL:
            return VOX_SSL_ERROR_SYSCALL;
        case SSL_ERROR_SSL:
            return VOX_SSL_ERROR_SSL;
        case SSL_ERROR_ZERO_RETURN:
            return VOX_SSL_ERROR_ZERO_RETURN;
        default:
            return VOX_SSL_ERROR_SSL;
    }
}

/* ===== Context API ===== */

vox_ssl_context_t* vox_ssl_openssl_context_create(vox_mpool_t* mpool, vox_ssl_mode_t mode) {
    if (!mpool) {
        return NULL;
    }

    /* 分配 context 结构（使用内存池） */
    size_t ctx_size = sizeof(struct vox_ssl_context);
    vox_ssl_context_t* ctx = (vox_ssl_context_t*)vox_mpool_alloc(mpool, ctx_size);
    if (!ctx) {
        return NULL;
    }

    memset(ctx, 0, ctx_size);
    ctx->mode = mode;
    ctx->mpool = mpool;
    ctx->is_dtls = false;  /* 默认为 TLS，需要通过配置来设置 DTLS */
    ctx->dtls_mtu = 0;     /* 0 表示使用默认值 1440 */

    /* 创建 OpenSSL context */
    /* 注意：默认使用 TLS 方法，DTLS 需要通过 vox_ssl_openssl_context_configure_dtls 来设置 */
    const SSL_METHOD* method = (mode == VOX_SSL_MODE_SERVER) ?
        TLS_server_method() : TLS_client_method();
    
    ctx->ctx = SSL_CTX_new(method);
    if (!ctx->ctx) {
        VOX_LOG_ERROR("Failed to create SSL_CTX");
        vox_mpool_free(mpool, ctx);
        return NULL;
    }

    /* 设置默认选项 */
    SSL_CTX_set_options(ctx->ctx, SSL_OP_ALL | SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3);

    return ctx;
}

void vox_ssl_openssl_context_destroy(vox_ssl_context_t* ctx) {
    if (!ctx) {
        return;
    }

    if (ctx->ctx) {
        SSL_CTX_free(ctx->ctx);
        ctx->ctx = NULL;
    }

    vox_mpool_t* mpool = ctx->mpool;
    vox_mpool_free(mpool, ctx);
}

/* 配置 context 为 DTLS 模式（必须在 configure 之前调用） */
static int vox_ssl_openssl_context_configure_dtls(vox_ssl_context_t* ctx) {
    if (!ctx || !ctx->ctx) {
        return -1;
    }

    /* 如果已经是 DTLS，无需重新配置 */
    if (ctx->is_dtls) {
        return 0;
    }

    /* 保存旧的 context */
    SSL_CTX* old_ctx = ctx->ctx;

    /* 创建新的 DTLS context */
    const SSL_METHOD* method = (ctx->mode == VOX_SSL_MODE_SERVER) ?
        DTLS_server_method() : DTLS_client_method();
    
    ctx->ctx = SSL_CTX_new(method);
    if (!ctx->ctx) {
        VOX_LOG_ERROR("Failed to create DTLS SSL_CTX");
        ctx->ctx = old_ctx;  /* 恢复旧的 context */
        return -1;
    }

    /* 设置默认选项 */
    SSL_CTX_set_options(ctx->ctx, SSL_OP_ALL | SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3);
    
    /* 为 DTLS 设置额外的选项 */
    /* 禁用自动查询 MTU（推荐手动设置） */
    SSL_CTX_set_options(ctx->ctx, SSL_OP_NO_QUERY_MTU);

    /* 释放旧的 context */
    SSL_CTX_free(old_ctx);

    /* 标记为 DTLS */
    ctx->is_dtls = true;

    return 0;
}

int vox_ssl_openssl_context_configure(vox_ssl_context_t* ctx, const vox_ssl_config_t* config) {
    if (!ctx || !config) {
        return -1;
    }

    /* 检查是否需要配置为 DTLS（通过检查 protocols 字段是否包含 "DTLS"） */
    if (config->protocols && strstr(config->protocols, "DTLS") != NULL) {
        if (vox_ssl_openssl_context_configure_dtls(ctx) != 0) {
            VOX_LOG_ERROR("Failed to configure DTLS context");
            return -1;
        }
    }

    /* 服务器模式：加载证书和私钥 */
    if (ctx->mode == VOX_SSL_MODE_SERVER) {
        if (config->cert_file && config->key_file) {
            if (SSL_CTX_use_certificate_file(ctx->ctx, config->cert_file, SSL_FILETYPE_PEM) != 1) {
                VOX_LOG_ERROR("Failed to load certificate file: %s", config->cert_file);
                return -1;
            }

            if (SSL_CTX_use_PrivateKey_file(ctx->ctx, config->key_file, SSL_FILETYPE_PEM) != 1) {
                VOX_LOG_ERROR("Failed to load private key file: %s", config->key_file);
                return -1;
            }

            /* 验证私钥与证书匹配 */
            if (SSL_CTX_check_private_key(ctx->ctx) != 1) {
                VOX_LOG_ERROR("Private key does not match certificate");
                return -1;
            }
        }
    }

    /* 客户端模式：加载 CA 证书 */
    if (ctx->mode == VOX_SSL_MODE_CLIENT) {
        if (config->ca_file || config->ca_path) {
            if (SSL_CTX_load_verify_locations(ctx->ctx, config->ca_file, config->ca_path) != 1) {
                VOX_LOG_ERROR("Failed to load CA certificates");
                /* 不返回错误，允许继续（某些情况下可能不需要验证） */
            }
        }

        /* 设置验证模式 */
        if (config->verify_peer) {
            SSL_CTX_set_verify(ctx->ctx, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, NULL);
        } else {
            SSL_CTX_set_verify(ctx->ctx, SSL_VERIFY_NONE, NULL);
        }
    }

    /* 设置密码套件 */
    if (config->ciphers) {
        if (SSL_CTX_set_cipher_list(ctx->ctx, config->ciphers) != 1) {
            VOX_LOG_ERROR("Failed to set cipher list");
            return -1;
        }
    }

    /* 设置 DTLS MTU（如果指定） */
    if (ctx->is_dtls && config->dtls_mtu > 0) {
        /* 验证 MTU 值的合理性 */
        if (config->dtls_mtu < 512) {
            VOX_LOG_ERROR("DTLS MTU too small: %d (minimum 512)", config->dtls_mtu);
            return -1;
        }
        if (config->dtls_mtu > 1500) {
            VOX_LOG_ERROR("DTLS MTU too large: %d (maximum 1500 for standard Ethernet)", config->dtls_mtu);
            return -1;
        }
        ctx->dtls_mtu = config->dtls_mtu;
    }

    return 0;
}

/* ===== Session API ===== */

vox_ssl_session_t* vox_ssl_openssl_session_create(vox_ssl_context_t* ctx, vox_mpool_t* mpool) {
    if (!ctx || !mpool) {
        return NULL;
    }

    /* 检查 ctx->ctx 是否有效 */
    if (!ctx->ctx) {
        VOX_LOG_ERROR("vox_ssl_openssl_session_create: ctx->ctx is NULL");
        return NULL;
    }

    /* 分配 session 结构（使用内存池） */
    size_t session_size = sizeof(struct vox_ssl_session);
    vox_ssl_session_t* session = (vox_ssl_session_t*)vox_mpool_alloc(mpool, session_size);
    if (!session) {
        VOX_LOG_ERROR("vox_ssl_openssl_session_create: failed to allocate session structure (size=%zu)", session_size);
        return NULL;
    }

    memset(session, 0, session_size);
    session->ctx = ctx;
    session->mpool = mpool;
    session->state = VOX_SSL_STATE_INIT;
    session->last_error = VOX_SSL_ERROR_NONE;

    /* 创建 Memory BIO */
    session->rbio = BIO_new(BIO_s_mem());
    if (!session->rbio) {
        VOX_LOG_ERROR("vox_ssl_openssl_session_create: failed to create rbio");
        vox_mpool_free(mpool, session);
        return NULL;
    }
    
    session->wbio = BIO_new(BIO_s_mem());
    if (!session->wbio) {
        VOX_LOG_ERROR("vox_ssl_openssl_session_create: failed to create wbio");
        BIO_free(session->rbio);
        vox_mpool_free(mpool, session);
        return NULL;
    }

    /* 创建 SSL 对象 */
    session->ssl = SSL_new(ctx->ctx);
    if (!session->ssl) {
        VOX_LOG_ERROR("vox_ssl_openssl_session_create: failed to create SSL object");
        BIO_free(session->rbio);
        BIO_free(session->wbio);
        vox_mpool_free(mpool, session);
        return NULL;
    }

    /* 将 BIO 关联到 SSL 对象 */
    SSL_set_bio(session->ssl, session->rbio, session->wbio);

    /* 如果是 DTLS，设置 DTLS 特定的选项 */
    if (ctx->is_dtls) {
        /* 禁用预读（UDP 无流特性） */
        SSL_set_read_ahead(session->ssl, 0);
        
        /* 禁用自动查询 MTU（推荐手动设置） */
        SSL_set_options(session->ssl, SSL_OP_NO_QUERY_MTU);
        
        /* 计算 MTU 值
         * 以太网标准 MTU: 1500 字节
         * IP 头: 20 字节（IPv4）或 40 字节（IPv6）
         * UDP 头: 8 字节
         * DTLS 头: 约 13-29 字节（取决于记录类型）
         * 
         * 理论最大值：
         * - IPv4: 1500 - 20 - 8 - 29 ≈ 1443 字节（应用层）
         * - IPv6: 1500 - 40 - 8 - 29 ≈ 1423 字节（应用层）
         * 
         * 默认值：1440 字节（IPv4，留一些余量）
         * 链路层 MTU = 应用层 MTU + IP头 + UDP头 + DTLS头 ≈ 应用层 MTU + 60
         */
        int app_mtu = ctx->dtls_mtu > 0 ? ctx->dtls_mtu : 1440;  /* 使用配置值或默认值 */
        int link_mtu = 1500; /* 标准以太网 MTU */
        
        /* 如果应用层 MTU 较大，相应调整链路层 MTU
         * 但不超过标准以太网 MTU 1500
         */
        if (app_mtu + 60 > link_mtu) {
            link_mtu = app_mtu + 60;
            if (link_mtu > 1500) {
                link_mtu = 1500;
                /* 如果链路层 MTU 被限制为 1500，相应调整应用层 MTU */
                app_mtu = link_mtu - 60;
            }
        }
        
        /* 设置应用层 MTU（不包括 DTLS 头） */
        SSL_set_mtu(session->ssl, app_mtu);
        
        /* DTLS_set_link_mtu 设置链路层 MTU（包括所有头）
         * 注意：这个函数在 OpenSSL 1.1.0+ 中可用
         * 如果编译时链接的 OpenSSL 版本不支持，链接器会报错
         * 为了兼容旧版本，我们使用条件编译
         */
        #if OPENSSL_VERSION_NUMBER >= 0x10100000L
        /* OpenSSL 1.1.0+ 支持 DTLS_set_link_mtu */
        DTLS_set_link_mtu(session->ssl, link_mtu);
        #elif defined(OPENSSL_VERSION_NUMBER) && OPENSSL_VERSION_NUMBER >= 0x10001000L
        /* OpenSSL 1.0.1+ 可能也支持，尝试调用 */
        /* 注意：如果链接时不存在，链接器会报错，需要用户升级 OpenSSL */
        DTLS_set_link_mtu(session->ssl, link_mtu);
        #endif
        /* 如果 DTLS_set_link_mtu 不可用，只使用 SSL_set_mtu 也可以工作
         * OpenSSL 会根据 SSL_set_mtu 的值自动计算链路层 MTU
         */
    }

    /* 根据模式设置 SSL 状态 */
    if (ctx->mode == VOX_SSL_MODE_SERVER) {
        SSL_set_accept_state(session->ssl);
    } else {
        SSL_set_connect_state(session->ssl);
    }

    return session;
}

void vox_ssl_openssl_session_destroy(vox_ssl_session_t* session) {
    if (!session) {
        return;
    }

    if (session->ssl) {
        /* SSL_free 会自动释放关联的 BIO */
        SSL_free(session->ssl);
        session->ssl = NULL;
        session->rbio = NULL;  /* 已被 SSL_free 释放 */
        session->wbio = NULL;  /* 已被 SSL_free 释放 */
    } else {
        /* 如果 SSL 对象创建失败，需要手动释放 BIO */
        if (session->rbio) {
            BIO_free(session->rbio);
        }
        if (session->wbio) {
            BIO_free(session->wbio);
        }
    }

    vox_mpool_t* mpool = session->mpool;
    vox_mpool_free(mpool, session);
}

void* vox_ssl_openssl_session_get_rbio(vox_ssl_session_t* session) {
    if (!session) {
        return NULL;
    }
    return (void*)session->rbio;
}

void* vox_ssl_openssl_session_get_wbio(vox_ssl_session_t* session) {
    if (!session) {
        return NULL;
    }
    return (void*)session->wbio;
}

int vox_ssl_openssl_session_handshake(vox_ssl_session_t* session) {
    if (!session || !session->ssl) {
        return -1;
    }

    int ret = SSL_do_handshake(session->ssl);
    
    if (ret == 1) {
        /* 握手成功 */
        session->state = VOX_SSL_STATE_CONNECTED;
        session->last_error = VOX_SSL_ERROR_NONE;
        return 0;
    }

    /* 握手未完成，检查错误 */
    int ssl_error = SSL_get_error(session->ssl, ret);
    session->last_error = openssl_error_to_vox_error(ssl_error);

    if (ssl_error == SSL_ERROR_WANT_READ || ssl_error == SSL_ERROR_WANT_WRITE) {
        /* 需要更多数据，这是正常的 */
        session->state = VOX_SSL_STATE_HANDSHAKING;
        return (ssl_error == SSL_ERROR_WANT_READ) ? VOX_SSL_ERROR_WANT_READ : VOX_SSL_ERROR_WANT_WRITE;
    }

    /* 其他错误 */
    session->state = VOX_SSL_STATE_CLOSED;
    return -1;
}

ssize_t vox_ssl_openssl_session_read(vox_ssl_session_t* session, void* buf, size_t len) {
    if (!session || !session->ssl || !buf || len == 0) {
        return -1;
    }

    int ret = SSL_read(session->ssl, buf, (int)len);

    if (ret > 0) {
        /* 成功读取数据 */
        session->last_error = VOX_SSL_ERROR_NONE;
        return (ssize_t)ret;
    }

    /* 检查错误 */
    int ssl_error = SSL_get_error(session->ssl, ret);
    session->last_error = openssl_error_to_vox_error(ssl_error);

    if (ssl_error == SSL_ERROR_WANT_READ || ssl_error == SSL_ERROR_WANT_WRITE) {
        /* 需要更多数据 */
        return (ssl_error == SSL_ERROR_WANT_READ) ? VOX_SSL_ERROR_WANT_READ : VOX_SSL_ERROR_WANT_WRITE;
    }

    if (ssl_error == SSL_ERROR_ZERO_RETURN) {
        /* 连接关闭 */
        session->state = VOX_SSL_STATE_CLOSED;
        return 0;
    }

    /* 其他错误 */
    return -1;
}

ssize_t vox_ssl_openssl_session_write(vox_ssl_session_t* session, const void* buf, size_t len) {
    if (!session || !session->ssl || !buf || len == 0) {
        return -1;
    }

    int ret = SSL_write(session->ssl, buf, (int)len);

    if (ret > 0) {
        /* 成功写入数据 */
        session->last_error = VOX_SSL_ERROR_NONE;
        return (ssize_t)ret;
    }

    /* 检查错误 */
    int ssl_error = SSL_get_error(session->ssl, ret);
    session->last_error = openssl_error_to_vox_error(ssl_error);

    if (ssl_error == SSL_ERROR_WANT_READ || ssl_error == SSL_ERROR_WANT_WRITE) {
        /* 需要更多数据 */
        return (ssl_error == SSL_ERROR_WANT_READ) ? VOX_SSL_ERROR_WANT_READ : VOX_SSL_ERROR_WANT_WRITE;
    }

    /* 其他错误 */
    return -1;
}

int vox_ssl_openssl_session_shutdown(vox_ssl_session_t* session) {
    if (!session || !session->ssl) {
        return -1;
    }

    int ret = SSL_shutdown(session->ssl);
    
    if (ret == 1) {
        /* 关闭完成 */
        session->state = VOX_SSL_STATE_CLOSED;
        session->last_error = VOX_SSL_ERROR_NONE;
        return 0;
    }

    /* 检查错误 */
    int ssl_error = SSL_get_error(session->ssl, ret);
    session->last_error = openssl_error_to_vox_error(ssl_error);

    if (ssl_error == SSL_ERROR_WANT_READ || ssl_error == SSL_ERROR_WANT_WRITE) {
        /* 需要更多数据来完成关闭 */
        return (ssl_error == SSL_ERROR_WANT_READ) ? VOX_SSL_ERROR_WANT_READ : VOX_SSL_ERROR_WANT_WRITE;
    }

    /* 其他错误 */
    return -1;
}

vox_ssl_state_t vox_ssl_openssl_session_get_state(vox_ssl_session_t* session) {
    if (!session) {
        return VOX_SSL_STATE_CLOSED;
    }
    return session->state;
}

vox_ssl_error_t vox_ssl_openssl_session_get_error(vox_ssl_session_t* session) {
    if (!session) {
        return VOX_SSL_ERROR_INVALID_STATE;
    }
    return session->last_error;
}

int vox_ssl_openssl_session_get_error_string(vox_ssl_session_t* session, char* buf, size_t len) {
    if (!session || !buf || len == 0) {
        return -1;
    }

    unsigned long err = ERR_get_error();
    if (err == 0) {
        /* 没有错误 */
        buf[0] = '\0';
        return 0;
    }

    /* 获取错误字符串 */
    char err_buf[256];
    ERR_error_string_n(err, err_buf, sizeof(err_buf));
    
    size_t err_len = strlen(err_buf);
    if (err_len >= len) {
        err_len = len - 1;
    }
    memcpy(buf, err_buf, err_len);
    buf[err_len] = '\0';

    return (int)err_len;
}

/* ===== BIO 操作 API ===== */

size_t vox_ssl_openssl_bio_pending(vox_ssl_session_t* session, vox_ssl_bio_type_t bio_type) {
    if (!session) {
        return 0;
    }

#ifdef VOX_USE_OPENSSL
    BIO* bio = (bio_type == VOX_SSL_BIO_RBIO) ? session->rbio : session->wbio;
    if (!bio) {
        return 0;
    }

    return (size_t)BIO_pending(bio);
#else
    (void)session;
    (void)bio_type;
    return 0;
#endif
}

ssize_t vox_ssl_openssl_bio_read(vox_ssl_session_t* session, vox_ssl_bio_type_t bio_type, void* buf, size_t len) {
    if (!session || !buf || len == 0) {
        return -1;
    }

#ifdef VOX_USE_OPENSSL
    BIO* bio = (bio_type == VOX_SSL_BIO_RBIO) ? session->rbio : session->wbio;
    if (!bio) {
        return -1;
    }

    int ret = BIO_read(bio, buf, (int)len);
    if (ret < 0) {
        return -1;
    }

    return (ssize_t)ret;
#else
    (void)session;
    (void)bio_type;
    (void)buf;
    (void)len;
    return -1;
#endif
}

ssize_t vox_ssl_openssl_bio_write(vox_ssl_session_t* session, vox_ssl_bio_type_t bio_type, const void* buf, size_t len) {
    if (!session || !buf || len == 0) {
        return -1;
    }

#ifdef VOX_USE_OPENSSL
    BIO* bio = (bio_type == VOX_SSL_BIO_RBIO) ? session->rbio : session->wbio;
    if (!bio) {
        return -1;
    }

    int ret = BIO_write(bio, buf, (int)len);
    if (ret < 0) {
        return -1;
    }

    return (ssize_t)ret;
#else
    (void)session;
    (void)bio_type;
    (void)buf;
    (void)len;
    return -1;
#endif
}

#else /* VOX_USE_OPENSSL */

/* 如果没有启用 OpenSSL，提供空实现 */
vox_ssl_context_t* vox_ssl_openssl_context_create(vox_mpool_t* mpool, vox_ssl_mode_t mode) {
    (void)mpool;
    (void)mode;
    return NULL;
}

void vox_ssl_openssl_context_destroy(vox_ssl_context_t* ctx) {
    (void)ctx;
}

int vox_ssl_openssl_context_configure(vox_ssl_context_t* ctx, const vox_ssl_config_t* config) {
    (void)ctx;
    (void)config;
    return -1;
}

vox_ssl_session_t* vox_ssl_openssl_session_create(vox_ssl_context_t* ctx, vox_mpool_t* mpool) {
    (void)ctx;
    (void)mpool;
    return NULL;
}

void vox_ssl_openssl_session_destroy(vox_ssl_session_t* session) {
    (void)session;
}

void* vox_ssl_openssl_session_get_rbio(vox_ssl_session_t* session) {
    (void)session;
    return NULL;
}

void* vox_ssl_openssl_session_get_wbio(vox_ssl_session_t* session) {
    (void)session;
    return NULL;
}

int vox_ssl_openssl_session_handshake(vox_ssl_session_t* session) {
    (void)session;
    return -1;
}

ssize_t vox_ssl_openssl_session_read(vox_ssl_session_t* session, void* buf, size_t len) {
    (void)session;
    (void)buf;
    (void)len;
    return -1;
}

ssize_t vox_ssl_openssl_session_write(vox_ssl_session_t* session, const void* buf, size_t len) {
    (void)session;
    (void)buf;
    (void)len;
    return -1;
}

int vox_ssl_openssl_session_shutdown(vox_ssl_session_t* session) {
    (void)session;
    return -1;
}

vox_ssl_state_t vox_ssl_openssl_session_get_state(vox_ssl_session_t* session) {
    (void)session;
    return VOX_SSL_STATE_CLOSED;
}

vox_ssl_error_t vox_ssl_openssl_session_get_error(vox_ssl_session_t* session) {
    (void)session;
    return VOX_SSL_ERROR_INVALID_STATE;
}

int vox_ssl_openssl_session_get_error_string(vox_ssl_session_t* session, char* buf, size_t len) {
    (void)session;
    (void)buf;
    (void)len;
    return -1;
}

size_t vox_ssl_openssl_bio_pending(vox_ssl_session_t* session, vox_ssl_bio_type_t bio_type) {
    (void)session;
    (void)bio_type;
    return 0;
}

ssize_t vox_ssl_openssl_bio_read(vox_ssl_session_t* session, vox_ssl_bio_type_t bio_type, void* buf, size_t len) {
    (void)session;
    (void)bio_type;
    (void)buf;
    (void)len;
    return -1;
}

ssize_t vox_ssl_openssl_bio_write(vox_ssl_session_t* session, vox_ssl_bio_type_t bio_type, const void* buf, size_t len) {
    (void)session;
    (void)bio_type;
    (void)buf;
    (void)len;
    return -1;
}

#endif /* VOX_USE_OPENSSL */
