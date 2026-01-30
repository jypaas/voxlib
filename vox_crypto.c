/*
 * vox_crypto.c - 加密和哈希算法实现
 * 提供 MD5, SHA1, SHA256, HMAC-MD5, HMAC-SHA1, HMAC-SHA256, Base64, CRC32 等常见算法
 */

#include "vox_os.h"
#include "vox_crypto.h"
#include <string.h>
#include <stdint.h>

#ifdef _MSC_VER
    #include <intrin.h>
#endif

/* 字节序转换函数 - 使用编译器内置函数优化 */
static inline uint32_t swap_uint32(uint32_t x) {
#if VOX_LITTLE_ENDIAN
    #if defined(__GNUC__) || defined(__clang__)
        return __builtin_bswap32(x);
    #elif defined(_MSC_VER)
        return _byteswap_ulong(x);
    #else
        return ((x & 0x000000FF) << 24) |
               ((x & 0x0000FF00) << 8) |
               ((x & 0x00FF0000) >> 8) |
               ((x & 0xFF000000) >> 24);
    #endif
#else
    return x;
#endif
}

/* 循环左移 */
static inline uint32_t left_rotate(uint32_t x, int n) {
    return (x << n) | (x >> (32 - n));
}

/* 循环右移 */
static inline uint32_t right_rotate(uint32_t x, int n) {
    return (x >> n) | (x << (32 - n));
}

/* ===== MD5 实现 ===== */

/* MD5 常量 */
static const uint32_t MD5_S[64] = {
    7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
    5,  9, 14, 20, 5,  9, 14, 20, 5,  9, 14, 20, 5,  9, 14, 20,
    4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
    6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21
};

static const uint32_t MD5_K[64] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee,
    0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
    0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be,
    0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
    0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa,
    0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
    0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed,
    0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
    0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c,
    0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
    0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05,
    0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039,
    0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
    0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1,
    0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391
};

/* MD5 辅助函数 */
static uint32_t F(uint32_t x, uint32_t y, uint32_t z) { return (x & y) | (~x & z); }
static uint32_t G(uint32_t x, uint32_t y, uint32_t z) { return (x & z) | (y & ~z); }
static uint32_t H(uint32_t x, uint32_t y, uint32_t z) { return x ^ y ^ z; }
static uint32_t I(uint32_t x, uint32_t y, uint32_t z) { return y ^ (x | ~z); }

void vox_md5_init(vox_md5_ctx_t* ctx) {
    ctx->state[0] = 0x67452301;
    ctx->state[1] = 0xefcdab89;
    ctx->state[2] = 0x98badcfe;
    ctx->state[3] = 0x10325476;
    ctx->count[0] = 0;
    ctx->count[1] = 0;
    memset(ctx->buffer, 0, 64);
}

static void md5_transform(uint32_t state[4], const uint8_t block[64]) {
    uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    uint32_t x[16];
    
    /* 将块转换为小端序的32位整数数组 - 使用 memcpy 优化 */
    #if VOX_LITTLE_ENDIAN
        /* 小端序平台可以直接 memcpy */
        memcpy(x, block, 64);
    #else
        /* 大端序平台需要转换 */
        for (int i = 0; i < 16; i++) {
            x[i] = (uint32_t)block[i * 4] |
                   ((uint32_t)block[i * 4 + 1] << 8) |
                   ((uint32_t)block[i * 4 + 2] << 16) |
                   ((uint32_t)block[i * 4 + 3] << 24);
        }
    #endif
    
    /* 主循环 - 展开为4个阶段以减少分支 */
    /* 阶段1: i = 0-15 */
    for (int i = 0; i < 16; i++) {
        uint32_t f = F(b, c, d);
        f = f + a + MD5_K[i] + x[i];
        a = d;
        d = c;
        c = b;
        b = b + left_rotate(f, MD5_S[i]);
    }
    
    /* 阶段2: i = 16-31 */
    for (int i = 16; i < 32; i++) {
        uint32_t f = G(b, c, d);
        uint32_t g = (5 * i + 1) % 16;
        f = f + a + MD5_K[i] + x[g];
        a = d;
        d = c;
        c = b;
        b = b + left_rotate(f, MD5_S[i]);
    }
    
    /* 阶段3: i = 32-47 */
    for (int i = 32; i < 48; i++) {
        uint32_t f = H(b, c, d);
        uint32_t g = (3 * i + 5) % 16;
        f = f + a + MD5_K[i] + x[g];
        a = d;
        d = c;
        c = b;
        b = b + left_rotate(f, MD5_S[i]);
    }
    
    /* 阶段4: i = 48-63 */
    for (int i = 48; i < 64; i++) {
        uint32_t f = I(b, c, d);
        uint32_t g = (7 * i) % 16;
        f = f + a + MD5_K[i] + x[g];
        a = d;
        d = c;
        c = b;
        b = b + left_rotate(f, MD5_S[i]);
    }
    
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
}

