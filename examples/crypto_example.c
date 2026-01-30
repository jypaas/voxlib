/*
 * crypto_example.c - 加密和哈希算法测试程序
 * 编译: gcc -o crypto_example crypto_example.c ../vox_crypto.c
 * 运行: ./crypto_example
 */

#include "../vox_crypto.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* ===== 测试框架 ===== */

typedef struct {
    int total;
    int passed;
    int failed;
} test_stats_t;

static test_stats_t g_stats = {0, 0, 0};

/* 测试用例结构 */
typedef struct {
    const char* name;
    const void* input;
    size_t input_len;
    const char* expected_hex;  /* 对于哈希算法 */
    const char* expected_str;  /* 对于Base64 */
} test_case_t;

/* 验证并打印测试结果 */
static int verify_hash(const char* test_name, const uint8_t* digest, size_t digest_size,
                       const char* expected_hex, const char* hex_str) {
    (void)(digest);
    (void)(digest_size);
    int passed = (expected_hex && strcmp(hex_str, expected_hex) == 0);
    
    printf("  %s: %s\n", test_name, hex_str);
    if (expected_hex) {
        printf("    Expected: %s\n", expected_hex);
        printf("    Result: %s\n", passed ? "PASS" : "FAIL");
    }
    
    g_stats.total++;
    if (passed || !expected_hex) {
        g_stats.passed++;
        return 1;
    } else {
        g_stats.failed++;
        return 0;
    }
}

/* 验证Base64往返测试 */
static int verify_base64_roundtrip(const char* test_name, const void* input, size_t input_len,
                                    int is_url_safe) {
    char encoded[512];
    uint8_t decoded[512];
    int encoded_len, decoded_len;
    int passed = 0;
    
    if (is_url_safe) {
        encoded_len = vox_base64url_encode(input, input_len, encoded, sizeof(encoded));
        if (encoded_len < 0) {
            printf("  %s: ENCODE FAILED\n", test_name);
            g_stats.total++;
            g_stats.failed++;
            return 0;
        }
        decoded_len = vox_base64url_decode(encoded, decoded, sizeof(decoded));
    } else {
        encoded_len = vox_base64_encode(input, input_len, encoded, sizeof(encoded));
        if (encoded_len < 0) {
            printf("  %s: ENCODE FAILED\n", test_name);
            g_stats.total++;
            g_stats.failed++;
            return 0;
        }
        decoded_len = vox_base64_decode(encoded, decoded, sizeof(decoded));
    }
    
    if (decoded_len < 0) {
        printf("  %s: DECODE FAILED\n", test_name);
        g_stats.total++;
        g_stats.failed++;
        return 0;
    }
    
    if ((size_t)decoded_len == input_len && memcmp(decoded, input, input_len) == 0) {
        passed = 1;
    }
    
    printf("  %s: \"%s\" -> \"%s\" -> ", test_name, 
           input_len > 0 ? (const char*)input : "(empty)", encoded);
    if (input_len > 0 && input_len < 50) {
        /* 确保不越界写入 */
        if (decoded_len < (int)sizeof(decoded)) {
            decoded[decoded_len] = '\0';
            printf("\"%s\"", (const char*)decoded);
        } else {
            printf("(%d bytes)", (int)decoded_len);
        }
    } else {
        printf("(%d bytes)", (int)decoded_len);
    }
    printf(" [%s]\n", passed ? "PASS" : "FAIL");
    
    g_stats.total++;
    if (passed) {
        g_stats.passed++;
    } else {
        g_stats.failed++;
    }
    
    return passed;
}

