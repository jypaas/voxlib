/*
 * vox_crypto.h - 加密和哈希算法
 * 提供 MD5, SHA1, SHA256, HMAC-MD5, HMAC-SHA1, HMAC-SHA256, Base64, CRC32 等常见算法
 */

#ifndef VOX_CRYPTO_H
#define VOX_CRYPTO_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ===== MD5 ===== */

/* MD5 上下文结构 */
typedef struct {
    uint32_t state[4];    /* 状态 (ABCD) */
    uint32_t count[2];    /* 位数，模2^64 (低32位，高32位) */
    uint8_t buffer[64];   /* 输入缓冲区 */
} vox_md5_ctx_t;

/* MD5 哈希值大小（字节） */
#define VOX_MD5_DIGEST_SIZE 16

/**
 * 初始化 MD5 上下文
 * @param ctx MD5 上下文指针
 */
void vox_md5_init(vox_md5_ctx_t* ctx);

/**
 * 更新 MD5 上下文（处理数据）
 * @param ctx MD5 上下文指针
 * @param data 输入数据
 * @param len 数据长度（字节）
 */
void vox_md5_update(vox_md5_ctx_t* ctx, const void* data, size_t len);

/**
 * 完成 MD5 计算，输出哈希值
 * @param ctx MD5 上下文指针
 * @param digest 输出缓冲区（至少16字节）
 */
void vox_md5_final(vox_md5_ctx_t* ctx, uint8_t digest[VOX_MD5_DIGEST_SIZE]);

/**
 * 计算数据的 MD5 哈希值（便捷函数）
 * @param data 输入数据
 * @param len 数据长度（字节）
 * @param digest 输出缓冲区（至少16字节）
 */
void vox_md5(const void* data, size_t len, uint8_t digest[VOX_MD5_DIGEST_SIZE]);

/**
 * 将 MD5 哈希值转换为十六进制字符串
 * @param digest MD5 哈希值
 * @param hex_str 输出缓冲区（至少33字节，包含结尾的'\0'）
 */
void vox_md5_hex(const uint8_t digest[VOX_MD5_DIGEST_SIZE], char hex_str[33]);

/* ===== SHA1 ===== */

/* SHA1 上下文结构 */
typedef struct {
    uint32_t state[5];    /* 状态 (A, B, C, D, E) */
    uint32_t count[2];    /* 位数，模2^64 */
    uint8_t buffer[64];   /* 输入缓冲区 */
} vox_sha1_ctx_t;

/* SHA1 哈希值大小（字节） */
#define VOX_SHA1_DIGEST_SIZE 20

/**
 * 初始化 SHA1 上下文
 * @param ctx SHA1 上下文指针
 */
void vox_sha1_init(vox_sha1_ctx_t* ctx);

/**
 * 更新 SHA1 上下文（处理数据）
 * @param ctx SHA1 上下文指针
 * @param data 输入数据
 * @param len 数据长度（字节）
 */
void vox_sha1_update(vox_sha1_ctx_t* ctx, const void* data, size_t len);

/**
 * 完成 SHA1 计算，输出哈希值
 * @param ctx SHA1 上下文指针
 * @param digest 输出缓冲区（至少20字节）
 */
void vox_sha1_final(vox_sha1_ctx_t* ctx, uint8_t digest[VOX_SHA1_DIGEST_SIZE]);

/**
 * 计算数据的 SHA1 哈希值（便捷函数）
 * @param data 输入数据
 * @param len 数据长度（字节）
 * @param digest 输出缓冲区（至少20字节）
 */
void vox_sha1(const void* data, size_t len, uint8_t digest[VOX_SHA1_DIGEST_SIZE]);

/**
 * 将 SHA1 哈希值转换为十六进制字符串
 * @param digest SHA1 哈希值
 * @param hex_str 输出缓冲区（至少41字节，包含结尾的'\0'）
 */
void vox_sha1_hex(const uint8_t digest[VOX_SHA1_DIGEST_SIZE], char hex_str[41]);

/* ===== SHA256 ===== */

/* SHA256 上下文结构 */
typedef struct {
    uint32_t state[8];    /* 状态 (A, B, C, D, E, F, G, H) */
    uint64_t count;       /* 位数，模2^64 */
    uint8_t buffer[64];   /* 输入缓冲区 */
} vox_sha256_ctx_t;

/* SHA256 哈希值大小（字节） */
#define VOX_SHA256_DIGEST_SIZE 32

/**
 * 初始化 SHA256 上下文
 * @param ctx SHA256 上下文指针
 */
void vox_sha256_init(vox_sha256_ctx_t* ctx);

/**
 * 更新 SHA256 上下文（处理数据）
 * @param ctx SHA256 上下文指针
 * @param data 输入数据
 * @param len 数据长度（字节）
 */
void vox_sha256_update(vox_sha256_ctx_t* ctx, const void* data, size_t len);

/**
 * 完成 SHA256 计算，输出哈希值
 * @param ctx SHA256 上下文指针
 * @param digest 输出缓冲区（至少32字节）
 */
void vox_sha256_final(vox_sha256_ctx_t* ctx, uint8_t digest[VOX_SHA256_DIGEST_SIZE]);

/**
 * 计算数据的 SHA256 哈希值（便捷函数）
 * @param data 输入数据
 * @param len 数据长度（字节）
 * @param digest 输出缓冲区（至少32字节）
 */
void vox_sha256(const void* data, size_t len, uint8_t digest[VOX_SHA256_DIGEST_SIZE]);

/**
 * 将 SHA256 哈希值转换为十六进制字符串
 * @param digest SHA256 哈希值
 * @param hex_str 输出缓冲区（至少65字节，包含结尾的'\0'）
 */
