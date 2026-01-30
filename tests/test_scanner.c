/* ============================================================
 * test_scanner.c - vox_scanner 模块测试
 * ============================================================ */

#include "test_runner.h"
#include "../vox_scanner.h"
#include "../vox_string.h"
#include <string.h>

/* 测试创建和初始化 */
static void test_scanner_init(vox_mpool_t* mpool) {
    char* buf = (char*)vox_mpool_alloc(mpool, 100);
    strcpy(buf, "Hello, World!");
    
    vox_scanner_t scanner;
    TEST_ASSERT_EQ(vox_scanner_init(&scanner, buf, strlen(buf), VOX_SCANNER_NONE), 0, "初始化scanner失败");
    
    TEST_ASSERT_EQ(vox_scanner_remaining(&scanner), strlen(buf), "剩余长度不正确");
    TEST_ASSERT_EQ(vox_scanner_eof(&scanner), 0, "不应在EOF");
    
    vox_scanner_destroy(&scanner);
}

/* 测试字符集 */
static void test_scanner_charset(vox_mpool_t* mpool) {
    (void)mpool;  /* 未使用的参数 */
    vox_charset_t cs;
    vox_charset_init(&cs);
    
    vox_charset_add_char(&cs, 'a');
    TEST_ASSERT_EQ(vox_charset_contains(&cs, 'a'), 1, "字符集应包含'a'");
    TEST_ASSERT_EQ(vox_charset_contains(&cs, 'b'), 0, "字符集不应包含'b'");
    
    vox_charset_add_range(&cs, '0', '9');
    TEST_ASSERT_EQ(vox_charset_contains(&cs, '5'), 1, "字符集应包含'5'");
    
    vox_charset_add_alpha(&cs);
    TEST_ASSERT_EQ(vox_charset_contains(&cs, 'z'), 1, "字符集应包含字母");
    
    vox_charset_add_digit(&cs);
    TEST_ASSERT_EQ(vox_charset_contains(&cs, '9'), 1, "字符集应包含数字");
}

/* 测试扫描字符 */
static void test_scanner_scan_char(vox_mpool_t* mpool) {
    char* buf = (char*)vox_mpool_alloc(mpool, 100);
    strcpy(buf, "ABC");
    
    vox_scanner_t scanner;
    vox_scanner_init(&scanner, buf, strlen(buf), VOX_SCANNER_NONE);
    
    int ch = vox_scanner_get_char(&scanner);
    TEST_ASSERT_EQ(ch, 'A', "扫描字符失败");
    
    ch = vox_scanner_peek_char(&scanner);
    TEST_ASSERT_EQ(ch, 'B', "peek字符失败");
    
    ch = vox_scanner_get_char(&scanner);
    TEST_ASSERT_EQ(ch, 'B', "扫描字符失败");
    
    vox_scanner_destroy(&scanner);
}

/* 测试扫描字符串 */
static void test_scanner_scan_string(vox_mpool_t* mpool) {
    char* buf = (char*)vox_mpool_alloc(mpool, 100);
    strcpy(buf, "Hello World");
    
    vox_scanner_t scanner;
    vox_scanner_init(&scanner, buf, strlen(buf), VOX_SCANNER_NONE);
    
    vox_strview_t view;
    TEST_ASSERT_EQ(vox_scanner_get_until_char(&scanner, ' ', false, &view), 0, "扫描字符串失败");
    TEST_ASSERT_EQ(view.len, 5, "扫描字符串长度不正确");
    TEST_ASSERT_EQ(memcmp(view.ptr, "Hello", 5), 0, "扫描字符串内容不正确");
    
    vox_scanner_skip(&scanner, 1);  /* 跳过空格 */
    
    TEST_ASSERT_EQ(vox_scanner_get_until_char(&scanner, '\0', false, &view), 0, "扫描字符串失败");
    TEST_ASSERT_EQ(view.len, 5, "扫描字符串长度不正确");
    TEST_ASSERT_EQ(memcmp(view.ptr, "World", 5), 0, "扫描字符串内容不正确");
    
    vox_scanner_destroy(&scanner);
}

/* 测试跳过字符 */
static void test_scanner_skip(vox_mpool_t* mpool) {
    char* buf = (char*)vox_mpool_alloc(mpool, 100);
    strcpy(buf, "   Hello");
    
    vox_scanner_t scanner;
    vox_scanner_init(&scanner, buf, strlen(buf), VOX_SCANNER_AUTOSKIP_WS);
    
    /* 自动跳过空白字符后，应该指向'H' */
    int ch = vox_scanner_peek_char(&scanner);
    TEST_ASSERT_EQ(ch, 'H', "自动跳过空白字符失败");
    
    vox_scanner_destroy(&scanner);
}

/* 测试EOF */
static void test_scanner_eof(vox_mpool_t* mpool) {
    char* buf = (char*)vox_mpool_alloc(mpool, 100);
    strcpy(buf, "A");
    
    vox_scanner_t scanner;
    vox_scanner_init(&scanner, buf, strlen(buf), VOX_SCANNER_NONE);
    
    vox_scanner_get_char(&scanner);
    TEST_ASSERT_EQ(vox_scanner_eof(&scanner), 1, "应该在EOF");
    
    vox_scanner_destroy(&scanner);
}

/* 测试套件 */
test_case_t test_scanner_cases[] = {
    {"init", test_scanner_init},
    {"charset", test_scanner_charset},
    {"scan_char", test_scanner_scan_char},
    {"scan_string", test_scanner_scan_string},
    {"skip", test_scanner_skip},
    {"eof", test_scanner_eof},
};

test_suite_t test_scanner_suite = {
    "vox_scanner",
    test_scanner_cases,
    sizeof(test_scanner_cases) / sizeof(test_scanner_cases[0])
};