void vox_md5_update(vox_md5_ctx_t* ctx, const void* data, size_t len) {
    const uint8_t* input = (const uint8_t*)data;
    uint32_t i, index, partLen;
    
    index = (uint32_t)((ctx->count[0] >> 3) & 0x3F);
    
    /* 更新位计数 */
    uint64_t bit_count = ((uint64_t)ctx->count[1] << 32) | ctx->count[0];
    bit_count += (uint64_t)len << 3;
    ctx->count[0] = (uint32_t)(bit_count & 0xFFFFFFFF);
    ctx->count[1] = (uint32_t)(bit_count >> 32);
    
    partLen = 64 - index;
    
    if (len >= partLen) {
        memcpy(&ctx->buffer[index], input, partLen);
        md5_transform(ctx->state, ctx->buffer);
        
        /* 处理完整的64字节块 */
        for (i = partLen; i + 63 < len; i += 64) {
            md5_transform(ctx->state, &input[i]);
        }
        index = 0;
    } else {
        i = 0;
    }
    
    /* 保存剩余数据 */
    if (len > i) {
        memcpy(&ctx->buffer[index], &input[i], len - i);
    }
}

void vox_md5_final(vox_md5_ctx_t* ctx, uint8_t digest[VOX_MD5_DIGEST_SIZE]) {
    uint8_t bits[8];
    uint8_t padding[64];
    uint32_t index, padLen;
    
    /* 保存位数（小端序） */
    memcpy(bits, ctx->count, 8);
    
    /* 计算填充 */
    index = (uint32_t)((ctx->count[0] >> 3) & 0x3F);
    padLen = (index < 56) ? (56 - index) : (120 - index);
    
    /* 准备填充数据 - 避免多次 update 调用 */
    padding[0] = 0x80;
    memset(&padding[1], 0, padLen - 1);
    
    /* 添加填充 */
    vox_md5_update(ctx, padding, padLen);
    
    /* 添加长度 */
    vox_md5_update(ctx, bits, 8);
    
    /* 输出（小端序） */
    #if VOX_LITTLE_ENDIAN
        memcpy(digest, ctx->state, 16);
    #else
        for (int i = 0; i < 4; i++) {
            uint32_t val = swap_uint32(ctx->state[i]);
            memcpy(&digest[i * 4], &val, 4);
        }
    #endif
}

void vox_md5(const void* data, size_t len, uint8_t digest[VOX_MD5_DIGEST_SIZE]) {
    vox_md5_ctx_t ctx;
    vox_md5_init(&ctx);
    vox_md5_update(&ctx, data, len);
    vox_md5_final(&ctx, digest);
}

void vox_md5_hex(const uint8_t digest[VOX_MD5_DIGEST_SIZE], char hex_str[33]) {
    static const char hex_chars[] = "0123456789abcdef";
    for (int i = 0; i < 16; i++) {
        hex_str[i * 2] = hex_chars[(digest[i] >> 4) & 0xF];
        hex_str[i * 2 + 1] = hex_chars[digest[i] & 0xF];
    }
    hex_str[32] = '\0';
}

/* ===== SHA1 实现 ===== */

void vox_sha1_init(vox_sha1_ctx_t* ctx) {
    ctx->state[0] = 0x67452301;
    ctx->state[1] = 0xEFCDAB89;
    ctx->state[2] = 0x98BADCFE;
    ctx->state[3] = 0x10325476;
    ctx->state[4] = 0xC3D2E1F0;
    ctx->count[0] = 0;
    ctx->count[1] = 0;
    memset(ctx->buffer, 0, 64);
}

