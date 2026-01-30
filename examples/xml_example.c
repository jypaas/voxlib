/*
 * xml_example.c - XML 解析器示例程序
 * 演示 vox_xml 的基本用法
 */

#include "../vox_xml.h"
#include "../vox_mpool.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* 打印字符串视图 */
static void print_strview(const char* label, const vox_strview_t* sv) {
    if (label && label[0] != '\0') {
        printf("%s: ", label);
    }
    if (sv && sv->ptr && sv->len > 0) {
        printf("\"%.*s\"", (int)sv->len, sv->ptr);
    } else {
        printf("(空)");
    }
}

/* 示例1: 简单 XML 元素 */
static void example_simple_element(void) {
    printf("=== 示例 1: 简单 XML 元素 ===\n");
    
    const char* xml_str = "<person name=\"张三\" age=\"30\" city=\"北京\"/>";
    printf("XML: %s\n", xml_str);
    
    vox_mpool_t* mpool = vox_mpool_create();
    if (!mpool) {
        fprintf(stderr, "创建内存池失败\n");
        return;
    }
    
    vox_xml_err_info_t err_info;
    vox_xml_node_t* root = vox_xml_parse_str(mpool, xml_str, &err_info);
    
    if (!root) {
        printf("解析错误: %s\n", err_info.message);
        printf("位置: 行 %d, 列 %d, 偏移 %zu\n", err_info.line, err_info.column, err_info.offset);
        vox_mpool_destroy(mpool);
        return;
    }
    
    printf("解析成功\n");
    vox_strview_t name = vox_xml_get_name(root);
    printf("  节点名: ");
    print_strview("", &name);
    printf("\n");
    
    printf("  属性数量: %zu\n", vox_xml_get_attr_count(root));
    
    /* 遍历属性 */
    vox_xml_attr_t* attr = vox_xml_first_attr(root);
    while (attr) {
        printf("    ");
        print_strview("", &attr->name);
        printf(" = ");
        print_strview("", &attr->value);
        printf("\n");
        attr = vox_xml_next_attr(attr);
    }
    
    vox_mpool_destroy(mpool);
    printf("\n");
}

/* 示例2: 带文本内容的元素 */
static void example_element_with_content(void) {
    printf("=== 示例 2: 带文本内容的元素 ===\n");
    
    const char* xml_str = "<message>Hello, World!</message>";
    printf("XML: %s\n", xml_str);
    
    vox_mpool_t* mpool = vox_mpool_create();
    if (!mpool) {
        fprintf(stderr, "创建内存池失败\n");
        return;
    }
    
    vox_xml_err_info_t err_info;
    vox_xml_node_t* root = vox_xml_parse_str(mpool, xml_str, &err_info);
    
    if (!root) {
        printf("解析错误: %s\n", err_info.message);
        printf("位置: 行 %d, 列 %d, 偏移 %zu\n", err_info.line, err_info.column, err_info.offset);
        vox_mpool_destroy(mpool);
        return;
    }
    
    printf("解析成功\n");
    vox_strview_t name = vox_xml_get_name(root);
    printf("  节点名: ");
    print_strview("", &name);
    printf("\n");
    
    vox_strview_t content = vox_xml_get_content(root);
    printf("  文本内容: ");
    print_strview("", &content);
    printf("\n");
    
    vox_mpool_destroy(mpool);
    printf("\n");
}

/* 示例3: 嵌套元素 */
static void example_nested_elements(void) {
    printf("=== 示例 3: 嵌套元素 ===\n");
    
    const char* xml_str = 
        "<book>"
        "<title>XML 解析指南</title>"
        "<author name=\"张三\" email=\"zhangsan@example.com\"/>"
        "<price>99.99</price>"
        "</book>";
    printf("XML: %s\n", xml_str);
    
    vox_mpool_t* mpool = vox_mpool_create();
    if (!mpool) {
        fprintf(stderr, "创建内存池失败\n");
        return;
    }
    
    vox_xml_err_info_t err_info;
    vox_xml_node_t* root = vox_xml_parse_str(mpool, xml_str, &err_info);
    
    if (!root) {
        printf("解析错误: %s\n", err_info.message);
        printf("位置: 行 %d, 列 %d, 偏移 %zu\n", err_info.line, err_info.column, err_info.offset);
        vox_mpool_destroy(mpool);
        return;
    }
    
    printf("解析成功\n");
    printf("  根节点: ");
    print_strview("", &root->name);
    printf("\n");
    printf("  子节点数量: %zu\n", vox_xml_get_child_count(root));
    
    /* 遍历子节点 */
    vox_xml_node_t* child = vox_xml_first_child(root);
    while (child) {
        printf("    子节点: ");
        print_strview("", &child->name);
        printf("\n");
        
        vox_strview_t content = vox_xml_get_content(child);
        if (content.len > 0) {
            printf("      内容: ");
            print_strview("", &content);
            printf("\n");
        }
        
        if (vox_xml_get_attr_count(child) > 0) {
            printf("      属性:\n");
            vox_xml_attr_t* attr = vox_xml_first_attr(child);
            while (attr) {
                printf("        ");
                print_strview("", &attr->name);
                printf(" = ");
                print_strview("", &attr->value);
                printf("\n");
                attr = vox_xml_next_attr(attr);
            }
        }
        
        child = vox_xml_next_child(child);
    }
    
    vox_mpool_destroy(mpool);
    printf("\n");
}

