/* ============================================================
 * test_http_ws.c - http websocket 测试（握手 + 帧解析）
 * ============================================================ */

#include "test_runner.h"

#include "../vox_vector.h"
#include "../vox_string.h"

#include "../http/vox_http_ws.h"
#include "../http/vox_http_context.h"
#include "../http/vox_http_internal.h" /* internal create/feed */

#include <string.h>
#include <stdint.h>

static vox_strview_t find_res_header(const vox_http_response_t* res, const char* name) {
    if (!res || !name) return (vox_strview_t)VOX_STRVIEW_NULL;
    vox_vector_t* headers = (vox_vector_t*)res->headers;
    if (!headers) return (vox_strview_t)VOX_STRVIEW_NULL;
    size_t nlen = strlen(name);
    size_t cnt = vox_vector_size(headers);
    for (size_t i = 0; i < cnt; i++) {
        const vox_http_header_t* kv = (const vox_http_header_t*)vox_vector_get(headers, i);
        if (!kv || !kv->name.ptr) continue;
        if (vox_http_strieq(kv->name.ptr, kv->name.len, name, nlen)) return kv->value;
    }
    return (vox_strview_t)VOX_STRVIEW_NULL;
}

static vox_http_header_t* make_header(vox_mpool_t* mpool, const char* k, const char* v) {
    vox_http_header_t* h = (vox_http_header_t*)vox_mpool_alloc(mpool, sizeof(vox_http_header_t));
    if (!h) return NULL;
    h->name = vox_strview_from_cstr(k);
    h->value = vox_strview_from_cstr(v);
    return h;
}

static void test_ws_handshake_accept(vox_mpool_t* mpool) {
    /* RFC6455 示例 key */
    const char* key = "dGhlIHNhbXBsZSBub25jZQ==";
    const char* expected_accept = "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=";

    vox_http_context_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.mpool = mpool;
    ctx.conn = NULL; /* 单测：不需要真正切换到 ws 模式 */

    /* 构造 request headers */
    vox_vector_t* req_headers = vox_vector_create(mpool);
    TEST_ASSERT_NOT_NULL(req_headers, "创建 req headers 失败");
    TEST_ASSERT_EQ(vox_vector_push(req_headers, make_header(mpool, "Connection", "Upgrade")), 0, "push header 失败");
    TEST_ASSERT_EQ(vox_vector_push(req_headers, make_header(mpool, "Upgrade", "websocket")), 0, "push header 失败");
    TEST_ASSERT_EQ(vox_vector_push(req_headers, make_header(mpool, "Sec-WebSocket-Version", "13")), 0, "push header 失败");
    TEST_ASSERT_EQ(vox_vector_push(req_headers, make_header(mpool, "Sec-WebSocket-Key", key)), 0, "push header 失败");

    ctx.req.is_upgrade = true;
    ctx.req.headers = req_headers;
    ctx.req.http_major = 1;
    ctx.req.http_minor = 1;

    vox_http_ws_callbacks_t cbs;
    memset(&cbs, 0, sizeof(cbs));
    TEST_ASSERT_EQ(vox_http_ws_upgrade(&ctx, &cbs), 0, "ws upgrade 失败");
    TEST_ASSERT_EQ(ctx.res.status, 101, "status 应为 101");

    vox_strview_t acc = find_res_header(&ctx.res, "Sec-WebSocket-Accept");
    TEST_ASSERT_NOT_NULL(acc.ptr, "缺少 Sec-WebSocket-Accept");
    TEST_ASSERT_EQ(acc.len, strlen(expected_accept), "accept 长度不正确");
    TEST_ASSERT_EQ(memcmp(acc.ptr, expected_accept, acc.len), 0, "accept 值不正确");
}

/* ===== 帧解析 ===== */
static char g_msg[256];
static size_t g_msg_len = 0;
static bool g_msg_is_text = false;
static int g_close_code = 0;

static void on_msg(vox_http_ws_conn_t* ws, const void* data, size_t len, bool is_text, void* user_data) {
    (void)ws;
    (void)user_data;
    g_msg_len = (len < sizeof(g_msg)) ? len : sizeof(g_msg);
    if (g_msg_len > 0 && data) memcpy(g_msg, data, g_msg_len);
    g_msg_is_text = is_text;
}

