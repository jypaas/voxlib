/*
 * file_example.c - 文件操作示例程序
 * 演示 vox_file 的基本用法
 */

#include "../vox_file.h"
#include "../vox_mpool.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* 目录遍历回调函数 */
static int walk_callback(const char* path, const vox_file_info_t* info, void* user_data) {
    (void)user_data;
    if (info->is_directory) {
        printf("  [目录] %s\n", path);
    } else {
        printf("  [文件] %s (大小: %lld 字节)\n", path, (long long)info->size);
    }
    return 0;  /* 继续遍历 */
}

int main(void) {
    /* 创建内存池 */
    vox_mpool_t* mpool = vox_mpool_create();
    if (!mpool) {
        fprintf(stderr, "创建内存池失败\n");
        return 1;
    }
    
    printf("=== 测试文件读写 ===\n");
    const char* test_file = "test_file.txt";
    
    /* 写入文件 */
    vox_file_t* file = vox_file_open(mpool, test_file, VOX_FILE_MODE_WRITE);
    if (!file) {
        fprintf(stderr, "打开文件失败: %s\n", test_file);
        vox_mpool_destroy(mpool);
        return 1;
    }
    
    const char* content = "Hello, World!\nThis is a test file.\n";
    int64_t written = vox_file_write(file, content, strlen(content));
    printf("写入 %lld 字节到文件\n", (long long)written);
    
    vox_file_close(file);
    
    /* 读取文件 */
    file = vox_file_open(mpool, test_file, VOX_FILE_MODE_READ);
    if (file) {
        char buffer[256];
        int64_t read_bytes = vox_file_read(file, buffer, sizeof(buffer) - 1);
        if (read_bytes > 0) {
            buffer[read_bytes] = '\0';
            printf("读取内容: %s", buffer);
        }
        vox_file_close(file);
    }
    
    /* 使用便捷函数读取整个文件 */
    printf("\n=== 使用便捷函数读取整个文件 ===\n");
    size_t file_size = 0;
    char* file_content = (char*)vox_file_read_all(mpool, test_file, &file_size);
    if (file_content) {
        printf("文件大小: %zu 字节\n", file_size);
        printf("文件内容: %s", file_content);
        vox_mpool_free(mpool, file_content);
    }
    
    printf("\n=== 测试文件信息 ===\n");
    vox_file_info_t info;
    if (vox_file_stat(test_file, &info) == 0) {
        printf("文件存在: %s\n", info.exists ? "是" : "否");
        printf("是目录: %s\n", info.is_directory ? "是" : "否");
        printf("是普通文件: %s\n", info.is_regular_file ? "是" : "否");
        printf("文件大小: %lld 字节\n", (long long)info.size);
        printf("修改时间: %lld\n", (long long)info.modified_time);
    }
    
    printf("\n=== 测试文件定位 ===\n");
    file = vox_file_open(mpool, test_file, VOX_FILE_MODE_READ);
    if (file) {
        int64_t pos = vox_file_tell(file);
        printf("当前位置: %lld\n", (long long)pos);
        
        vox_file_seek(file, 7, VOX_FILE_SEEK_SET);
        pos = vox_file_tell(file);
        printf("定位到位置 7，当前位置: %lld\n", (long long)pos);
        
        char buffer[32];
        int64_t read_bytes = vox_file_read(file, buffer, sizeof(buffer) - 1);
        if (read_bytes > 0) {
            buffer[read_bytes] = '\0';
            printf("从位置 7 读取: %s", buffer);
        }
        
        vox_file_close(file);
    }
    
    printf("\n=== 测试文件追加 ===\n");
    file = vox_file_open(mpool, test_file, VOX_FILE_MODE_APPEND);
    if (file) {
        const char* append_content = "Appended line.\n";
        vox_file_write(file, append_content, strlen(append_content));
        vox_file_close(file);
        
        /* 读取追加后的内容 */
        file_content = (char*)vox_file_read_all(mpool, test_file, NULL);
        if (file_content) {
            printf("追加后的内容:\n%s", file_content);
            vox_mpool_free(mpool, file_content);
        }
    }
    
    printf("\n=== 测试路径操作 ===\n");
    const char* test_path = "/path/to/file.txt";
    printf("路径: %s\n", test_path);
    printf("路径分隔符: %c\n", vox_file_separator());
    printf("文件名: %s\n", vox_file_basename(test_path));
    printf("扩展名: %s\n", vox_file_ext(test_path) ? vox_file_ext(test_path) : "(无)");
    
    char* dirname = vox_file_dirname(mpool, test_path);
    if (dirname) {
        printf("目录名: %s\n", dirname);
        vox_mpool_free(mpool, dirname);
    }
    
    char* joined = vox_file_join(mpool, "/path/to", "file.txt");
    if (joined) {
        printf("连接路径: %s\n", joined);
        vox_mpool_free(mpool, joined);
    }
    
    char* normalized = vox_file_normalize(mpool, "/path/to/../other/./file.txt");
    if (normalized) {
        printf("规范化路径: %s -> %s\n", "/path/to/../other/./file.txt", normalized);
        vox_mpool_free(mpool, normalized);
    }
    
    printf("\n=== 测试当前工作目录 ===\n");
    char* cwd = vox_file_getcwd(mpool);
    if (cwd) {
        printf("当前工作目录: %s\n", cwd);
        
        /* 测试更改工作目录（如果可能） */
        printf("尝试更改工作目录...\n");
        /* 注意：在实际测试中，更改工作目录可能会影响后续操作，所以这里只是演示 */
        /* 在实际应用中，应该保存原始目录并在测试后恢复 */
        
        vox_mpool_free(mpool, cwd);
    } else {
        printf("获取当前工作目录失败\n");
    }
    
    printf("\n=== 测试文件复制 ===\n");
    const char* src_file = test_file;
    const char* dst_file = "test_file_copy.txt";
    if (vox_file_copy(mpool, src_file, dst_file) == 0) {
        printf("文件复制成功: %s -> %s\n", src_file, dst_file);
        
        /* 验证复制结果 */
        size_t src_size = 0, dst_size = 0;
        char* src_content = (char*)vox_file_read_all(mpool, src_file, &src_size);
        char* dst_content = (char*)vox_file_read_all(mpool, dst_file, &dst_size);
        
        if (src_content && dst_content && src_size == dst_size && 
            memcmp(src_content, dst_content, src_size) == 0) {
            printf("复制验证成功，文件内容相同\n");
        }
        
        if (src_content) vox_mpool_free(mpool, src_content);
        if (dst_content) vox_mpool_free(mpool, dst_content);
    } else {
        printf("文件复制失败\n");
    }
    
    printf("\n=== 测试文件重命名 ===\n");
    const char* old_name = "test_old.txt";
    const char* new_name = "test_new.txt";
    
    /* 先创建一个文件用于重命名测试 */
    if (vox_file_write_all(mpool, old_name, "Test rename content", 19) == 0) {
        printf("创建测试文件: %s\n", old_name);
        
        if (vox_file_rename(mpool, old_name, new_name) == 0) {
            printf("文件重命名成功: %s -> %s\n", old_name, new_name);
            
            /* 验证重命名结果 */
            if (vox_file_exists(new_name)) {
                printf("重命名验证成功，新文件存在\n");
                vox_file_remove(mpool, new_name);
            }
        } else {
            printf("文件重命名失败\n");
            vox_file_remove(mpool, old_name);
        }
    }
    
    printf("\n=== 测试目录操作 ===\n");
    const char* test_dir = "test_dir";
    if (vox_file_mkdir(mpool, test_dir, false) == 0) {
        printf("创建目录成功: %s\n", test_dir);
        
        /* 在目录中创建文件 */
        char* test_file_in_dir = vox_file_join(mpool, test_dir, "test.txt");
        if (test_file_in_dir) {
            if (vox_file_write_all(mpool, test_file_in_dir, "Test content", 12) == 0) {
                printf("在目录中创建文件: %s\n", test_file_in_dir);
            }
            vox_mpool_free(mpool, test_file_in_dir);
        }
        
        /* 测试递归创建目录 */
        const char* nested_dir = "test_dir/nested/deep";
        if (vox_file_mkdir(mpool, nested_dir, true) == 0) {
            printf("递归创建目录成功: %s\n", nested_dir);
        }
        
        /* 遍历目录 */
        printf("遍历目录 %s:\n", test_dir);
        int count = vox_file_walk(mpool, test_dir, walk_callback, NULL);
        printf("共找到 %d 个文件/目录\n", count);
        
        /* 删除目录（递归） */
        if (vox_file_rmdir(mpool, test_dir, true) == 0) {
            printf("删除目录成功: %s\n", test_dir);
        } else {
            printf("删除目录失败\n");
        }
    } else {
        printf("创建目录失败\n");
    }
    
    printf("\n=== 清理测试文件 ===\n");
    vox_file_remove(mpool, test_file);
    vox_file_remove(mpool, dst_file);
    printf("清理完成\n");
    
    /* 销毁内存池 */
    vox_mpool_destroy(mpool);
    
    printf("\n所有测试完成！\n");
    return 0;
}