/* 示例4: 查找子节点和属性 */
static void example_find_operations(void) {
    printf("=== 示例 4: 查找子节点和属性 ===\n");
    
    const char* xml_str = 
        "<config>"
        "<database host=\"localhost\" port=\"3306\" user=\"admin\"/>"
        "<cache size=\"1024\" timeout=\"60\"/>"
        "</config>";
    printf("XML: %s\n", xml_str);
    
    vox_mpool_t* mpool = vox_mpool_create();
    if (!mpool) {
        fprintf(stderr, "创建内存池失败\n");
        return;
    }
    
    vox_xml_err_info_t err_info;
    vox_xml_node_t* root = vox_xml_parse_str(mpool, xml_str, &err_info);
    
    if (!root) {
        printf("解析错误: %s\n", err_info.message);
        printf("位置: 行 %d, 列 %d, 偏移 %zu\n", err_info.line, err_info.column, err_info.offset);
        vox_mpool_destroy(mpool);
        return;
    }
    
    printf("解析成功\n");
    
    /* 查找 database 节点 */
    vox_xml_node_t* db_node = vox_xml_find_child(root, "database");
    if (db_node) {
        printf("  找到 database 节点\n");
        vox_strview_t host = vox_xml_get_attr_value(db_node, "host");
        vox_strview_t port = vox_xml_get_attr_value(db_node, "port");
        vox_strview_t user = vox_xml_get_attr_value(db_node, "user");
        
        printf("    host: ");
        print_strview("", &host);
        printf("\n");
        printf("    port: ");
        print_strview("", &port);
        printf("\n");
        printf("    user: ");
        print_strview("", &user);
        printf("\n");
    }
    
    /* 查找 cache 节点 */
    vox_xml_node_t* cache_node = vox_xml_find_child(root, "cache");
    if (cache_node) {
        printf("  找到 cache 节点\n");
        vox_strview_t size = vox_xml_get_attr_value(cache_node, "size");
        vox_strview_t timeout = vox_xml_get_attr_value(cache_node, "timeout");
        
        printf("    size: ");
        print_strview("", &size);
        printf("\n");
        printf("    timeout: ");
        print_strview("", &timeout);
        printf("\n");
    }
    
    vox_mpool_destroy(mpool);
    printf("\n");
}

/* 示例5: 带注释和处理指令的 XML */
static void example_with_comments(void) {
    printf("=== 示例 5: 带注释和处理指令的 XML ===\n");
    
    const char* xml_str = 
        "<?xml version=\"1.0\"?>"
        "<!-- 这是注释 -->"
        "<root>"
        "<item id=\"1\">项目1</item>"
        "<!-- 另一个注释 -->"
        "<item id=\"2\">项目2</item>"
        "</root>";
    printf("XML: %s\n", xml_str);
    
    vox_mpool_t* mpool = vox_mpool_create();
    if (!mpool) {
        fprintf(stderr, "创建内存池失败\n");
        return;
    }
    
    vox_xml_err_info_t err_info;
    vox_xml_node_t* root = vox_xml_parse_str(mpool, xml_str, &err_info);
    
    if (!root) {
        printf("解析错误: %s\n", err_info.message);
        printf("位置: 行 %d, 列 %d, 偏移 %zu\n", err_info.line, err_info.column, err_info.offset);
        vox_mpool_destroy(mpool);
        return;
    }
    
    printf("解析成功（注释和处理指令已被忽略）\n");
    printf("  根节点: ");
    print_strview("", &root->name);
    printf("\n");
    printf("  子节点数量: %zu\n", vox_xml_get_child_count(root));
    
    /* 遍历子节点 */
    vox_xml_node_t* child = vox_xml_first_child(root);
    while (child) {
        printf("    子节点: ");
        print_strview("", &child->name);
        printf("\n");
        
        vox_strview_t id = vox_xml_get_attr_value(child, "id");
        printf("      id: ");
        print_strview("", &id);
        printf("\n");
        
        vox_strview_t content = vox_xml_get_content(child);
        printf("      内容: ");
        print_strview("", &content);
        printf("\n");
        
        child = vox_xml_next_child(child);
    }
    
    vox_mpool_destroy(mpool);
    printf("\n");
}

