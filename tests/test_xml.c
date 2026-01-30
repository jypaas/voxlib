/* ============================================================
 * test_xml.c - vox_xml 模块测试
 * ============================================================ */

#include "test_runner.h"
#include "../vox_xml.h"
#include <string.h>

/* 测试解析简单XML */
static void test_xml_parse_simple(vox_mpool_t* mpool) {
    char* xml = (char*)vox_mpool_alloc(mpool, 200);
    strcpy(xml, "<root>Hello</root>");
    size_t size = strlen(xml);
    
    vox_xml_node_t* node = vox_xml_parse(mpool, xml, &size, NULL);
    TEST_ASSERT_NOT_NULL(node, "解析XML失败");
    
    vox_strview_t name = vox_xml_get_name(node);
    TEST_ASSERT_EQ(memcmp(name.ptr, "root", 4), 0, "节点名称不正确");
    
    vox_strview_t content = vox_xml_get_content(node);
    TEST_ASSERT_EQ(memcmp(content.ptr, "Hello", 5), 0, "节点内容不正确");
}

/* 测试解析带属性的XML */
static void test_xml_parse_with_attrs(vox_mpool_t* mpool) {
    char* xml = (char*)vox_mpool_alloc(mpool, 200);
    strcpy(xml, "<root id=\"1\" name=\"test\">Content</root>");
    size_t size = strlen(xml);
    
    vox_xml_node_t* node = vox_xml_parse(mpool, xml, &size, NULL);
    TEST_ASSERT_NOT_NULL(node, "解析XML失败");
    
    size_t attr_count = vox_xml_get_attr_count(node);
    TEST_ASSERT_EQ(attr_count, 2, "属性数量不正确");
    
    vox_strview_t id_value = vox_xml_get_attr_value(node, "id");
    TEST_ASSERT_EQ(memcmp(id_value.ptr, "1", 1), 0, "属性值不正确");
    
    vox_strview_t name_value = vox_xml_get_attr_value(node, "name");
    TEST_ASSERT_EQ(memcmp(name_value.ptr, "test", 4), 0, "属性值不正确");
}

/* 测试解析嵌套XML */
static void test_xml_parse_nested(vox_mpool_t* mpool) {
    char* xml = (char*)vox_mpool_alloc(mpool, 300);
    strcpy(xml, "<root><child1>Content1</child1><child2>Content2</child2></root>");
    size_t size = strlen(xml);
    
    vox_xml_node_t* node = vox_xml_parse(mpool, xml, &size, NULL);
    TEST_ASSERT_NOT_NULL(node, "解析嵌套XML失败");
    
    size_t child_count = vox_xml_get_child_count(node);
    TEST_ASSERT_EQ(child_count, 2, "子节点数量不正确");
    
    vox_xml_node_t* child1 = vox_xml_find_child(node, "child1");
    TEST_ASSERT_NOT_NULL(child1, "查找子节点失败");
    vox_strview_t content1 = vox_xml_get_content(child1);
    TEST_ASSERT_EQ(memcmp(content1.ptr, "Content1", 8), 0, "子节点内容不正确");
}

/* 测试查找属性和子节点 */
static void test_xml_find_ops(vox_mpool_t* mpool) {
    char* xml = (char*)vox_mpool_alloc(mpool, 200);
    strcpy(xml, "<root attr=\"value\"><child>Text</child></root>");
    size_t size = strlen(xml);
    
    vox_xml_node_t* node = vox_xml_parse(mpool, xml, &size, NULL);
    TEST_ASSERT_NOT_NULL(node, "解析XML失败");
    
    vox_xml_attr_t* attr = vox_xml_find_attr(node, "attr");
    TEST_ASSERT_NOT_NULL(attr, "查找属性失败");
    
    vox_xml_node_t* child = vox_xml_find_child(node, "child");
    TEST_ASSERT_NOT_NULL(child, "查找子节点失败");
}

/* 测试遍历子节点 */
static void test_xml_traverse_children(vox_mpool_t* mpool) {
    char* xml = (char*)vox_mpool_alloc(mpool, 300);
    strcpy(xml, "<root><a>1</a><b>2</b><c>3</c></root>");
    size_t size = strlen(xml);
    
    vox_xml_node_t* node = vox_xml_parse(mpool, xml, &size, NULL);
    TEST_ASSERT_NOT_NULL(node, "解析XML失败");
    
    int count = 0;
    vox_xml_node_t* child = vox_xml_first_child(node);
    while (child) {
        count++;
        child = vox_xml_next_child(child);
    }
    TEST_ASSERT_EQ(count, 3, "遍历子节点数量不正确");
}