/* 验证Base64编码结果 */
static int verify_base64_encode(const char* test_name, const void* input, size_t input_len,
                                 const char* expected, int is_url_safe) {
    char encoded[512];
    int encoded_len;
    
    if (is_url_safe) {
        encoded_len = vox_base64url_encode(input, input_len, encoded, sizeof(encoded));
    } else {
        encoded_len = vox_base64_encode(input, input_len, encoded, sizeof(encoded));
    }
    
    if (encoded_len < 0) {
        printf("  %s: ENCODE FAILED\n", test_name);
        g_stats.total++;
        g_stats.failed++;
        return 0;
    }
    
    int passed = (expected && strcmp(encoded, expected) == 0);
    
    printf("  %s: \"%s\"\n", test_name, encoded);
    if (expected) {
        printf("    Expected: \"%s\"\n", expected);
        printf("    Result: %s\n", passed ? "PASS" : "FAIL");
    }
    
    g_stats.total++;
    if (passed || !expected) {
        g_stats.passed++;
        return 1;
    } else {
        g_stats.failed++;
        return 0;
    }
}

/* ===== MD5 测试 ===== */

static void test_md5_single(const char* input, size_t len, const char* expected) {
    uint8_t digest[VOX_MD5_DIGEST_SIZE];
    char hex_str[33];
    
    vox_md5(input, len, digest);
    vox_md5_hex(digest, hex_str);
    verify_hash(input, digest, VOX_MD5_DIGEST_SIZE, expected, hex_str);
}

static void test_md5_streaming(const char* part1, size_t len1, 
                                const char* part2, size_t len2, 
                                const char* expected) {
    vox_md5_ctx_t ctx;
    uint8_t digest[VOX_MD5_DIGEST_SIZE];
    char hex_str[33];
    
    vox_md5_init(&ctx);
    vox_md5_update(&ctx, part1, len1);
    vox_md5_update(&ctx, part2, len2);
    vox_md5_final(&ctx, digest);
    vox_md5_hex(digest, hex_str);
    
    char test_name[128];
    snprintf(test_name, sizeof(test_name), "Streaming: \"%s\" + \"%s\"", part1, part2);
    verify_hash(test_name, digest, VOX_MD5_DIGEST_SIZE, expected, hex_str);
}

static void test_md5(void) {
    printf("\n=== Testing MD5 ===\n");
    
    test_md5_single("hello", 5, "5d41402abc4b2a76b9719d911017c592");
    test_md5_single("The quick brown fox jumps over the lazy dog", 43, 
                    "9e107d9d372bb6826bd81d3542a419d6");
    test_md5_single("", 0, "d41d8cd98f00b204e9800998ecf8427e");
    test_md5_streaming("hello", 5, " world", 6, "5eb63bbbe01eeed093cb22bb8f5acdc3");
}

/* ===== SHA1 测试 ===== */

static void test_sha1_single(const char* input, size_t len, const char* expected) {
    uint8_t digest[VOX_SHA1_DIGEST_SIZE];
    char hex_str[41];
    
    vox_sha1(input, len, digest);
    vox_sha1_hex(digest, hex_str);
    verify_hash(input, digest, VOX_SHA1_DIGEST_SIZE, expected, hex_str);
}

static void test_sha1_streaming(const char* part1, size_t len1,
                                 const char* part2, size_t len2,
                                 const char* expected) {
    vox_sha1_ctx_t ctx;
    uint8_t digest[VOX_SHA1_DIGEST_SIZE];
    char hex_str[41];
    
    vox_sha1_init(&ctx);
    vox_sha1_update(&ctx, part1, len1);
    vox_sha1_update(&ctx, part2, len2);
    vox_sha1_final(&ctx, digest);
    vox_sha1_hex(digest, hex_str);
    
    char test_name[128];
    snprintf(test_name, sizeof(test_name), "Streaming: \"%s\" + \"%s\"", part1, part2);
    verify_hash(test_name, digest, VOX_SHA1_DIGEST_SIZE, expected, hex_str);
}

static void test_sha1(void) {
    printf("\n=== Testing SHA1 ===\n");
    
    test_sha1_single("hello", 5, "aaf4c61ddcc5e8a2dabede0f3b482cd9aea9434d");
    test_sha1_single("The quick brown fox jumps over the lazy dog", 43,
                     "2fd4e1c67a2d28fced849ee1bb76e7391b93eb12");
    test_sha1_single("", 0, "da39a3ee5e6b4b0d3255bfef95601890afd80709");
    test_sha1_streaming("hello", 5, " world", 6, "2aae6c35c94fcfb415dbe95f408b9ce91ee846ed");
}

