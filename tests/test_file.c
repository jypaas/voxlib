/* ============================================================
 * test_file.c - vox_file 模块测试
 * ============================================================ */

#include "test_runner.h"
#include "../vox_file.h"
#include <string.h>

/* 测试文件打开和关闭 */
static void test_file_open_close(vox_mpool_t* mpool) {
    const char* test_file = "test_file_open.txt";
    
    /* 创建测试文件 */
    vox_file_t* file = vox_file_open(mpool, test_file, VOX_FILE_MODE_WRITE);
    TEST_ASSERT_NOT_NULL(file, "打开文件失败");
    TEST_ASSERT_EQ(vox_file_close(file), 0, "关闭文件失败");
    
    /* 清理 */
    vox_file_remove(mpool, test_file);
}

/* 测试文件读写 */
static void test_file_read_write(vox_mpool_t* mpool) {
    const char* test_file = "test_file_rw.txt";
    const char* content = "Hello, World!";
    
    /* 写入文件 */
    vox_file_t* file = vox_file_open(mpool, test_file, VOX_FILE_MODE_WRITE);
    TEST_ASSERT_NOT_NULL(file, "打开文件失败");
    
    int64_t written = vox_file_write(file, content, strlen(content));
    TEST_ASSERT_EQ(written, (int64_t)strlen(content), "写入文件失败");
    
    TEST_ASSERT_EQ(vox_file_close(file), 0, "关闭文件失败");
    
    /* 读取文件 */
    file = vox_file_open(mpool, test_file, VOX_FILE_MODE_READ);
    TEST_ASSERT_NOT_NULL(file, "打开文件失败");
    
    char buffer[100] = {0};
    int64_t read = vox_file_read(file, buffer, sizeof(buffer) - 1);
    TEST_ASSERT_EQ(read, (int64_t)strlen(content), "读取文件失败");
    TEST_ASSERT_STR_EQ(buffer, content, "读取内容不正确");
    
    TEST_ASSERT_EQ(vox_file_close(file), 0, "关闭文件失败");
    
    /* 清理 */
    vox_file_remove(mpool, test_file);
}

/* 测试文件定位 */
static void test_file_seek_tell(vox_mpool_t* mpool) {
    const char* test_file = "test_file_seek.txt";
    const char* content = "Hello, World!";
    
    vox_file_t* file = vox_file_open(mpool, test_file, VOX_FILE_MODE_WRITE);
    TEST_ASSERT_NOT_NULL(file, "打开文件失败");
    vox_file_write(file, content, strlen(content));
    vox_file_close(file);
    
    /* 测试seek和tell */
    file = vox_file_open(mpool, test_file, VOX_FILE_MODE_READ);
    TEST_ASSERT_NOT_NULL(file, "打开文件失败");
    
    int64_t pos = vox_file_tell(file);
    TEST_ASSERT_EQ(pos, 0, "初始位置应为0");
    
    vox_file_seek(file, 7, VOX_FILE_SEEK_SET);
    pos = vox_file_tell(file);
    TEST_ASSERT_EQ(pos, 7, "seek后位置不正确");
    
    char buffer[10] = {0};
    vox_file_read(file, buffer, 5);
    TEST_ASSERT_STR_EQ(buffer, "World", "读取内容不正确");
    
    vox_file_close(file);
    vox_file_remove(mpool, test_file);
}

/* 测试文件追加 */
static void test_file_append(vox_mpool_t* mpool) {
    const char* test_file = "test_file_append.txt";
    
    /* 写入初始内容 */
    vox_file_t* file = vox_file_open(mpool, test_file, VOX_FILE_MODE_WRITE);
    TEST_ASSERT_NOT_NULL(file, "打开文件失败");
    vox_file_write(file, "Hello", 5);
    vox_file_close(file);
    
    /* 追加内容 */
    file = vox_file_open(mpool, test_file, VOX_FILE_MODE_APPEND);
    TEST_ASSERT_NOT_NULL(file, "打开文件失败");
    vox_file_write(file, ", World!", 8);
    vox_file_close(file);
    
    /* 验证内容 */
    size_t size;
    char* content = (char*)vox_file_read_all(mpool, test_file, &size);
    TEST_ASSERT_NOT_NULL(content, "读取文件失败");
    TEST_ASSERT_STR_EQ(content, "Hello, World!", "追加后内容不正确");
    vox_mpool_free(mpool, content);
    
    vox_file_remove(mpool, test_file);
}

/* 测试文件信息 */
static void test_file_stat(vox_mpool_t* mpool) {
    const char* test_file = "test_file_stat.txt";
    
    vox_file_t* file = vox_file_open(mpool, test_file, VOX_FILE_MODE_WRITE);
    TEST_ASSERT_NOT_NULL(file, "打开文件失败");
    vox_file_write(file, "test", 4);
    vox_file_close(file);
    
    vox_file_info_t info;
    TEST_ASSERT_EQ(vox_file_stat(test_file, &info), 0, "获取文件信息失败");
    TEST_ASSERT_EQ(info.exists, 1, "文件应存在");
    TEST_ASSERT_EQ(info.is_regular_file, 1, "应为普通文件");
    TEST_ASSERT_EQ(info.size, 4, "文件大小不正确");
    
    vox_file_remove(mpool, test_file);
}