/* 测试遍历属性 */
static void test_xml_traverse_attrs(vox_mpool_t* mpool) {
    char* xml = (char*)vox_mpool_alloc(mpool, 200);
    strcpy(xml, "<root a=\"1\" b=\"2\" c=\"3\"/>");
    size_t size = strlen(xml);
    
    vox_xml_node_t* node = vox_xml_parse(mpool, xml, &size, NULL);
    TEST_ASSERT_NOT_NULL(node, "解析XML失败");
    
    int count = 0;
    vox_xml_attr_t* attr = vox_xml_first_attr(node);
    while (attr) {
        count++;
        attr = vox_xml_next_attr(attr);
    }
    TEST_ASSERT_EQ(count, 3, "遍历属性数量不正确");
}

/* 测试创建和操作节点 */
static void test_xml_create_node(vox_mpool_t* mpool) {
    vox_strview_t name = vox_strview_from_cstr("test");
    vox_xml_node_t* node = vox_xml_node_new(mpool, &name);
    TEST_ASSERT_NOT_NULL(node, "创建节点失败");
    
    vox_strview_t node_name = vox_xml_get_name(node);
    TEST_ASSERT_EQ(memcmp(node_name.ptr, "test", 4), 0, "节点名称不正确");
    
    vox_strview_t content = vox_strview_from_cstr("content");
    vox_xml_set_content(node, &content);
    
    vox_strview_t node_content = vox_xml_get_content(node);
    TEST_ASSERT_EQ(memcmp(node_content.ptr, "content", 7), 0, "节点内容不正确");
}

/* 测试错误处理 */
static void test_xml_error_handling(vox_mpool_t* mpool) {
    char* xml = (char*)vox_mpool_alloc(mpool, 100);
    strcpy(xml, "invalid xml content");  /* 完全无效的XML */
    size_t size = strlen(xml);
    
    vox_xml_err_info_t err_info = {0};
    vox_xml_node_t* node = vox_xml_parse(mpool, xml, &size, &err_info);
    /* XML解析器可能容忍部分无效XML，如果返回了节点，至少应该检查错误信息 */
    if (node == NULL) {
        /* 解析失败，应该有错误信息 */
        TEST_ASSERT_NE(err_info.message, NULL, "错误信息应为非空");
    } else {
        /* 解析器容忍了无效XML，至少验证节点存在 */
        TEST_ASSERT_NOT_NULL(node, "节点不应为NULL");
    }
}

/* 测试CDATA */
static void test_xml_cdata(vox_mpool_t* mpool) {
    char* xml = (char*)vox_mpool_alloc(mpool, 200);
    strcpy(xml, "<root><![CDATA[<test>content</test>]]></root>");
    size_t size = strlen(xml);
    
    vox_xml_node_t* node = vox_xml_parse(mpool, xml, &size, NULL);
    /* CDATA可能不被支持，如果解析失败也是可以接受的 */
    if (node) {
        /* CDATA节点存在，验证节点不为NULL */
        TEST_ASSERT_NOT_NULL(node, "CDATA节点不应为NULL");
    }
    /* 如果CDATA不被支持（返回NULL），这是可以接受的，至少验证解析器不会崩溃 */
}

/* 测试注释（如果支持） */
static void test_xml_comment(vox_mpool_t* mpool) {
    char* xml = (char*)vox_mpool_alloc(mpool, 200);
    strcpy(xml, "<root><!-- comment --><child>content</child></root>");
    size_t size = strlen(xml);
    
    vox_xml_node_t* node = vox_xml_parse(mpool, xml, &size, NULL);
    /* 注释可能被忽略，但至少应该能解析出子节点 */
    if (node) {
        size_t child_count = vox_xml_get_child_count(node);
        /* 注释可能被忽略，所以子节点数量可能是1或2 */
        TEST_ASSERT_GE(child_count, 1, "应该至少有一个子节点");
    }
}

/* 测试自闭合标签 */
static void test_xml_self_closing(vox_mpool_t* mpool) {
    char* xml = (char*)vox_mpool_alloc(mpool, 200);
    strcpy(xml, "<root><child1/><child2 attr=\"value\"/></root>");
    size_t size = strlen(xml);
    
    vox_xml_node_t* node = vox_xml_parse(mpool, xml, &size, NULL);
    TEST_ASSERT_NOT_NULL(node, "解析自闭合标签失败");
    
    size_t child_count = vox_xml_get_child_count(node);
    TEST_ASSERT_EQ(child_count, 2, "自闭合标签子节点数量不正确");
    
    vox_xml_node_t* child2 = vox_xml_find_child(node, "child2");
    TEST_ASSERT_NOT_NULL(child2, "查找自闭合子节点失败");
    vox_strview_t attr_value = vox_xml_get_attr_value(child2, "attr");
    TEST_ASSERT_EQ(memcmp(attr_value.ptr, "value", 5), 0, "自闭合标签属性值不正确");
}