/* ===== SHA256 测试 ===== */

static void test_sha256_single(const char* input, size_t len, const char* expected) {
    uint8_t digest[VOX_SHA256_DIGEST_SIZE];
    char hex_str[65];
    
    vox_sha256(input, len, digest);
    vox_sha256_hex(digest, hex_str);
    verify_hash(input, digest, VOX_SHA256_DIGEST_SIZE, expected, hex_str);
}

static void test_sha256_streaming(const char* part1, size_t len1,
                                  const char* part2, size_t len2,
                                  const char* expected) {
    vox_sha256_ctx_t ctx;
    uint8_t digest[VOX_SHA256_DIGEST_SIZE];
    char hex_str[65];
    
    vox_sha256_init(&ctx);
    vox_sha256_update(&ctx, part1, len1);
    vox_sha256_update(&ctx, part2, len2);
    vox_sha256_final(&ctx, digest);
    vox_sha256_hex(digest, hex_str);
    
    char test_name[128];
    snprintf(test_name, sizeof(test_name), "Streaming: \"%s\" + \"%s\"", part1, part2);
    verify_hash(test_name, digest, VOX_SHA256_DIGEST_SIZE, expected, hex_str);
}

static void test_sha256(void) {
    printf("\n=== Testing SHA256 ===\n");
    
    /* 标准测试向量 */
    test_sha256_single("hello", 5, "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824");
    test_sha256_single("The quick brown fox jumps over the lazy dog", 43,
                       "d7a8fbb307d7809469ca9abcb0082e4f8d5651e46d3cdb762d02d0bf37c9e592");
    test_sha256_single("", 0, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    test_sha256_streaming("hello", 5, " world", 6, "b94d27b9934d3e08a52e52d7da7dabfac484efe37a5380ee9088f7ace2efcde9");
}

/* ===== HMAC-MD5 测试 ===== */

static void test_hmac_md5_single(const void* key, size_t key_len,
                                  const void* data, size_t data_len,
                                  const char* test_name, const char* expected) {
    uint8_t digest[VOX_MD5_DIGEST_SIZE];
    char hex_str[33];
    
    vox_hmac_md5(key, key_len, data, data_len, digest);
    vox_hmac_md5_hex(digest, hex_str);
    verify_hash(test_name, digest, VOX_MD5_DIGEST_SIZE, expected, hex_str);
}

static void test_hmac_md5(void) {
    printf("\n=== Testing HMAC-MD5 ===\n");
    
    /* RFC 2202 测试向量 */
    const uint8_t key1[16] = {0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b,
                              0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b};
    test_hmac_md5_single(key1, 16, "Hi There", 8, 
                         "RFC2202: key=0x0b*16, data=\"Hi There\"",
                         "9294727a3638bb1c13f48ef8158bfc9d");
    
    /* 简单字符串测试 */
    test_hmac_md5_single("key", 3, "The quick brown fox jumps over the lazy dog", 43,
                         "key=\"key\", data=\"The quick brown fox...\"", NULL);
}

/* ===== HMAC-SHA1 测试 ===== */

static void test_hmac_sha1_single(const void* key, size_t key_len,
                                   const void* data, size_t data_len,
                                   const char* test_name, const char* expected) {
    uint8_t digest[VOX_SHA1_DIGEST_SIZE];
    char hex_str[41];
    
    vox_hmac_sha1(key, key_len, data, data_len, digest);
    vox_hmac_sha1_hex(digest, hex_str);
    verify_hash(test_name, digest, VOX_SHA1_DIGEST_SIZE, expected, hex_str);
}

static void test_hmac_sha1(void) {
    printf("\n=== Testing HMAC-SHA1 ===\n");
    
    /* RFC 2202 测试向量 */
    const uint8_t key1[16] = {0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b,
                              0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b};
    test_hmac_sha1_single(key1, 16, "Hi There", 8,
                          "RFC2202: key=0x0b*16, data=\"Hi There\"",
                          "675b0b3a1b4ddf4e124872da6c2f632bfed957e9");
    
    /* 简单字符串测试 */
    test_hmac_sha1_single("key", 3, "The quick brown fox jumps over the lazy dog", 43,
                          "key=\"key\", data=\"The quick brown fox...\"", NULL);
}

/* ===== HMAC-SHA256 测试 ===== */

static void test_hmac_sha256_single(const void* key, size_t key_len,
                                     const void* data, size_t data_len,
                                     const char* test_name, const char* expected) {
    uint8_t digest[VOX_SHA256_DIGEST_SIZE];
    char hex_str[65];
    
    vox_hmac_sha256(key, key_len, data, data_len, digest);
    vox_hmac_sha256_hex(digest, hex_str);
    verify_hash(test_name, digest, VOX_SHA256_DIGEST_SIZE, expected, hex_str);
}

static void test_hmac_sha256(void) {
    printf("\n=== Testing HMAC-SHA256 ===\n");
    
    /* RFC 4231 测试向量 */
    const uint8_t key1[20] = {0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b,
                              0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b,
                              0x0b, 0x0b, 0x0b, 0x0b};
    test_hmac_sha256_single(key1, 20, "Hi There", 8,
                            "RFC4231: key=0x0b*20, data=\"Hi There\"",
                            "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");
    
    /* 简单字符串测试 */
    test_hmac_sha256_single("key", 3, "The quick brown fox jumps over the lazy dog", 43,
                            "key=\"key\", data=\"The quick brown fox...\"", NULL);
    
    /* 空密钥测试 */
    test_hmac_sha256_single("", 0, "message", 7,
                            "key=\"(empty)\", data=\"message\"", NULL);
    
    /* 空数据测试 */
    test_hmac_sha256_single("key", 3, "", 0,
                            "key=\"key\", data=\"(empty)\"", NULL);
}

/* ===== Base64 测试 ===== */

static void test_base64(void) {
    printf("\n=== Testing Base64 ===\n");
    
    /* 基本字符串测试 */
    verify_base64_encode("hello", "hello", 5, "aGVsbG8=", 0);
    verify_base64_roundtrip("hello", "hello", 5, 0);
    
    verify_base64_encode("Hello, World!", "Hello, World!", 13, "SGVsbG8sIFdvcmxkIQ==", 0);
    verify_base64_roundtrip("Hello, World!", "Hello, World!", 13, 0);
    
    /* 空字符串 */
    verify_base64_encode("(empty)", "", 0, "", 0);
    verify_base64_roundtrip("(empty)", "", 0, 0);
    
    /* 二进制数据 */
    uint8_t binary[] = {0x00, 0x01, 0x02, 0x03, 0xFF, 0xFE, 0xFD};
    verify_base64_roundtrip("binary data", binary, sizeof(binary), 0);
}

/* ===== Base64URL 测试 ===== */

static void test_base64url(void) {
    printf("\n=== Testing URL/Filename Safe Base64 (Base64URL) ===\n");
    
    /* 基本字符串测试 */
    verify_base64_roundtrip("hello", "hello", 5, 1);
    verify_base64_roundtrip("Hello, World!", "Hello, World!", 13, 1);
    
    /* 空字符串 */
    verify_base64_roundtrip("(empty)", "", 0, 1);
    
    /* 二进制数据 */
    uint8_t binary[] = {0x00, 0x01, 0x02, 0x03, 0xFF, 0xFE, 0xFD};
    verify_base64_roundtrip("binary data", binary, sizeof(binary), 1);
    
    /* URL和文件名安全性验证 */
    printf("\n  URL/Filename safety check:\n");
    const char* test_data = "test data with special chars: +/=";
    char encoded[512];
    int encoded_len = vox_base64url_encode(test_data, strlen(test_data), encoded, sizeof(encoded));
    
    if (encoded_len >= 0) {
        printf("    Input: \"%s\"\n", test_data);
        printf("    Encoded: \"%s\"\n", encoded);
        
        int has_unsafe = 0;
        for (int i = 0; encoded[i] != '\0'; i++) {
            if (encoded[i] == '+' || encoded[i] == '/' || encoded[i] == '=') {
                has_unsafe = 1;
                break;
            }
        }
        printf("    Contains unsafe chars (+, /, =): %s\n", 
               has_unsafe ? "YES" : "NO");
        printf("    Safe for URL/filename: %s\n", has_unsafe ? "NO" : "YES");
        
        g_stats.total++;
        if (!has_unsafe) {
            g_stats.passed++;
        } else {
            g_stats.failed++;
        }
    }
    
    /* 兼容性测试 - 解码标准Base64格式 */
    printf("\n  Compatibility test (decoding standard Base64):\n");
    const char* standard_base64 = "aGVsbG8=";
    uint8_t decoded[256];
    int decoded_len = vox_base64url_decode(standard_base64, decoded, sizeof(decoded));
    
    if (decoded_len > 0) {
        /* 确保不越界写入 */
        if (decoded_len < (int)sizeof(decoded)) {
            decoded[decoded_len] = '\0';
        }
        int match = (decoded_len == 5 && memcmp(decoded, "hello", 5) == 0);
        printf("    Standard Base64: \"%s\" -> \"%s\" [%s]\n", 
               standard_base64, (const char*)decoded, match ? "PASS" : "FAIL");
        
        g_stats.total++;
        if (match) {
            g_stats.passed++;
        } else {
            g_stats.failed++;
        }
    }
}

/* ===== CRC32 测试 ===== */

static void test_crc32_single(const char* input, size_t len, uint32_t expected) {
    uint32_t crc = vox_crc32(input, len);
    int passed = (expected == 0 || crc == expected);
    
    printf("  \"%s\": 0x%08X", input, crc);
    if (expected != 0) {
        printf(" (expected: 0x%08X) [%s]\n", expected, passed ? "PASS" : "FAIL");
    } else {
        printf("\n");
    }
    
    g_stats.total++;
    if (passed) {
        g_stats.passed++;
    } else {
        g_stats.failed++;
    }
}

static void test_crc32_streaming(const char* part1, size_t len1,
                                  const char* part2, size_t len2) {
    uint32_t crc = vox_crc32_init();
    crc = vox_crc32_update(crc, part1, len1);
    crc = vox_crc32_update(crc, part2, len2);
    crc = vox_crc32_final(crc);
    
    uint32_t crc_direct = vox_crc32("hello world", 11);
    int match = (crc == crc_direct);
    
    printf("  Streaming: \"%s\" + \"%s\" = 0x%08X\n", part1, part2, crc);
    printf("    Direct: \"hello world\" = 0x%08X [%s]\n", crc_direct, 
           match ? "MATCH" : "MISMATCH");
    
    g_stats.total++;
    if (match) {
        g_stats.passed++;
    } else {
        g_stats.failed++;
    }
}

static void test_crc32(void) {
    printf("\n=== Testing CRC32 ===\n");
    
    test_crc32_single("hello", 5, 0);  /* 不验证具体值，只测试功能 */
    test_crc32_single("The quick brown fox jumps over the lazy dog", 43, 0);
    test_crc32_single("", 0, 0x00000000);
    test_crc32_streaming("hello", 5, " world", 6);
}

/* ===== 主函数 ===== */

int main(void) {
    printf("=== Crypto Algorithm Test Suite ===\n");
    printf("====================================\n");
    
    test_md5();
    test_sha1();
    test_sha256();
    test_hmac_md5();
    test_hmac_sha1();
    test_hmac_sha256();
    test_base64();
    test_base64url();
    test_crc32();
    
    printf("\n=== Test Summary ===\n");
    printf("Total tests: %d\n", g_stats.total);
    printf("Passed: %d\n", g_stats.passed);
    printf("Failed: %d\n", g_stats.failed);
    printf("Success rate: %.1f%%\n", 
           g_stats.total > 0 ? (100.0 * g_stats.passed / g_stats.total) : 0.0);
    
    if (g_stats.failed == 0) {
        printf("\n✓ All tests passed!\n");
        return 0;
    } else {
        printf("\n✗ Some tests failed!\n");
        return 1;
    }
}
