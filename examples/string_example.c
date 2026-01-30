/*
 * string_example.c - 字符串处理示例程序
 * 演示 vox_string 的基本用法
 */

#include "../vox_string.h"
#include "../vox_mpool.h"
#include <stdio.h>
#include <string.h>

int main(void) {
    /* 创建内存池 */
    vox_mpool_t* mpool = vox_mpool_create();
    if (!mpool) {
        fprintf(stderr, "创建内存池失败\n");
        return 1;
    }
    
    printf("=== 创建字符串 ===\n");
    vox_string_t* str = vox_string_create(mpool);
    if (!str) {
        fprintf(stderr, "创建字符串失败\n");
        return 1;
    }
    printf("空字符串创建成功，长度: %zu, 容量: %zu\n", 
           vox_string_length(str), vox_string_capacity(str));
    
    printf("\n=== 从C字符串创建 ===\n");
    vox_string_t* str1 = vox_string_from_cstr(mpool, "Hello, World!");
    if (str1) {
        printf("从C字符串创建: \"%s\" (长度: %zu)\n", 
               vox_string_cstr(str1), vox_string_length(str1));
    }
    
    printf("\n=== 设置字符串内容 ===\n");
    vox_string_set(str, "Hello");
    printf("设置后: \"%s\" (长度: %zu)\n", 
           vox_string_cstr(str), vox_string_length(str));
    
    printf("\n=== 追加字符串 ===\n");
    vox_string_append(str, ", ");
    vox_string_append(str, "World");
    vox_string_append_char(str, '!');
    printf("追加后: \"%s\" (长度: %zu)\n", 
           vox_string_cstr(str), vox_string_length(str));
    
    printf("\n=== 格式化追加 ===\n");
    vox_string_append_format(str, " The number is %d, float is %.2f", 42, 3.14);
    printf("格式化追加后: \"%s\"\n", vox_string_cstr(str));
    
    printf("\n=== 插入字符串 ===\n");
    vox_string_insert(str, 5, " Beautiful");
    printf("在位置5插入后: \"%s\"\n", vox_string_cstr(str));
    
    printf("\n=== 删除字符 ===\n");
    vox_string_remove(str, 5, 10);
    printf("删除位置5的10个字符后: \"%s\"\n", vox_string_cstr(str));
    
    printf("\n=== 查找子字符串 ===\n");
    size_t pos = vox_string_find(str, "World", 0);
    if (pos != SIZE_MAX) {
        printf("找到 'World' 在位置: %zu\n", pos);
    } else {
        printf("未找到 'World'\n");
    }
    
    printf("\n=== 替换字符串 ===\n");
    int count = vox_string_replace(str, "World", "Universe");
    printf("替换 'World' 为 'Universe'，共替换 %d 次\n", count);
    printf("替换后: \"%s\"\n", vox_string_cstr(str));
    
    printf("\n=== 提取子字符串 ===\n");
    vox_string_t* substr = vox_string_substr(mpool, str, 0, 5);
    if (substr) {
        printf("提取前5个字符: \"%s\"\n", vox_string_cstr(substr));
        vox_string_destroy(substr);
    }
    
    printf("\n=== 字符串转换 ===\n");
    vox_string_t* test_str = vox_string_from_cstr(mpool, "Hello World");
    printf("原始: \"%s\"\n", vox_string_cstr(test_str));
    
    vox_string_tolower(test_str);
    printf("转小写: \"%s\"\n", vox_string_cstr(test_str));
    
    vox_string_toupper(test_str);
    printf("转大写: \"%s\"\n", vox_string_cstr(test_str));
    
    printf("\n=== 去除空白字符 ===\n");
    vox_string_t* trim_str = vox_string_from_cstr(mpool, "   Hello World   ");
    printf("原始: \"%s\" (长度: %zu)\n", 
           vox_string_cstr(trim_str), vox_string_length(trim_str));
    vox_string_trim(trim_str);
    printf("去除空白后: \"%s\" (长度: %zu)\n", 
           vox_string_cstr(trim_str), vox_string_length(trim_str));
    
    printf("\n=== 字符串比较 ===\n");
    vox_string_t* str2 = vox_string_from_cstr(mpool, "Hello");
    vox_string_t* str3 = vox_string_from_cstr(mpool, "Hello");
    vox_string_t* str4 = vox_string_from_cstr(mpool, "World");
    
    int cmp1 = vox_string_compare(str2, str3);
    int cmp2 = vox_string_compare(str2, str4);
    printf("比较 \"%s\" 和 \"%s\": %d\n", 
           vox_string_cstr(str2), vox_string_cstr(str3), cmp1);
    printf("比较 \"%s\" 和 \"%s\": %d\n", 
           vox_string_cstr(str2), vox_string_cstr(str4), cmp2);
    
    printf("\n=== 复制字符串 ===\n");
    vox_string_t* cloned = vox_string_clone(mpool, str);
    if (cloned) {
        printf("复制成功: \"%s\" (长度: %zu)\n", 
               vox_string_cstr(cloned), vox_string_length(cloned));
        vox_string_destroy(cloned);
    }
    
    printf("\n=== 测试大量追加（自动扩容） ===\n");
    vox_string_t* large_str = vox_string_create(mpool);
    for (int i = 0; i < 100; i++) {
        vox_string_append_format(large_str, "Item %d, ", i);
    }
    printf("追加100次后，长度: %zu, 容量: %zu\n", 
           vox_string_length(large_str), vox_string_capacity(large_str));
    printf("前50个字符: \"%.50s...\"\n", vox_string_cstr(large_str));
    
    printf("\n=== 测试预留容量 ===\n");
    vox_string_t* reserve_str = vox_string_create(mpool);
    printf("预留前，容量: %zu\n", vox_string_capacity(reserve_str));
    vox_string_reserve(reserve_str, 1000);
    printf("预留1000后，容量: %zu\n", vox_string_capacity(reserve_str));
    vox_string_set(reserve_str, "Test");
    printf("设置内容后，长度: %zu, 容量: %zu\n", 
           vox_string_length(reserve_str), vox_string_capacity(reserve_str));
    
    printf("\n=== 测试调整大小 ===\n");
    vox_string_t* resize_str = vox_string_from_cstr(mpool, "Hello");
    printf("原始: \"%s\" (长度: %zu)\n", 
           vox_string_cstr(resize_str), vox_string_length(resize_str));
    vox_string_resize(resize_str, 10);
    printf("调整到10后: \"%s\" (长度: %zu)\n", 
           vox_string_cstr(resize_str), vox_string_length(resize_str));
    vox_string_resize(resize_str, 3);
    printf("调整到3后: \"%s\" (长度: %zu)\n", 
           vox_string_cstr(resize_str), vox_string_length(resize_str));
    
    printf("\n=== 清理资源 ===\n");
    vox_string_destroy(str);
    vox_string_destroy(str1);
    vox_string_destroy(test_str);
    vox_string_destroy(trim_str);
    vox_string_destroy(str2);
    vox_string_destroy(str3);
    vox_string_destroy(str4);
    vox_string_destroy(large_str);
    vox_string_destroy(reserve_str);
    vox_string_destroy(resize_str);
    
    /* 销毁内存池 */
    vox_mpool_destroy(mpool);
    
    printf("\n所有测试完成！\n");
    return 0;
}