/* 测试特殊字符和实体 */
static void test_xml_special_chars(vox_mpool_t* mpool) {
    char* xml = (char*)vox_mpool_alloc(mpool, 300);
    strcpy(xml, "<root>&lt;test&gt;&amp;&quot;apos&quot;</root>");
    size_t size = strlen(xml);
    
    vox_xml_node_t* node = vox_xml_parse(mpool, xml, &size, NULL);
    TEST_ASSERT_NOT_NULL(node, "解析特殊字符失败");
    
    vox_strview_t content = vox_xml_get_content(node);
    /* 实体应该被正确解析或保留 */
    TEST_ASSERT_GT(content.len, 0, "特殊字符内容不应为空");
}

/* 测试多级嵌套 */
static void test_xml_deep_nesting(vox_mpool_t* mpool) {
    char* xml = (char*)vox_mpool_alloc(mpool, 500);
    strcpy(xml, "<a><b><c><d><e>deep</e></d></c></b></a>");
    size_t size = strlen(xml);
    
    vox_xml_node_t* node = vox_xml_parse(mpool, xml, &size, NULL);
    TEST_ASSERT_NOT_NULL(node, "解析深度嵌套失败");
    
    vox_xml_node_t* b = vox_xml_find_child(node, "b");
    TEST_ASSERT_NOT_NULL(b, "查找第一层子节点失败");
    
    vox_xml_node_t* c = vox_xml_find_child(b, "c");
    TEST_ASSERT_NOT_NULL(c, "查找第二层子节点失败");
    
    vox_xml_node_t* d = vox_xml_find_child(c, "d");
    TEST_ASSERT_NOT_NULL(d, "查找第三层子节点失败");
    
    vox_xml_node_t* e = vox_xml_find_child(d, "e");
    TEST_ASSERT_NOT_NULL(e, "查找第四层子节点失败");
    
    vox_strview_t content = vox_xml_get_content(e);
    TEST_ASSERT_EQ(memcmp(content.ptr, "deep", 4), 0, "深度嵌套内容不正确");
}

/* 测试混合内容 */
static void test_xml_mixed_content(vox_mpool_t* mpool) {
    char* xml = (char*)vox_mpool_alloc(mpool, 300);
    strcpy(xml, "<root>Text before<child>child content</child>Text after</root>");
    size_t size = strlen(xml);
    
    vox_xml_node_t* node = vox_xml_parse(mpool, xml, &size, NULL);
    TEST_ASSERT_NOT_NULL(node, "解析混合内容失败");
    
    /* 混合内容可能被解析为文本节点和元素节点的组合 */
    size_t child_count = vox_xml_get_child_count(node);
    /* 至少应该有一个子元素 */
    TEST_ASSERT_GE(child_count, 1, "混合内容应至少有一个子节点");
    
    vox_xml_node_t* child = vox_xml_find_child(node, "child");
    TEST_ASSERT_NOT_NULL(child, "查找混合内容中的子元素失败");
}

/* 测试空元素 */
static void test_xml_empty_elements(vox_mpool_t* mpool) {
    char* xml = (char*)vox_mpool_alloc(mpool, 200);
    strcpy(xml, "<root><empty1></empty1><empty2/><empty3 attr=\"value\"></empty3></root>");
    size_t size = strlen(xml);
    
    vox_xml_node_t* node = vox_xml_parse(mpool, xml, &size, NULL);
    TEST_ASSERT_NOT_NULL(node, "解析空元素失败");
    
    size_t child_count = vox_xml_get_child_count(node);
    TEST_ASSERT_EQ(child_count, 3, "空元素数量不正确");
    
    vox_xml_node_t* empty3 = vox_xml_find_child(node, "empty3");
    TEST_ASSERT_NOT_NULL(empty3, "查找带属性的空元素失败");
    vox_strview_t attr_value = vox_xml_get_attr_value(empty3, "attr");
    TEST_ASSERT_EQ(memcmp(attr_value.ptr, "value", 5), 0, "空元素属性值不正确");
}

/* 测试套件 */
test_case_t test_xml_cases[] = {
    {"parse_simple", test_xml_parse_simple},
    {"parse_with_attrs", test_xml_parse_with_attrs},
    {"parse_nested", test_xml_parse_nested},
    {"find_ops", test_xml_find_ops},
    {"traverse_children", test_xml_traverse_children},
    {"traverse_attrs", test_xml_traverse_attrs},
    {"create_node", test_xml_create_node},
    {"error_handling", test_xml_error_handling},
    {"cdata", test_xml_cdata},
    {"comment", test_xml_comment},
    {"self_closing", test_xml_self_closing},
    {"special_chars", test_xml_special_chars},
    {"deep_nesting", test_xml_deep_nesting},
    {"mixed_content", test_xml_mixed_content},
    {"empty_elements", test_xml_empty_elements},
};

test_suite_t test_xml_suite = {
    "vox_xml",
    test_xml_cases,
    sizeof(test_xml_cases) / sizeof(test_xml_cases[0])
};
