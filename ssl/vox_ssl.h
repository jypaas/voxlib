/*
 * vox_ssl.h - SSL/TLS Backend 抽象层
 * 提供统一的 SSL/TLS 接口，支持 OpenSSL/WolfSSL/mbedTLS 等
 */

#ifndef VOX_SSL_H
#define VOX_SSL_H

#include "../vox_mpool.h"
#include "../vox_os.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 前向声明 */
typedef struct vox_ssl_context vox_ssl_context_t;
typedef struct vox_ssl_session vox_ssl_session_t;

/* SSL/TLS 模式 */
typedef enum {
    VOX_SSL_MODE_CLIENT = 0,  /* 客户端模式 */
    VOX_SSL_MODE_SERVER       /* 服务器模式 */
} vox_ssl_mode_t;

/* SSL/TLS 状态 */
typedef enum {
    VOX_SSL_STATE_INIT = 0,      /* 初始化 */
    VOX_SSL_STATE_HANDSHAKING,   /* 握手中 */
    VOX_SSL_STATE_CONNECTED,     /* 已连接 */
    VOX_SSL_STATE_CLOSED         /* 已关闭 */
} vox_ssl_state_t;

/* SSL/TLS 错误码 */
typedef enum {
    VOX_SSL_ERROR_NONE = 0,
    VOX_SSL_ERROR_WANT_READ = -1,     /* 需要读取更多数据 */
    VOX_SSL_ERROR_WANT_WRITE = -2,    /* 需要写入更多数据 */
    VOX_SSL_ERROR_SYSCALL = -3,       /* 系统调用错误 */
    VOX_SSL_ERROR_SSL = -4,           /* SSL 错误 */
    VOX_SSL_ERROR_ZERO_RETURN = -5,   /* 连接关闭 */
    VOX_SSL_ERROR_INVALID_STATE = -6  /* 无效状态 */
} vox_ssl_error_t;

/* SSL/TLS 配置 */
typedef struct {
    const char* cert_file;       /* 证书文件路径（服务器模式） */
    const char* key_file;        /* 私钥文件路径（服务器模式） */
    const char* ca_file;         /* CA 证书文件路径（客户端模式，用于验证服务器） */
    const char* ca_path;         /* CA 证书目录路径 */
    bool verify_peer;            /* 是否验证对端证书（客户端模式） */
    bool verify_hostname;        /* 是否验证主机名（客户端模式） */
    const char* ciphers;         /* 密码套件列表 */
    const char* protocols;       /* 支持的协议版本（如 "TLSv1.2,TLSv1.3"） */
    int dtls_mtu;                /* DTLS 应用层 MTU（字节），0 表示使用默认值 1440 */
                                /* 建议值：IPv4 使用 1440，IPv6 使用 1420，最大不超过 1500 */
} vox_ssl_config_t;

/* ===== SSL Context API ===== */

/**
 * 创建 SSL Context
 * @param mpool 内存池
 * @param mode SSL 模式（客户端或服务器）
 * @return 成功返回 context 指针，失败返回 NULL
 */
vox_ssl_context_t* vox_ssl_context_create(vox_mpool_t* mpool, vox_ssl_mode_t mode);

/**
 * 销毁 SSL Context
 * @param ctx context 指针
 */
void vox_ssl_context_destroy(vox_ssl_context_t* ctx);

/**
 * 配置 SSL Context
 * @param ctx context 指针
 * @param config 配置结构体
 * @return 成功返回0，失败返回-1
 */
int vox_ssl_context_configure(vox_ssl_context_t* ctx, const vox_ssl_config_t* config);

/* ===== SSL Session API ===== */

/**
 * 创建 SSL Session（从 context 创建）
 * @param ctx context 指针
 * @param mpool 内存池
 * @return 成功返回 session 指针，失败返回 NULL
 */
vox_ssl_session_t* vox_ssl_session_create(vox_ssl_context_t* ctx, vox_mpool_t* mpool);

/**
 * 销毁 SSL Session
 * @param session session 指针
 */
void vox_ssl_session_destroy(vox_ssl_session_t* session);