static void sha1_transform(uint32_t state[5], const uint8_t block[64]) {
    uint32_t a, b, c, d, e;
    uint32_t w[80];
    
    /* 将块转换为大端序的32位整数数组 - 优化字节序转换 */
    #if VOX_LITTLE_ENDIAN
        /* 小端序平台需要转换 */
        for (int i = 0; i < 16; i++) {
            uint32_t val;
            memcpy(&val, &block[i * 4], 4);
            w[i] = swap_uint32(val);
        }
    #else
        /* 大端序平台可以直接 memcpy */
        memcpy(w, block, 64);
    #endif
    
    /* 扩展到80个字 - 优化：延迟计算以减少内存访问 */
    for (int i = 16; i < 80; i++) {
        w[i] = left_rotate(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }
    
    /* 注意：可以进一步优化为在循环中按需计算，但会增加代码复杂度 */
    
    a = state[0];
    b = state[1];
    c = state[2];
    d = state[3];
    e = state[4];
    
    /* 主循环 - 展开为4个阶段以减少分支 */
    /* 阶段1: i = 0-19 */
    for (int i = 0; i < 20; i++) {
        uint32_t f = (b & c) | ((~b) & d);
        uint32_t temp = left_rotate(a, 5) + f + e + 0x5A827999 + w[i];
        e = d;
        d = c;
        c = left_rotate(b, 30);
        b = a;
        a = temp;
    }
    
    /* 阶段2: i = 20-39 */
    for (int i = 20; i < 40; i++) {
        uint32_t f = b ^ c ^ d;
        uint32_t temp = left_rotate(a, 5) + f + e + 0x6ED9EBA1 + w[i];
        e = d;
        d = c;
        c = left_rotate(b, 30);
        b = a;
        a = temp;
    }
    
    /* 阶段3: i = 40-59 */
    for (int i = 40; i < 60; i++) {
        uint32_t f = (b & c) | (b & d) | (c & d);
        uint32_t temp = left_rotate(a, 5) + f + e + 0x8F1BBCDC + w[i];
        e = d;
        d = c;
        c = left_rotate(b, 30);
        b = a;
        a = temp;
    }
    
    /* 阶段4: i = 60-79 */
    for (int i = 60; i < 80; i++) {
        uint32_t f = b ^ c ^ d;
        uint32_t temp = left_rotate(a, 5) + f + e + 0xCA62C1D6 + w[i];
        e = d;
        d = c;
        c = left_rotate(b, 30);
        b = a;
        a = temp;
    }
    
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
}

void vox_sha1_update(vox_sha1_ctx_t* ctx, const void* data, size_t len) {
    const uint8_t* input = (const uint8_t*)data;
    uint32_t i, index, partLen;
    
    index = (uint32_t)((ctx->count[0] >> 3) & 0x3F);
    
    /* 更新位计数 - 优化溢出检查 */
    uint64_t bit_count = ((uint64_t)ctx->count[1] << 32) | ctx->count[0];
    bit_count += (uint64_t)len << 3;
    ctx->count[0] = (uint32_t)(bit_count & 0xFFFFFFFF);
    ctx->count[1] = (uint32_t)(bit_count >> 32);
    
    partLen = 64 - index;
    
    if (len >= partLen) {
        memcpy(&ctx->buffer[index], input, partLen);
        sha1_transform(ctx->state, ctx->buffer);
        
        /* 处理完整的64字节块 */
        for (i = partLen; i + 63 < len; i += 64) {
            sha1_transform(ctx->state, &input[i]);
        }
        index = 0;
    } else {
        i = 0;
    }
    
    /* 保存剩余数据 */
    if (len > i) {
        memcpy(&ctx->buffer[index], &input[i], len - i);
    }
}

void vox_sha1_final(vox_sha1_ctx_t* ctx, uint8_t digest[VOX_SHA1_DIGEST_SIZE]) {
    uint8_t bits[8];
    uint8_t padding[64];
    uint32_t index, padLen;
    uint64_t bit_count;
    
    /* 保存原始位数（在填充之前）
     * count[0] 是低32位，count[1] 是高32位
     */
    bit_count = ((uint64_t)ctx->count[1] << 32) | ctx->count[0];
    
    /* 计算填充 */
    index = (uint32_t)((ctx->count[0] >> 3) & 0x3F);
    padLen = (index < 56) ? (56 - index) : (120 - index);
    
    /* 准备填充数据 */
    padding[0] = 0x80;
    memset(&padding[1], 0, padLen - 1);
    
    /* 将长度字段转换为大端序字节 */
    for (int i = 0; i < 8; i++) {
        bits[i] = (uint8_t)((bit_count >> (56 - i * 8)) & 0xFF);
    }
    
    /* 添加填充（0x80 + 0x00...） */
    vox_sha1_update(ctx, padding, padLen);
    
    /* 添加长度（大端序64位） */
    vox_sha1_update(ctx, bits, 8);
    
    /* 输出（大端序） */
    for (int i = 0; i < 5; i++) {
        digest[i * 4] = (uint8_t)((ctx->state[i] >> 24) & 0xFF);
        digest[i * 4 + 1] = (uint8_t)((ctx->state[i] >> 16) & 0xFF);
        digest[i * 4 + 2] = (uint8_t)((ctx->state[i] >> 8) & 0xFF);
        digest[i * 4 + 3] = (uint8_t)(ctx->state[i] & 0xFF);
    }
}

void vox_sha1(const void* data, size_t len, uint8_t digest[VOX_SHA1_DIGEST_SIZE]) {
    vox_sha1_ctx_t ctx;
    vox_sha1_init(&ctx);
    vox_sha1_update(&ctx, data, len);
    vox_sha1_final(&ctx, digest);
}

void vox_sha1_hex(const uint8_t digest[VOX_SHA1_DIGEST_SIZE], char hex_str[41]) {
    static const char hex_chars[] = "0123456789abcdef";
    for (int i = 0; i < 20; i++) {
        hex_str[i * 2] = hex_chars[(digest[i] >> 4) & 0xF];
        hex_str[i * 2 + 1] = hex_chars[digest[i] & 0xF];
    }
    hex_str[40] = '\0';
}

/* ===== SHA256 实现 ===== */

/* SHA256 常量 */
static const uint32_t SHA256_K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
    0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
    0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
    0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

/* SHA256 辅助函数 */
static inline uint32_t sha256_ch(uint32_t x, uint32_t y, uint32_t z) {
    return (x & y) ^ (~x & z);
}

static inline uint32_t sha256_maj(uint32_t x, uint32_t y, uint32_t z) {
    return (x & y) ^ (x & z) ^ (y & z);
}

static inline uint32_t sha256_ep0(uint32_t x) {
    return (right_rotate(x, 2)) ^ (right_rotate(x, 13)) ^ (right_rotate(x, 22));
}

static inline uint32_t sha256_ep1(uint32_t x) {
    return (right_rotate(x, 6)) ^ (right_rotate(x, 11)) ^ (right_rotate(x, 25));
}

static inline uint32_t sha256_sig0(uint32_t x) {
    return (right_rotate(x, 7)) ^ (right_rotate(x, 18)) ^ (x >> 3);
}

static inline uint32_t sha256_sig1(uint32_t x) {
    return (right_rotate(x, 17)) ^ (right_rotate(x, 19)) ^ (x >> 10);
}

void vox_sha256_init(vox_sha256_ctx_t* ctx) {
    ctx->state[0] = 0x6a09e667;
    ctx->state[1] = 0xbb67ae85;
    ctx->state[2] = 0x3c6ef372;
    ctx->state[3] = 0xa54ff53a;
    ctx->state[4] = 0x510e527f;
    ctx->state[5] = 0x9b05688c;
    ctx->state[6] = 0x1f83d9ab;
    ctx->state[7] = 0x5be0cd19;
    ctx->count = 0;
    memset(ctx->buffer, 0, 64);
}

static void sha256_transform(uint32_t state[8], const uint8_t block[64]) {
    uint32_t a, b, c, d, e, f, g, h;
    uint32_t w[64];
    
    /* 将块转换为大端序的32位整数数组 */
    #if VOX_LITTLE_ENDIAN
        for (int i = 0; i < 16; i++) {
            uint32_t val;
            memcpy(&val, &block[i * 4], 4);
            w[i] = swap_uint32(val);
        }
    #else
        memcpy(w, block, 64);
    #endif
    
    /* 扩展到64个字 */
    for (int i = 16; i < 64; i++) {
        w[i] = sha256_sig1(w[i - 2]) + w[i - 7] + sha256_sig0(w[i - 15]) + w[i - 16];
    }
    
    a = state[0];
    b = state[1];
    c = state[2];
    d = state[3];
    e = state[4];
    f = state[5];
    g = state[6];
    h = state[7];
    
    /* 主循环 */
    for (int i = 0; i < 64; i++) {
        uint32_t temp1 = h + sha256_ep1(e) + sha256_ch(e, f, g) + SHA256_K[i] + w[i];
        uint32_t temp2 = sha256_ep0(a) + sha256_maj(a, b, c);
        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }
    
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
}

void vox_sha256_update(vox_sha256_ctx_t* ctx, const void* data, size_t len) {
    const uint8_t* input = (const uint8_t*)data;
    size_t i, index, partLen;
    
    index = (size_t)((ctx->count >> 3) & 0x3F);
    
    /* 更新位计数 */
    ctx->count += (uint64_t)len << 3;
    
    partLen = 64 - index;
    
    if (len >= partLen) {
        memcpy(&ctx->buffer[index], input, partLen);
        sha256_transform(ctx->state, ctx->buffer);
        
        /* 处理完整的64字节块 */
        for (i = partLen; i + 64 <= len; i += 64) {
            sha256_transform(ctx->state, &input[i]);
        }
        index = 0;
    } else {
        i = 0;
    }
    
    /* 保存剩余数据 */
    if (len > i) {
        memcpy(&ctx->buffer[index], &input[i], len - i);
    }
}

void vox_sha256_final(vox_sha256_ctx_t* ctx, uint8_t digest[VOX_SHA256_DIGEST_SIZE]) {
    uint8_t bits[8];
    uint8_t padding[64];
    size_t index, padLen;
    uint64_t bit_count;
    
    /* 保存原始位数（在填充之前） */
    bit_count = ctx->count;
    
    /* 计算填充 */
    index = (size_t)((ctx->count >> 3) & 0x3F);
    padLen = (index < 56) ? (56 - index) : (120 - index);
    
    /* 准备填充数据 */
    padding[0] = 0x80;
    memset(&padding[1], 0, padLen - 1);
    
    /* 将长度字段转换为大端序字节 */
    for (int i = 0; i < 8; i++) {
        bits[i] = (uint8_t)((bit_count >> (56 - i * 8)) & 0xFF);
    }
    
    /* 添加填充（0x80 + 0x00...） */
    vox_sha256_update(ctx, padding, padLen);
    
    /* 添加长度（大端序64位） */
    vox_sha256_update(ctx, bits, 8);
    
    /* 输出（大端序） */
    for (int i = 0; i < 8; i++) {
        digest[i * 4] = (uint8_t)((ctx->state[i] >> 24) & 0xFF);
        digest[i * 4 + 1] = (uint8_t)((ctx->state[i] >> 16) & 0xFF);
        digest[i * 4 + 2] = (uint8_t)((ctx->state[i] >> 8) & 0xFF);
        digest[i * 4 + 3] = (uint8_t)(ctx->state[i] & 0xFF);
    }
}

void vox_sha256(const void* data, size_t len, uint8_t digest[VOX_SHA256_DIGEST_SIZE]) {
    vox_sha256_ctx_t ctx;
    vox_sha256_init(&ctx);
    vox_sha256_update(&ctx, data, len);
    vox_sha256_final(&ctx, digest);
}

void vox_sha256_hex(const uint8_t digest[VOX_SHA256_DIGEST_SIZE], char hex_str[65]) {
    static const char hex_chars[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        hex_str[i * 2] = hex_chars[(digest[i] >> 4) & 0xF];
        hex_str[i * 2 + 1] = hex_chars[digest[i] & 0xF];
    }
    hex_str[64] = '\0';
}

/* ===== HMAC-MD5 实现 ===== */

void vox_hmac_md5(const void* key, size_t key_len,
                  const void* data, size_t data_len,
                  uint8_t digest[VOX_MD5_DIGEST_SIZE]) {
    uint8_t key_block[64];
    uint8_t o_key_pad[64];
    uint8_t i_key_pad[64];
    vox_md5_ctx_t ctx;
    
    /* 如果密钥长度超过64字节，先计算其MD5 */
    if (key_len > 64) {
        vox_md5(key, key_len, key_block);
        key = key_block;
        key_len = 16;
    }
    
    /* 准备密钥块 */
    memset(key_block, 0, 64);
    memcpy(key_block, key, key_len);
    
    /* 计算 o_key_pad 和 i_key_pad */
    for (int i = 0; i < 64; i++) {
        o_key_pad[i] = key_block[i] ^ 0x5c;
        i_key_pad[i] = key_block[i] ^ 0x36;
    }
    
    /* 计算 HMAC-MD5 = MD5(o_key_pad || MD5(i_key_pad || data)) */
    vox_md5_init(&ctx);
    vox_md5_update(&ctx, i_key_pad, 64);
    vox_md5_update(&ctx, data, data_len);
    vox_md5_final(&ctx, digest);
    
    vox_md5_init(&ctx);
    vox_md5_update(&ctx, o_key_pad, 64);
    vox_md5_update(&ctx, digest, 16);
    vox_md5_final(&ctx, digest);
}

void vox_hmac_md5_hex(const uint8_t digest[VOX_MD5_DIGEST_SIZE], char hex_str[33]) {
    vox_md5_hex(digest, hex_str);
}

/* ===== HMAC-SHA1 实现 ===== */

void vox_hmac_sha1(const void* key, size_t key_len,
                   const void* data, size_t data_len,
                   uint8_t digest[VOX_SHA1_DIGEST_SIZE]) {
    uint8_t key_block[64];
    uint8_t o_key_pad[64];
    uint8_t i_key_pad[64];
    vox_sha1_ctx_t ctx;
    
    /* 如果密钥长度超过64字节，先计算其SHA1 */
    if (key_len > 64) {
        vox_sha1(key, key_len, key_block);
        key = key_block;
        key_len = 20;
    }
    
    /* 准备密钥块 */
    memset(key_block, 0, 64);
    memcpy(key_block, key, key_len);
    
    /* 计算 o_key_pad 和 i_key_pad */
    for (int i = 0; i < 64; i++) {
        o_key_pad[i] = key_block[i] ^ 0x5c;
        i_key_pad[i] = key_block[i] ^ 0x36;
    }
    
    /* 计算 HMAC-SHA1 = SHA1(o_key_pad || SHA1(i_key_pad || data)) */
    vox_sha1_init(&ctx);
    vox_sha1_update(&ctx, i_key_pad, 64);
    vox_sha1_update(&ctx, data, data_len);
    vox_sha1_final(&ctx, digest);
    
    vox_sha1_init(&ctx);
    vox_sha1_update(&ctx, o_key_pad, 64);
    vox_sha1_update(&ctx, digest, 20);
    vox_sha1_final(&ctx, digest);
}

void vox_hmac_sha1_hex(const uint8_t digest[VOX_SHA1_DIGEST_SIZE], char hex_str[41]) {
    vox_sha1_hex(digest, hex_str);
}

/* ===== HMAC-SHA256 实现 ===== */

void vox_hmac_sha256(const void* key, size_t key_len,
                     const void* data, size_t data_len,
                     uint8_t digest[VOX_SHA256_DIGEST_SIZE]) {
    uint8_t key_block[64];
    uint8_t o_key_pad[64];
    uint8_t i_key_pad[64];
    vox_sha256_ctx_t ctx;
    
    /* 如果密钥长度超过64字节，先计算其SHA256 */
    if (key_len > 64) {
        vox_sha256(key, key_len, key_block);
        key = key_block;
        key_len = 32;
    }
    
    /* 准备密钥块 */
    memset(key_block, 0, 64);
    memcpy(key_block, key, key_len);
    
    /* 计算 o_key_pad 和 i_key_pad */
    for (int i = 0; i < 64; i++) {
        o_key_pad[i] = key_block[i] ^ 0x5c;
        i_key_pad[i] = key_block[i] ^ 0x36;
    }
    
    /* 计算 HMAC-SHA256 = SHA256(o_key_pad || SHA256(i_key_pad || data)) */
    vox_sha256_init(&ctx);
    vox_sha256_update(&ctx, i_key_pad, 64);
    vox_sha256_update(&ctx, data, data_len);
    vox_sha256_final(&ctx, digest);
    
    vox_sha256_init(&ctx);
    vox_sha256_update(&ctx, o_key_pad, 64);
    vox_sha256_update(&ctx, digest, 32);
    vox_sha256_final(&ctx, digest);
}

void vox_hmac_sha256_hex(const uint8_t digest[VOX_SHA256_DIGEST_SIZE], char hex_str[65]) {
    vox_sha256_hex(digest, hex_str);
}

/* ===== Base64 实现 ===== */

static const char base64_chars[] = 
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int vox_base64_encode(const void* data, size_t len, char* output, size_t output_size) {
    const uint8_t* input = (const uint8_t*)data;
    size_t output_len = ((len + 2) / 3) * 4;
    
    if (output_size < output_len + 1) {
        return -1;
    }
    
    size_t i = 0, j = 0;
    /* 处理完整的3字节组 - 优化循环 */
    size_t full_groups = len / 3;
    for (size_t group = 0; group < full_groups; group++) {
        i = group * 3;
        uint32_t triple = ((uint32_t)input[i] << 16) | 
                          ((uint32_t)input[i + 1] << 8) | 
                          (uint32_t)input[i + 2];
        output[j++] = base64_chars[(triple >> 18) & 0x3F];
        output[j++] = base64_chars[(triple >> 12) & 0x3F];
        output[j++] = base64_chars[(triple >> 6) & 0x3F];
        output[j++] = base64_chars[triple & 0x3F];
    }
    
    /* 处理剩余字节 */
    i = full_groups * 3;
    if (i < len) {
        output[j++] = base64_chars[(input[i] >> 2) & 0x3F];
        if (i == len - 1) {
            output[j++] = base64_chars[((input[i] & 0x3) << 4)];
            output[j++] = '=';
        } else {
            output[j++] = base64_chars[((input[i] & 0x3) << 4) | ((input[i + 1] & 0xF0) >> 4)];
            output[j++] = base64_chars[((input[i + 1] & 0xF) << 2)];
        }
        output[j++] = '=';
    }
    
    output[j] = '\0';
    return (int)j;
}

/* Base64 字符查找表 - 优化性能 */
static const int8_t base64_char_table[256] = {
    -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2,
    -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2,
    -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, 62, -2, -2, -2, 63,
    52, 53, 54, 55, 56, 57, 58, 59, 60, 61, -2, -2, -2, -1, -2, -2,
    -2,  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14,
    15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, -2, -2, -2, -2, -2,
    -2, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40,
    41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, -2, -2, -2, -2, -2,
    -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2,
    -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2,
    -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2,
    -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2,
    -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2,
    -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2,
    -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2,
    -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2
};

static inline int base64_char_value(char c) {
    return base64_char_table[(unsigned char)c];
}

/* Base64URL 字符查找表 - 优化性能 */
static const int8_t base64url_char_table[256] = {
    -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2,
    -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2,
    -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, 62, -2, -2,
    52, 53, 54, 55, 56, 57, 58, 59, 60, 61, -2, -2, -2, -1, -2, -2,
    -2,  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14,
    15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, -2, -2, -2, -2, 63,
    -2, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40,
    41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, -2, -2, -2, -2, -2,
    -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2,
    -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2,
    -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2,
    -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2,
    -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2,
    -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2,
    -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2,
    -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2
};

static inline int base64url_char_value(char c) {
    int val = base64url_char_table[(unsigned char)c];
    /* 处理 '+' 和 '/' 的兼容性 */
    if (val == -2 && c == '+') return 62;
    if (val == -2 && c == '/') return 63;
    return val;
}

int vox_base64_decode(const char* encoded, void* output, size_t output_size) {
    size_t len = strlen(encoded);
    if (len == 0) return 0;
    
    /* 移除末尾的空白字符 */
    while (len > 0 && (encoded[len - 1] == ' ' || encoded[len - 1] == '\n' || encoded[len - 1] == '\r' || encoded[len - 1] == '\t')) {
        len--;
    }
    if (len == 0) return 0;
    
    size_t padding = 0;
    if (len > 0 && encoded[len - 1] == '=') padding++;
    if (len > 1 && encoded[len - 2] == '=') padding++;
    
    size_t output_len = (len * 3) / 4 - padding;
    if (output_size < output_len) {
        return -1;
    }
    
    uint8_t* out = (uint8_t*)output;
    size_t i = 0, j = 0;
    size_t process_len = len - padding;
    
    /* 处理完整的4字符组 */
    for (i = 0; i + 4 <= process_len; i += 4) {
        int v1 = base64_char_value(encoded[i]);
        int v2 = base64_char_value(encoded[i + 1]);
        int v3 = base64_char_value(encoded[i + 2]);
        int v4 = base64_char_value(encoded[i + 3]);
        
        if (v1 < 0 || v2 < 0 || v3 < 0 || v4 < 0) {
            return -1;
        }
        
        out[j++] = (uint8_t)((v1 << 2) | (v2 >> 4));
        out[j++] = (uint8_t)(((v2 & 0xF) << 4) | (v3 >> 2));
        out[j++] = (uint8_t)(((v3 & 0x3) << 6) | v4);
    }
    
    /* 处理填充或剩余字符 */
    if (padding > 0 && i < len) {
        int v1 = base64_char_value(encoded[i]);
        int v2 = (i + 1 < len) ? base64_char_value(encoded[i + 1]) : -1;
        
        if (v1 < 0 || v2 < 0) {
            return -1;
        }
        
        out[j++] = (uint8_t)((v1 << 2) | (v2 >> 4));
        
        if (padding == 1 && i + 2 < len) {
            int v3 = base64_char_value(encoded[i + 2]);
            if (v3 < 0) {
                return -1;
            }
            out[j++] = (uint8_t)(((v2 & 0xF) << 4) | (v3 >> 2));
        }
    } else if (i < process_len) {
        /* 没有填充但有余数，这是无效的Base64字符串 */
        return -1;
    }
    
    return (int)j;
}

/* ===== URL/Filename Safe Base64 实现 ===== */

static const char base64url_chars[] = 
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

int vox_base64url_encode(const void* data, size_t len, char* output, size_t output_size) {
    const uint8_t* input = (const uint8_t*)data;
    
    /* 处理空输入 */
    if (len == 0) {
        if (output_size < 1) {
            return -1;
        }
        output[0] = '\0';
        return 0;
    }
    
    /* URL安全的Base64不需要填充，所以最大长度是 (len + 2) / 3 * 4 */
    size_t output_len = ((len + 2) / 3) * 4;
    
    if (output_size < output_len + 1) {
        return -1;
    }
    
    size_t i = 0, j = 0;
    /* 处理完整的3字节组，使用 i + 2 < len 避免无符号溢出 */
    while (i + 2 < len) {
        output[j++] = base64url_chars[(input[i] >> 2) & 0x3F];
        output[j++] = base64url_chars[((input[i] & 0x3) << 4) | ((input[i + 1] & 0xF0) >> 4)];
        output[j++] = base64url_chars[((input[i + 1] & 0xF) << 2) | ((input[i + 2] & 0xC0) >> 6)];
        output[j++] = base64url_chars[input[i + 2] & 0x3F];
        i += 3;
    }
    
    /* 处理剩余字节 */
    if (i < len) {
        output[j++] = base64url_chars[(input[i] >> 2) & 0x3F];
        if (i == len - 1) {
            /* 只有1个剩余字节 */
            output[j++] = base64url_chars[((input[i] & 0x3) << 4)];
            /* URL安全的Base64不添加填充 */
        } else {
            /* 有2个剩余字节 */
            output[j++] = base64url_chars[((input[i] & 0x3) << 4) | ((input[i + 1] & 0xF0) >> 4)];
            output[j++] = base64url_chars[((input[i + 1] & 0xF) << 2)];
            /* URL安全的Base64不添加填充 */
        }
    }
    
    output[j] = '\0';
    return (int)j;
}

int vox_base64url_decode(const char* encoded, void* output, size_t output_size) {
    size_t len = strlen(encoded);
    if (len == 0) return 0;
    
    /* 计算填充（Base64URL可能没有填充，但我们也支持标准Base64格式） */
    size_t padding = 0;
    if (len > 0 && encoded[len - 1] == '=') padding++;
    if (len > 1 && encoded[len - 2] == '=') padding++;
    
    /* 如果没有填充，需要计算实际输出长度 */
    size_t output_len;
    if (padding == 0) {
        /* 没有填充，根据输入长度计算 */
        size_t remainder = len % 4;
        if (remainder == 0) {
            output_len = (len * 3) / 4;
        } else if (remainder == 1) {
            return -1;  /* 无效的Base64字符串 */
        } else if (remainder == 2) {
            output_len = (len * 3) / 4 + 1;
        } else {  /* remainder == 3 */
            output_len = (len * 3) / 4 + 2;
        }
    } else {
        output_len = (len * 3) / 4 - padding;
    }
    
    if (output_size < output_len) {
        return -1;
    }
    
    uint8_t* out = (uint8_t*)output;
    size_t i = 0, j = 0;
    
    /* 处理完整的4字符组 */
    size_t process_len = len - padding;
    if (process_len % 4 != 0) {
        process_len = (process_len / 4) * 4;
    }
    
    for (i = 0; i < process_len; i += 4) {
        int v1 = base64url_char_value(encoded[i]);
        int v2 = base64url_char_value(encoded[i + 1]);
        int v3 = base64url_char_value(encoded[i + 2]);
        int v4 = base64url_char_value(encoded[i + 3]);
        
        if (v1 < 0 || v2 < 0 || v3 < 0 || v4 < 0) {
            return -1;
        }
        
        out[j++] = (uint8_t)((v1 << 2) | (v2 >> 4));
        out[j++] = (uint8_t)(((v2 & 0xF) << 4) | (v3 >> 2));
        out[j++] = (uint8_t)(((v3 & 0x3) << 6) | v4);
    }
    
    /* 处理剩余字符（如果有） */
    size_t remaining = len - padding;
    if (i < remaining && i < len) {
        int v1 = base64url_char_value(encoded[i]);
        if (v1 < 0) {
            return -1;
        }
        
        /* 至少需要2个字符才能解码出1个字节 */
        if (i + 1 >= remaining || i + 1 >= len) {
            return -1;  /* 输入不完整 */
        }
        
        int v2 = base64url_char_value(encoded[i + 1]);
        if (v2 < 0) {
            return -1;
        }
        
        out[j++] = (uint8_t)((v1 << 2) | (v2 >> 4));
        
        /* 如果有第3个字符，可以解码出第2个字节 */
        if (i + 2 < remaining && i + 2 < len) {
            int v3 = base64url_char_value(encoded[i + 2]);
            if (v3 < 0) {
                return -1;
            }
            out[j++] = (uint8_t)(((v2 & 0xF) << 4) | (v3 >> 2));
        }
    }
    
    return (int)j;
}

/* ===== CRC32 实现 ===== */

static uint32_t crc32_table[256];
static int crc32_table_computed = 0;

static void compute_crc32_table(void) {
    if (crc32_table_computed) return;
    
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t crc = i;
        for (int j = 0; j < 8; j++) {
            if (crc & 1) {
                crc = (crc >> 1) ^ 0xEDB88320;
            } else {
                crc >>= 1;
            }
        }
        crc32_table[i] = crc;
    }
    
    crc32_table_computed = 1;
}