/* 测试文件存在性检查 */
static void test_file_exists(vox_mpool_t* mpool) {
    const char* test_file = "test_file_exists.txt";
    
    TEST_ASSERT_EQ(vox_file_exists(test_file), 0, "文件不应存在");
    
    vox_file_t* file = vox_file_open(mpool, test_file, VOX_FILE_MODE_WRITE);
    TEST_ASSERT_NOT_NULL(file, "打开文件失败");
    vox_file_close(file);
    
    TEST_ASSERT_EQ(vox_file_exists(test_file), 1, "文件应存在");
    
    vox_file_remove(mpool, test_file);
}

/* 测试文件复制和重命名 */
static void test_file_copy_rename(vox_mpool_t* mpool) {
    const char* src_file = "test_file_src.txt";
    const char* dst_file = "test_file_dst.txt";
    const char* new_file = "test_file_new.txt";
    const char* content = "test content";
    
    /* 创建源文件 */
    vox_file_t* file = vox_file_open(mpool, src_file, VOX_FILE_MODE_WRITE);
    TEST_ASSERT_NOT_NULL(file, "打开文件失败");
    vox_file_write(file, content, strlen(content));
    vox_file_close(file);
    
    /* 测试复制 */
    TEST_ASSERT_EQ(vox_file_copy(mpool, src_file, dst_file), 0, "复制文件失败");
    TEST_ASSERT_EQ(vox_file_exists(dst_file), 1, "目标文件应存在");
    
    size_t size;
    char* copied = (char*)vox_file_read_all(mpool, dst_file, &size);
    TEST_ASSERT_NOT_NULL(copied, "读取文件失败");
    TEST_ASSERT_STR_EQ(copied, content, "复制内容不正确");
    vox_mpool_free(mpool, copied);
    
    /* 测试重命名 */
    TEST_ASSERT_EQ(vox_file_rename(mpool, dst_file, new_file), 0, "重命名文件失败");
    TEST_ASSERT_EQ(vox_file_exists(dst_file), 0, "旧文件不应存在");
    TEST_ASSERT_EQ(vox_file_exists(new_file), 1, "新文件应存在");
    
    /* 清理 */
    vox_file_remove(mpool, src_file);
    vox_file_remove(mpool, new_file);
}

/* 测试路径操作 */
static void test_file_path_ops(vox_mpool_t* mpool) {
    const char* path1 = "/path/to";
    const char* path2 = "file.txt";
    
    char* joined = vox_file_join(mpool, path1, path2);
    TEST_ASSERT_NOT_NULL(joined, "连接路径失败");
    vox_mpool_free(mpool, joined);
    
    const char* full_path = "/path/to/file.txt";
    const char* basename = vox_file_basename(full_path);
    TEST_ASSERT_STR_EQ(basename, "file.txt", "获取basename失败");
    
    const char* ext = vox_file_ext(full_path);
    TEST_ASSERT_STR_EQ(ext, ".txt", "获取扩展名失败");
    
    char* dirname = vox_file_dirname(mpool, full_path);
    TEST_ASSERT_NOT_NULL(dirname, "获取dirname失败");
    vox_mpool_free(mpool, dirname);
}

/* 测试文件读取全部 */
static void test_file_read_all(vox_mpool_t* mpool) {
    const char* test_file = "test_file_readall.txt";
    const char* content = "This is a test file content.";
    
    vox_file_t* file = vox_file_open(mpool, test_file, VOX_FILE_MODE_WRITE);
    TEST_ASSERT_NOT_NULL(file, "打开文件失败");
    vox_file_write(file, content, strlen(content));
    vox_file_close(file);
    
    size_t size;
    char* read_content = (char*)vox_file_read_all(mpool, test_file, &size);
    TEST_ASSERT_NOT_NULL(read_content, "读取全部文件失败");
    TEST_ASSERT_EQ(size, strlen(content), "读取大小不正确");
    TEST_ASSERT_STR_EQ(read_content, content, "读取内容不正确");
    
    vox_mpool_free(mpool, read_content);
    vox_file_remove(mpool, test_file);
}

/* 测试文件写入全部 */
static void test_file_write_all(vox_mpool_t* mpool) {
    const char* test_file = "test_file_writeall.txt";
    const char* content = "This is written all at once.";
    
    TEST_ASSERT_EQ(vox_file_write_all(mpool, test_file, content, strlen(content)), 0, "写入全部文件失败");
    
    size_t size;
    char* read_content = (char*)vox_file_read_all(mpool, test_file, &size);
    TEST_ASSERT_NOT_NULL(read_content, "读取文件失败");
    TEST_ASSERT_STR_EQ(read_content, content, "写入内容不正确");
    
    vox_mpool_free(mpool, read_content);
    vox_file_remove(mpool, test_file);
}

/* 测试套件 */
test_case_t test_file_cases[] = {
    {"open_close", test_file_open_close},
    {"read_write", test_file_read_write},
    {"seek_tell", test_file_seek_tell},
    {"append", test_file_append},
    {"stat", test_file_stat},
    {"exists", test_file_exists},
    {"copy_rename", test_file_copy_rename},
    {"path_ops", test_file_path_ops},
    {"read_all", test_file_read_all},
    {"write_all", test_file_write_all},
};

test_suite_t test_file_suite = {
    "vox_file",
    test_file_cases,
    sizeof(test_file_cases) / sizeof(test_file_cases[0])
};
