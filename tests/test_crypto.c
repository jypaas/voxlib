/* ============================================================
 * test_crypto.c - vox_crypto 模块测试
 * ============================================================ */

#include "test_runner.h"
#include "../vox_crypto.h"
#include <string.h>

/* 测试MD5 */
static void test_crypto_md5(vox_mpool_t* mpool) {
    (void)mpool;  /* 未使用的参数 */
    const char* input = "hello";
    uint8_t digest[VOX_MD5_DIGEST_SIZE];
    char hex_str[33];
    
    /* 测试便捷函数 */
    vox_md5(input, strlen(input), digest);
    vox_md5_hex(digest, hex_str);
    TEST_ASSERT_NE(strlen(hex_str), 0, "MD5 hex字符串不应为空");
    
    /* 测试流式处理 */
    vox_md5_ctx_t ctx;
    vox_md5_init(&ctx);
    vox_md5_update(&ctx, "hel", 3);
    vox_md5_update(&ctx, "lo", 2);
    vox_md5_final(&ctx, digest);
    
    /* 验证结果 */
    uint8_t expected[VOX_MD5_DIGEST_SIZE];
    vox_md5("hello", 5, expected);
    TEST_ASSERT_EQ(memcmp(digest, expected, VOX_MD5_DIGEST_SIZE), 0, "流式MD5结果不正确");
}

/* 测试SHA1 */
static void test_crypto_sha1(vox_mpool_t* mpool) {
    (void)mpool;  /* 未使用的参数 */
    const char* input = "hello";
    uint8_t digest[VOX_SHA1_DIGEST_SIZE];
    char hex_str[41];
    
    /* 测试便捷函数 */
    vox_sha1(input, strlen(input), digest);
    vox_sha1_hex(digest, hex_str);
    TEST_ASSERT_NE(strlen(hex_str), 0, "SHA1 hex字符串不应为空");
    
    /* 测试流式处理 */
    vox_sha1_ctx_t ctx;
    vox_sha1_init(&ctx);
    vox_sha1_update(&ctx, "hel", 3);
    vox_sha1_update(&ctx, "lo", 2);
    vox_sha1_final(&ctx, digest);
    
    /* 验证结果 */
    uint8_t expected[VOX_SHA1_DIGEST_SIZE];
    vox_sha1("hello", 5, expected);
    TEST_ASSERT_EQ(memcmp(digest, expected, VOX_SHA1_DIGEST_SIZE), 0, "流式SHA1结果不正确");
}

/* 测试Base64 */
static void test_crypto_base64(vox_mpool_t* mpool) {
    (void)mpool;  /* 未使用的参数 */
    const char* input = "Hello, World!";
    char encoded[64];
    uint8_t decoded[64];
    
    /* 测试编码 */
    int encoded_len = vox_base64_encode(input, strlen(input), encoded, sizeof(encoded));
    TEST_ASSERT_NE(encoded_len, -1, "Base64编码失败");
    TEST_ASSERT_NE(encoded_len, 0, "编码长度不应为0");
    
    /* 测试解码 */
    int decoded_len = vox_base64_decode(encoded, decoded, sizeof(decoded));
    TEST_ASSERT_NE(decoded_len, -1, "Base64解码失败");
    TEST_ASSERT_EQ(decoded_len, (int)strlen(input), "解码长度不正确");
    TEST_ASSERT_EQ(memcmp(decoded, input, decoded_len), 0, "解码内容不正确");
}

/* 测试CRC32 */
static void test_crypto_crc32(vox_mpool_t* mpool) {
    (void)mpool;  /* 未使用的参数 */
    const char* input = "hello";
    uint32_t crc1 = vox_crc32(input, strlen(input));
    TEST_ASSERT_NE(crc1, 0, "CRC32计算失败");
    
    /* 相同输入应该得到相同结果 */
    uint32_t crc2 = vox_crc32(input, strlen(input));
    TEST_ASSERT_EQ(crc1, crc2, "相同输入应得到相同CRC32");
    
    /* 测试流式处理 */
    uint32_t crc3 = vox_crc32_init();
    crc3 = vox_crc32_update(crc3, "hel", 3);
    crc3 = vox_crc32_update(crc3, "lo", 2);
    crc3 = vox_crc32_final(crc3);
    
    TEST_ASSERT_EQ(crc1, crc3, "流式CRC32结果不正确");
}

/* 测试HMAC-MD5 */
static void test_crypto_hmac_md5(vox_mpool_t* mpool) {
    (void)mpool;  /* 未使用的参数 */
    const char* key = "secret";
    const char* data = "message";
    uint8_t digest[VOX_MD5_DIGEST_SIZE];
    
    vox_hmac_md5(key, strlen(key), data, strlen(data), digest);
    
    /* 验证结果不为全0 */
    int all_zero = 1;
    for (int i = 0; i < VOX_MD5_DIGEST_SIZE; i++) {
        if (digest[i] != 0) {
            all_zero = 0;
            break;
        }
    }
    TEST_ASSERT_EQ(all_zero, 0, "HMAC-MD5结果不应全为0");
}

/* 测试HMAC-SHA1 */
static void test_crypto_hmac_sha1(vox_mpool_t* mpool) {
    (void)mpool;  /* 未使用的参数 */
    const char* key = "secret";
    const char* data = "message";
    uint8_t digest[VOX_SHA1_DIGEST_SIZE];
    
    vox_hmac_sha1(key, strlen(key), data, strlen(data), digest);
    
    /* 验证结果不为全0 */
    int all_zero = 1;
    for (int i = 0; i < VOX_SHA1_DIGEST_SIZE; i++) {
        if (digest[i] != 0) {
            all_zero = 0;
            break;
        }
    }
    TEST_ASSERT_EQ(all_zero, 0, "HMAC-SHA1结果不应全为0");
}

/* 测试套件 */
test_case_t test_crypto_cases[] = {
    {"md5", test_crypto_md5},
    {"sha1", test_crypto_sha1},
    {"base64", test_crypto_base64},
    {"crc32", test_crypto_crc32},
    {"hmac_md5", test_crypto_hmac_md5},
    {"hmac_sha1", test_crypto_hmac_sha1},
};

test_suite_t test_crypto_suite = {
    "vox_crypto",
    test_crypto_cases,
    sizeof(test_crypto_cases) / sizeof(test_crypto_cases[0])
};
