/* ============================================================
 * test_http_router.c - http router 测试
 * ============================================================ */

#include "test_runner.h"

#include "../http/vox_http_router.h"
#include "../http/vox_http_context.h"

static void h1(vox_http_context_t* ctx) { (void)ctx; }
static void h2(vox_http_context_t* ctx) { (void)ctx; }

static void test_router_static_and_param(vox_mpool_t* mpool) {
    vox_http_router_t* r = vox_http_router_create(mpool);
    TEST_ASSERT_NOT_NULL(r, "创建 router 失败");

    vox_http_handler_cb hs1[] = { h1 };
    vox_http_handler_cb hs2[] = { h2 };
    TEST_ASSERT_EQ(vox_http_router_add(r, VOX_HTTP_METHOD_GET, "/hello", hs1, 1), 0, "添加静态路由失败");
    TEST_ASSERT_EQ(vox_http_router_add(r, VOX_HTTP_METHOD_GET, "/user/:id", hs2, 1), 0, "添加 param 路由失败");

    /* 静态匹配 */
    {
        vox_http_route_match_t m;
        TEST_ASSERT_EQ(vox_http_router_match(r, VOX_HTTP_METHOD_GET, "/hello", strlen("/hello"), mpool, &m), 0, "匹配静态路由失败");
        TEST_ASSERT_EQ(m.handler_count, 1, "handler_count 不正确");
        TEST_ASSERT_EQ((uintptr_t)m.handlers[0], (uintptr_t)h1, "handlers 不正确");
        TEST_ASSERT_EQ(m.param_count, 0, "静态路由不应产生 params");
    }

    /* param 匹配 */
    {
        const char* path = "/user/123";
        vox_http_route_match_t m;
        TEST_ASSERT_EQ(vox_http_router_match(r, VOX_HTTP_METHOD_GET, path, strlen(path), mpool, &m), 0, "匹配 param 路由失败");
        TEST_ASSERT_EQ(m.handler_count, 1, "handler_count 不正确");
        TEST_ASSERT_EQ((uintptr_t)m.handlers[0], (uintptr_t)h2, "handlers 不正确");
        TEST_ASSERT_EQ(m.param_count, 1, "param_count 不正确");
        TEST_ASSERT_STR_EQ(m.params[0].name.ptr, "id", "param 名称不正确");
        TEST_ASSERT_EQ(m.params[0].value.len, 3, "param 值长度不正确");
        TEST_ASSERT_EQ(memcmp(m.params[0].value.ptr, "123", 3), 0, "param 值不正确");
    }

    /* trailing slash */
    {
        const char* path = "/user/abc/";
        vox_http_route_match_t m;
        TEST_ASSERT_EQ(vox_http_router_match(r, VOX_HTTP_METHOD_GET, path, strlen(path), mpool, &m), 0, "匹配 trailing slash 失败");
        TEST_ASSERT_EQ(m.param_count, 1, "param_count 不正确");
        TEST_ASSERT_EQ(m.params[0].value.len, 3, "param 值长度不正确");
        TEST_ASSERT_EQ(memcmp(m.params[0].value.ptr, "abc", 3), 0, "param 值不正确");
    }
}

test_case_t test_http_router_cases[] = {
    {"static_and_param", test_router_static_and_param},
};

test_suite_t test_http_router_suite = {
    "http_router",
    test_http_router_cases,
    sizeof(test_http_router_cases) / sizeof(test_http_router_cases[0])
};

