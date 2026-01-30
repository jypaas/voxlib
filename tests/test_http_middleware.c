/* ============================================================
 * test_http_middleware.c - http middleware/chain 测试
 * ============================================================ */

#include "test_runner.h"

#include "../http/vox_http_context.h"
#include "../http/vox_http_internal.h" /* 为了构造内部 ctx 结构 */

static int g_order[16];
static int g_order_n = 0;

static void push(int x) {
    if (g_order_n < (int)(sizeof(g_order) / sizeof(g_order[0]))) {
        g_order[g_order_n++] = x;
    }
}

static void mw1(vox_http_context_t* ctx) {
    push(1);
    vox_http_context_next(ctx);
    push(2);
}

static void mw2(vox_http_context_t* ctx) {
    push(3);
    vox_http_context_next(ctx);
    push(4);
}

static void h(vox_http_context_t* ctx) {
    (void)ctx;
    push(5);
}

static void mw_abort(vox_http_context_t* ctx) {
    push(7);
    vox_http_context_abort(ctx);
}

static void test_middleware_next_order(vox_mpool_t* mpool) {
    (void)mpool;
    g_order_n = 0;

    vox_http_handler_cb hs[] = { mw1, mw2, h };

    vox_http_context_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.handlers = hs;
    ctx.handler_count = sizeof(hs) / sizeof(hs[0]);
    ctx.index = 0;
    ctx.aborted = false;

    vox_http_context_next(&ctx);

    TEST_ASSERT_EQ(g_order_n, 5, "执行顺序数量不正确");
    TEST_ASSERT_EQ(g_order[0], 1, "顺序不正确");
    TEST_ASSERT_EQ(g_order[1], 3, "顺序不正确");
    TEST_ASSERT_EQ(g_order[2], 5, "顺序不正确");
    TEST_ASSERT_EQ(g_order[3], 4, "顺序不正确");
    TEST_ASSERT_EQ(g_order[4], 2, "顺序不正确");
}

static void test_middleware_abort(vox_mpool_t* mpool) {
    (void)mpool;
    g_order_n = 0;

    vox_http_handler_cb hs[] = { mw_abort, h };

    vox_http_context_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.handlers = hs;
    ctx.handler_count = sizeof(hs) / sizeof(hs[0]);
    ctx.index = 0;
    ctx.aborted = false;

    vox_http_context_next(&ctx);

    TEST_ASSERT_EQ(g_order_n, 1, "abort 后不应继续执行后续 handler");
    TEST_ASSERT_EQ(g_order[0], 7, "abort handler 未执行");
}

test_case_t test_http_middleware_cases[] = {
    {"next_order", test_middleware_next_order},
    {"abort", test_middleware_abort},
};

test_suite_t test_http_middleware_suite = {
    "http_middleware",
    test_http_middleware_cases,
    sizeof(test_http_middleware_cases) / sizeof(test_http_middleware_cases[0])
};