uint32_t vox_crc32_init(void) {
    compute_crc32_table();
    return 0xFFFFFFFF;
}

uint32_t vox_crc32_update(uint32_t crc, const void* data, size_t len) {
    compute_crc32_table();
    
    const uint8_t* bytes = (const uint8_t*)data;
    
    /* 对齐到4字节边界以提高性能 */
    while (len > 0 && ((uintptr_t)bytes & 3) != 0) {
        crc = (crc >> 8) ^ crc32_table[(crc ^ *bytes) & 0xFF];
        bytes++;
        len--;
    }
    
    /* 处理4字节对齐的数据块 */
    const uint32_t* words = (const uint32_t*)bytes;
    size_t word_count = len / 4;
    for (size_t i = 0; i < word_count; i++) {
        uint32_t word = words[i];
        #if VOX_LITTLE_ENDIAN
            /* 小端序直接处理 */
        #else
            word = swap_uint32(word);
        #endif
        crc ^= word;
        crc = (crc >> 8) ^ crc32_table[crc & 0xFF];
        crc = (crc >> 8) ^ crc32_table[crc & 0xFF];
        crc = (crc >> 8) ^ crc32_table[crc & 0xFF];
        crc = (crc >> 8) ^ crc32_table[crc & 0xFF];
    }
    
    /* 处理剩余字节 */
    bytes = (const uint8_t*)(words + word_count);
    len = len % 4;
    for (size_t i = 0; i < len; i++) {
        crc = (crc >> 8) ^ crc32_table[(crc ^ bytes[i]) & 0xFF];
    }
    
    return crc;
}