/* 示例6: 创建和修改 XML 节点 */
static void example_create_and_modify(void) {
    printf("=== 示例 6: 创建和修改 XML 节点 ===\n");
    
    vox_mpool_t* mpool = vox_mpool_create();
    if (!mpool) {
        fprintf(stderr, "创建内存池失败\n");
        return;
    }
    
    /* 创建根节点 */
    vox_strview_t root_name = vox_strview_from_cstr("users");
    vox_xml_node_t* root = vox_xml_node_new(mpool, &root_name);
    if (!root) {
        fprintf(stderr, "创建根节点失败\n");
        vox_mpool_destroy(mpool);
        return;
    }
    
    /* 创建第一个用户节点 */
    vox_strview_t user1_name = vox_strview_from_cstr("user");
    vox_xml_node_t* user1 = vox_xml_node_new(mpool, &user1_name);
    if (user1) {
        vox_strview_t id_name = vox_strview_from_cstr("id");
        vox_strview_t id_value = vox_strview_from_cstr("1");
        vox_xml_attr_t* id_attr = vox_xml_attr_new(mpool, &id_name, &id_value);
        if (id_attr) {
            vox_xml_add_attr(user1, id_attr);
        }
        
        vox_strview_t name_name = vox_strview_from_cstr("name");
        vox_strview_t name_value = vox_strview_from_cstr("Alice");
        vox_xml_attr_t* name_attr = vox_xml_attr_new(mpool, &name_name, &name_value);
        if (name_attr) {
            vox_xml_add_attr(user1, name_attr);
        }
        
        vox_strview_t content = vox_strview_from_cstr("Alice's profile");
        vox_xml_set_content(user1, &content);
        
        vox_xml_add_child(root, user1);
    }
    
    /* 创建第二个用户节点 */
    vox_xml_node_t* user2 = vox_xml_node_new(mpool, &user1_name);
    if (user2) {
        vox_strview_t id_name = vox_strview_from_cstr("id");
        vox_strview_t id_value = vox_strview_from_cstr("2");
        vox_xml_attr_t* id_attr = vox_xml_attr_new(mpool, &id_name, &id_value);
        if (id_attr) {
            vox_xml_add_attr(user2, id_attr);
        }
        
        vox_strview_t name_name = vox_strview_from_cstr("name");
        vox_strview_t name_value = vox_strview_from_cstr("Bob");
        vox_xml_attr_t* name_attr = vox_xml_attr_new(mpool, &name_name, &name_value);
        if (name_attr) {
            vox_xml_add_attr(user2, name_attr);
        }
        
        vox_xml_add_child(root, user2);
    }
    
    printf("创建的 XML 结构:\n");
    vox_xml_print_debug(root, 0);
    
    /* 序列化 XML */
    char buffer[1024];
    size_t size = sizeof(buffer) - 1;
    int written = vox_xml_print(root, buffer, &size, true);
    if (written > 0) {
        buffer[written] = '\0';
        printf("\n序列化的 XML:\n%s\n", buffer);
    }
    
    vox_mpool_destroy(mpool);
    printf("\n");
}

/* 示例7: 克隆节点 */
static void example_clone(void) {
    printf("=== 示例 7: 克隆节点 ===\n");
    
    const char* xml_str = 
        "<template>"
        "<header>标题</header>"
        "<body>内容</body>"
        "</template>";
    printf("原始 XML: %s\n", xml_str);
    
    vox_mpool_t* mpool = vox_mpool_create();
    if (!mpool) {
        fprintf(stderr, "创建内存池失败\n");
        return;
    }
    
    vox_xml_err_info_t err_info;
    vox_xml_node_t* original = vox_xml_parse_str(mpool, xml_str, &err_info);
    
    if (!original) {
        printf("解析错误: %s\n", err_info.message);
        vox_mpool_destroy(mpool);
        return;
    }
    
    printf("原始节点:\n");
    vox_xml_print_debug(original, 0);
    
    /* 克隆节点 */
    vox_xml_node_t* cloned = vox_xml_clone(mpool, original);
    if (cloned) {
        printf("\n克隆的节点:\n");
        vox_xml_print_debug(cloned, 0);
    }
    
    vox_mpool_destroy(mpool);
    printf("\n");
}