static void on_close(vox_http_ws_conn_t* ws, int code, const char* reason, void* user_data) {
    (void)ws;
    (void)reason;
    (void)user_data;
    g_close_code = code;
}

static size_t build_masked_frame(uint8_t opcode, const void* payload, size_t plen, uint8_t* out, size_t out_cap) {
    uint8_t key[4] = {1, 2, 3, 4};
    size_t need = 2 + 4 + plen;
    if (plen > 125) return 0; /* 单测只覆盖小 payload */
    if (out_cap < need) return 0;
    out[0] = 0x80u | (opcode & 0x0Fu);
    out[1] = 0x80u | (uint8_t)plen;
    memcpy(out + 2, key, 4);
    const uint8_t* p = (const uint8_t*)payload;
    for (size_t i = 0; i < plen; i++) out[6 + i] = p[i] ^ key[i & 3u];
    return need;
}

static void test_ws_frame_text_binary_ping_close(vox_mpool_t* mpool) {
    vox_http_ws_callbacks_t cbs;
    memset(&cbs, 0, sizeof(cbs));
    cbs.on_message = on_msg;
    cbs.on_close = on_close;

    vox_http_ws_conn_t* ws = vox_http_ws_internal_create(mpool, NULL, &cbs);
    TEST_ASSERT_NOT_NULL(ws, "创建 ws 失败");

    /* text */
    {
        g_msg_len = 0;
        uint8_t buf[64];
        const char* msg = "hi";
        size_t n = build_masked_frame(0x1u, msg, 2, buf, sizeof(buf));
        TEST_ASSERT_GT(n, 0, "构造 text 帧失败");
        TEST_ASSERT_EQ(vox_http_ws_internal_feed(ws, buf, n), 0, "解析 text 帧失败");
        TEST_ASSERT_EQ(g_msg_is_text, true, "text 标记不正确");
        TEST_ASSERT_EQ(g_msg_len, 2, "text 长度不正确");
        TEST_ASSERT_EQ(memcmp(g_msg, "hi", 2), 0, "text 内容不正确");
    }

    /* binary */
    {
        g_msg_len = 0;
        uint8_t buf[64];
        uint8_t payload[3] = {0x01, 0x02, 0x03};
        size_t n = build_masked_frame(0x2u, payload, sizeof(payload), buf, sizeof(buf));
        TEST_ASSERT_GT(n, 0, "构造 binary 帧失败");
        TEST_ASSERT_EQ(vox_http_ws_internal_feed(ws, buf, n), 0, "解析 binary 帧失败");
        TEST_ASSERT_EQ(g_msg_is_text, false, "binary 标记不正确");
        TEST_ASSERT_EQ(g_msg_len, 3, "binary 长度不正确");
        TEST_ASSERT_EQ((uint8_t)g_msg[0], 0x01, "binary 内容不正确");
        TEST_ASSERT_EQ((uint8_t)g_msg[1], 0x02, "binary 内容不正确");
        TEST_ASSERT_EQ((uint8_t)g_msg[2], 0x03, "binary 内容不正确");
    }

    /* ping（只验证不报错） */
    {
        uint8_t buf[64];
        const char* payload = "x";
        size_t n = build_masked_frame(0x9u, payload, 1, buf, sizeof(buf));
        TEST_ASSERT_GT(n, 0, "构造 ping 帧失败");
        TEST_ASSERT_EQ(vox_http_ws_internal_feed(ws, buf, n), 0, "解析 ping 帧失败");
    }

    /* close */
    {
        g_close_code = 0;
        uint8_t close_payload[2] = {0x03, 0xE8}; /* 1000 */
        uint8_t buf[64];
        size_t n = build_masked_frame(0x8u, close_payload, 2, buf, sizeof(buf));
        TEST_ASSERT_GT(n, 0, "构造 close 帧失败");
        /* close 会返回非 0 并触发 on_close */
        (void)vox_http_ws_internal_feed(ws, buf, n);
        TEST_ASSERT_EQ(g_close_code, 1000, "close code 不正确");
    }
}

test_case_t test_http_ws_cases[] = {
    {"handshake_accept", test_ws_handshake_accept},
    {"frame_text_binary_ping_close", test_ws_frame_text_binary_ping_close},
};

test_suite_t test_http_ws_suite = {
    "http_ws",
    test_http_ws_cases,
    sizeof(test_http_ws_cases) / sizeof(test_http_ws_cases[0])
};