uint32_t vox_crc32_final(uint32_t crc) {
    return crc ^ 0xFFFFFFFF;
}

uint32_t vox_crc32(const void* data, size_t len) {
    uint32_t crc = vox_crc32_init();
    crc = vox_crc32_update(crc, data, len);
    return vox_crc32_final(crc);
}

/* ===== 安全随机数生成 ===== */

#if defined(VOX_OS_WINDOWS)
    #include <windows.h>
    #include <bcrypt.h>
    #pragma comment(lib, "bcrypt.lib")
#elif defined(VOX_OS_LINUX) || defined(VOX_OS_UNIX) || defined(VOX_OS_DARWIN)
    #include <sys/random.h>
    #include <unistd.h>
    #include <fcntl.h>
    #include <errno.h>
#endif

/**
 * 生成密码学安全的随机字节序列
 */
int vox_crypto_random_bytes(void* buf, size_t len) {
    if (!buf || len == 0) {
        return -1;
    }

#if defined(VOX_OS_WINDOWS)
    /* Windows: 使用 BCryptGenRandom */
    NTSTATUS status = BCryptGenRandom(NULL, (PUCHAR)buf, (ULONG)len, 
                                      BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    return (status == 0) ? 0 : -1;

#elif defined(VOX_OS_LINUX) || defined(VOX_OS_UNIX) || defined(VOX_OS_DARWIN)
    /* Linux/Unix: 优先使用 getrandom() 系统调用 */
    #if defined(__linux__) && defined(SYS_getrandom)
    ssize_t ret = getrandom(buf, len, 0);
    if (ret >= 0 && (size_t)ret == len) {
        return 0;
    }
    /* getrandom 失败，降级到 /dev/urandom */
    #endif

    /* 使用 /dev/urandom 作为降级方案 */
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return -1;
    }

    size_t total_read = 0;
    while (total_read < len) {
        ssize_t n = read(fd, (uint8_t*)buf + total_read, len - total_read);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) {
                continue;  /* 被信号中断，重试 */
            }
            close(fd);
            return -1;
        }
        total_read += n;
    }

    close(fd);
    return 0;

#else
    /* 不支持的平台 */
    #error "vox_crypto_random_bytes not implemented for this platform"
    return -1;
#endif
}