/* 示例8: 复杂 XML 结构 */
static void example_complex_structure(void) {
    printf("=== 示例 8: 复杂 XML 结构 ===\n");
    
    const char* xml_str = 
        "<library>"
        "<book id=\"1\" category=\"fiction\">"
        "<title>1984</title>"
        "<author>George Orwell</author>"
        "<year>1949</year>"
        "</book>"
        "<book id=\"2\" category=\"non-fiction\">"
        "<title>Clean Code</title>"
        "<author>Robert C. Martin</author>"
        "<year>2008</year>"
        "</book>"
        "</library>";
    printf("XML: %s\n", xml_str);
    
    vox_mpool_t* mpool = vox_mpool_create();
    if (!mpool) {
        fprintf(stderr, "创建内存池失败\n");
        return;
    }
    
    vox_xml_err_info_t err_info;
    vox_xml_node_t* root = vox_xml_parse_str(mpool, xml_str, &err_info);
    
    if (!root) {
        printf("解析错误: %s\n", err_info.message);
        printf("位置: 行 %d, 列 %d, 偏移 %zu\n", err_info.line, err_info.column, err_info.offset);
        vox_mpool_destroy(mpool);
        return;
    }
    
    printf("解析成功\n");
    printf("格式化输出:\n");
    vox_xml_print_debug(root, 0);
    
    /* 查找特定书籍 */
    vox_xml_node_t* book = vox_xml_first_child(root);
    int book_num = 1;
    while (book) {
        printf("\n书籍 %d:\n", book_num++);
        vox_strview_t id = vox_xml_get_attr_value(book, "id");
        vox_strview_t category = vox_xml_get_attr_value(book, "category");
        printf("  ID: ");
        print_strview("", &id);
        printf("\n");
        printf("  类别: ");
        print_strview("", &category);
        printf("\n");
        
        vox_xml_node_t* title = vox_xml_find_child(book, "title");
        if (title) {
            vox_strview_t title_content = vox_xml_get_content(title);
            printf("  标题: ");
            print_strview("", &title_content);
            printf("\n");
        }
        
        vox_xml_node_t* author = vox_xml_find_child(book, "author");
        if (author) {
            vox_strview_t author_content = vox_xml_get_content(author);
            printf("  作者: ");
            print_strview("", &author_content);
            printf("\n");
        }
        
        book = vox_xml_next_child(book);
    }
    
    vox_mpool_destroy(mpool);
    printf("\n");
}

/* 示例9: 错误处理 */
static void example_error_handling(void) {
    printf("=== 示例 9: 错误处理 ===\n");
    
    const char* invalid_xml = "<root><child></root>";  /* 标签不匹配 */
    printf("无效 XML: %s\n", invalid_xml);
    
    vox_mpool_t* mpool = vox_mpool_create();
    if (!mpool) {
        fprintf(stderr, "创建内存池失败\n");
        return;
    }
    
    vox_xml_err_info_t err_info;
    vox_xml_node_t* root = vox_xml_parse_str(mpool, invalid_xml, &err_info);
    
    if (!root) {
        printf("解析失败（预期行为）\n");
        printf("错误信息: %s\n", err_info.message);
        printf("错误位置: 行 %d, 列 %d, 偏移 %zu\n", 
               err_info.line, err_info.column, err_info.offset);
    } else {
        printf("意外：解析成功\n");
    }
    
    vox_mpool_destroy(mpool);
    printf("\n");
}

/* 示例10: XML 序列化 */
static void example_serialization(void) {
    printf("=== 示例 10: XML 序列化 ===\n");
    
    const char* xml_str = 
        "<config>"
        "<database host=\"localhost\" port=\"3306\"/>"
        "<cache size=\"1024\"/>"
        "</config>";
    printf("原始 XML: %s\n", xml_str);
    
    vox_mpool_t* mpool = vox_mpool_create();
    if (!mpool) {
        fprintf(stderr, "创建内存池失败\n");
        return;
    }
    
    vox_xml_err_info_t err_info;
    vox_xml_node_t* root = vox_xml_parse_str(mpool, xml_str, &err_info);
    
    if (!root) {
        printf("解析错误: %s\n", err_info.message);
        vox_mpool_destroy(mpool);
        return;
    }
    
    /* 序列化 XML */
    char buffer[1024];  /* 增大缓冲区 */
    size_t size = sizeof(buffer) - 1;
    int written = vox_xml_print(root, buffer, &size, true);
    
    if (written > 0) {
        buffer[written] = '\0';
        printf("序列化的 XML (带声明):\n%s\n", buffer);
    } else {
        printf("序列化失败（缓冲区不足）\n");
    }
    
    /* 不带声明的序列化 */
    size = sizeof(buffer) - 1;
    written = vox_xml_print(root, buffer, &size, false);
    if (written > 0) {
        buffer[written] = '\0';
        printf("序列化的 XML (不带声明):\n%s\n", buffer);
    }
    
    vox_mpool_destroy(mpool);
    printf("\n");
}

int main(void) {
    printf("Vox XML 解析器示例程序\n");
    printf("======================\n\n");
    
    example_simple_element();
    example_element_with_content();
    example_nested_elements();
    example_find_operations();
    example_with_comments();
    example_create_and_modify();
    example_clone();
    example_complex_structure();
    example_error_handling();
    example_serialization();
    
    printf("所有示例执行完成！\n");
    return 0;
}