/**
 * 获取读取 BIO（用于从 socket 读取加密数据后写入）
 * @param session session 指针
 * @return 返回 BIO 指针（平台特定类型，需要转换为具体实现类型）
 */
void* vox_ssl_session_get_rbio(vox_ssl_session_t* session);

/**
 * 获取写入 BIO（用于从 SSL 读取加密数据后写入 socket）
 * @param session session 指针
 * @return 返回 BIO 指针（平台特定类型，需要转换为具体实现类型）
 */
void* vox_ssl_session_get_wbio(vox_ssl_session_t* session);

/**
 * 执行 SSL 握手
 * @param session session 指针
 * @return 成功返回0，需要更多数据返回 VOX_SSL_ERROR_WANT_READ 或 VOX_SSL_ERROR_WANT_WRITE，失败返回-1
 */
int vox_ssl_session_handshake(vox_ssl_session_t* session);

/**
 * 读取解密后的数据
 * @param session session 指针
 * @param buf 缓冲区
 * @param len 缓冲区大小
 * @return 成功返回读取的字节数，需要更多数据返回 VOX_SSL_ERROR_WANT_READ，失败返回-1
 */
ssize_t vox_ssl_session_read(vox_ssl_session_t* session, void* buf, size_t len);

/**
 * 写入数据（将被加密）
 * @param session session 指针
 * @param buf 数据缓冲区
 * @param len 数据长度
 * @return 成功返回写入的字节数，需要更多数据返回 VOX_SSL_ERROR_WANT_WRITE，失败返回-1
 */
ssize_t vox_ssl_session_write(vox_ssl_session_t* session, const void* buf, size_t len);

/**
 * 关闭 SSL 连接（发送关闭通知）
 * @param session session 指针
 * @return 成功返回0，失败返回-1
 */
int vox_ssl_session_shutdown(vox_ssl_session_t* session);

/**
 * 获取 SSL 状态
 * @param session session 指针
 * @return 返回 SSL 状态
 */
vox_ssl_state_t vox_ssl_session_get_state(vox_ssl_session_t* session);

/**
 * 获取最后的错误码
 * @param session session 指针
 * @return 返回错误码
 */
vox_ssl_error_t vox_ssl_session_get_error(vox_ssl_session_t* session);

/**
 * 获取错误信息字符串
 * @param session session 指针
 * @param buf 缓冲区
 * @param len 缓冲区大小
 * @return 成功返回写入的字节数，失败返回-1
 */
int vox_ssl_session_get_error_string(vox_ssl_session_t* session, char* buf, size_t len);

/* ===== BIO 操作 API ===== */

/* BIO 类型 */
typedef enum {
    VOX_SSL_BIO_RBIO = 0,  /* 读取 BIO（从 socket 读取后写入） */
    VOX_SSL_BIO_WBIO       /* 写入 BIO（从 SSL 读取后写入 socket） */
} vox_ssl_bio_type_t;

/**
 * 检查 BIO 中待读取的数据量
 * @param session session 指针
 * @param bio_type BIO 类型（rbio 或 wbio）
 * @return 返回待读取的字节数，失败返回0
 */
size_t vox_ssl_bio_pending(vox_ssl_session_t* session, vox_ssl_bio_type_t bio_type);

/**
 * 从 BIO 读取数据
 * @param session session 指针
 * @param bio_type BIO 类型（rbio 或 wbio）
 * @param buf 缓冲区
 * @param len 缓冲区大小
 * @return 成功返回读取的字节数，失败返回-1
 */
ssize_t vox_ssl_bio_read(vox_ssl_session_t* session, vox_ssl_bio_type_t bio_type, void* buf, size_t len);

/**
 * 向 BIO 写入数据
 * @param session session 指针
 * @param bio_type BIO 类型（rbio 或 wbio）
 * @param buf 数据缓冲区
 * @param len 数据长度
 * @return 成功返回写入的字节数，失败返回-1
 */
ssize_t vox_ssl_bio_write(vox_ssl_session_t* session, vox_ssl_bio_type_t bio_type, const void* buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* VOX_SSL_H */