void vox_sha256_hex(const uint8_t digest[VOX_SHA256_DIGEST_SIZE], char hex_str[65]);

/* ===== HMAC-MD5 ===== */

/**
 * 计算 HMAC-MD5
 * @param key 密钥
 * @param key_len 密钥长度（字节）
 * @param data 数据
 * @param data_len 数据长度（字节）
 * @param digest 输出缓冲区（至少16字节）
 */
void vox_hmac_md5(const void* key, size_t key_len, 
                  const void* data, size_t data_len,
                  uint8_t digest[VOX_MD5_DIGEST_SIZE]);

/**
 * 将 HMAC-MD5 哈希值转换为十六进制字符串
 * @param digest HMAC-MD5 哈希值
 * @param hex_str 输出缓冲区（至少33字节，包含结尾的'\0'）
 */
void vox_hmac_md5_hex(const uint8_t digest[VOX_MD5_DIGEST_SIZE], char hex_str[33]);

/* ===== HMAC-SHA1 ===== */

/**
 * 计算 HMAC-SHA1
 * @param key 密钥
 * @param key_len 密钥长度（字节）
 * @param data 数据
 * @param data_len 数据长度（字节）
 * @param digest 输出缓冲区（至少20字节）
 */
void vox_hmac_sha1(const void* key, size_t key_len,
                   const void* data, size_t data_len,
                   uint8_t digest[VOX_SHA1_DIGEST_SIZE]);

/**
 * 将 HMAC-SHA1 哈希值转换为十六进制字符串
 * @param digest HMAC-SHA1 哈希值
 * @param hex_str 输出缓冲区（至少41字节，包含结尾的'\0'）
 */
void vox_hmac_sha1_hex(const uint8_t digest[VOX_SHA1_DIGEST_SIZE], char hex_str[41]);

/* ===== HMAC-SHA256 ===== */

/**
 * 计算 HMAC-SHA256
 * @param key 密钥
 * @param key_len 密钥长度（字节）
 * @param data 数据
 * @param data_len 数据长度（字节）
 * @param digest 输出缓冲区（至少32字节）
 */
void vox_hmac_sha256(const void* key, size_t key_len,
                     const void* data, size_t data_len,
                     uint8_t digest[VOX_SHA256_DIGEST_SIZE]);

/**
 * 将 HMAC-SHA256 哈希值转换为十六进制字符串
 * @param digest HMAC-SHA256 哈希值
 * @param hex_str 输出缓冲区（至少65字节，包含结尾的'\0'）
 */
void vox_hmac_sha256_hex(const uint8_t digest[VOX_SHA256_DIGEST_SIZE], char hex_str[65]);

/* ===== Base64 ===== */

/**
 * Base64 编码
 * @param data 输入数据
 * @param len 数据长度（字节）
 * @param output 输出缓冲区（至少 (len + 2) / 3 * 4 + 1 字节，包含结尾的'\0'）
 * @param output_size 输出缓冲区大小
 * @return 成功返回编码后的字符串长度（不含'\0'），失败返回-1
 */
int vox_base64_encode(const void* data, size_t len, char* output, size_t output_size);

/**
 * Base64 解码
 * @param encoded 编码后的字符串
 * @param output 输出缓冲区
 * @param output_size 输出缓冲区大小
 * @return 成功返回解码后的数据长度，失败返回-1
 */
int vox_base64_decode(const char* encoded, void* output, size_t output_size);

/* ===== URL/Filename Safe Base64 ===== */

/**
 * URL和文件名安全的Base64编码（Base64URL）
 * 使用 '-' 替代 '+'，使用 '_' 替代 '/'，不添加填充 '='
 * @param data 输入数据
 * @param len 数据长度（字节）
 * @param output 输出缓冲区（至少 (len + 2) / 3 * 4 + 1 字节，包含结尾的'\0'）
 * @param output_size 输出缓冲区大小
 * @return 成功返回编码后的字符串长度（不含'\0'），失败返回-1
 */
int vox_base64url_encode(const void* data, size_t len, char* output, size_t output_size);

/**
 * URL和文件名安全的Base64解码（Base64URL）
 * 支持标准Base64和Base64URL两种格式
 * @param encoded 编码后的字符串
 * @param output 输出缓冲区
 * @param output_size 输出缓冲区大小
 * @return 成功返回解码后的数据长度，失败返回-1
 */
int vox_base64url_decode(const char* encoded, void* output, size_t output_size);

/* ===== CRC32 ===== */

/**
 * 计算 CRC32 校验值
 * @param data 输入数据
 * @param len 数据长度（字节）
 * @return CRC32 校验值
 */
uint32_t vox_crc32(const void* data, size_t len);

/**
 * 初始化 CRC32 计算（用于流式处理）
 * @return 初始 CRC32 值
 */
uint32_t vox_crc32_init(void);

/**
 * 更新 CRC32 计算
 * @param crc 当前的 CRC32 值
 * @param data 输入数据
 * @param len 数据长度（字节）
 * @return 更新后的 CRC32 值
 */
uint32_t vox_crc32_update(uint32_t crc, const void* data, size_t len);

/**
 * 完成 CRC32 计算（用于流式处理）
 * @param crc 当前的 CRC32 值
 * @return 最终的 CRC32 值
 */
uint32_t vox_crc32_final(uint32_t crc);

/* ===== 安全随机数生成 ===== */

/**
 * 生成密码学安全的随机字节序列
 * 使用平台相关的安全随机数生成器：
 * - Windows: BCryptGenRandom
 * - Linux/Unix: getrandom() 或 /dev/urandom
 * @param buf 输出缓冲区
 * @param len 需要生成的随机字节数
 * @return 成功返回0，失败返回-1
 */
int vox_crypto_random_bytes(void* buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* VOX_CRYPTO_H */
