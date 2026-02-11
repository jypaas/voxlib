/*
 * vox_http_engine.c - Engine 实现
 */

#include "vox_http_engine.h"
#include "vox_http_internal.h"
#include "../vox_log.h"
#include <string.h>

struct vox_http_group {
    vox_http_engine_t* engine;
    vox_string_t* prefix;
    vox_vector_t* middleware; /* element: vox_http_handler_cb* */
};

struct vox_http_engine {
    vox_loop_t* loop;
    vox_mpool_t* mpool;
    vox_http_router_t* router;
    vox_vector_t* global_middleware; /* element: vox_http_handler_cb* */
    void* user_data;
};

static int vox_http_vec_push_handler(vox_mpool_t* mpool, vox_vector_t* vec, vox_http_handler_cb cb) {
    if (!mpool || !vec || !cb) return -1;
    vox_http_handler_cb* slot = (vox_http_handler_cb*)vox_mpool_alloc(mpool, sizeof(vox_http_handler_cb));
    if (!slot) return -1;
    *slot = cb;
    if (vox_vector_push(vec, slot) != 0) {
        vox_mpool_free(mpool, slot);
        return -1;
    }
    return 0;
}

vox_http_engine_t* vox_http_engine_create(vox_loop_t* loop) {
    if (!loop) return NULL;
    vox_mpool_t* mpool = vox_loop_get_mpool(loop);
    if (!mpool) return NULL;

    vox_http_engine_t* e = (vox_http_engine_t*)vox_mpool_alloc(mpool, sizeof(vox_http_engine_t));
    if (!e) return NULL;
    memset(e, 0, sizeof(*e));
    e->loop = loop;
    e->mpool = mpool;
    e->router = vox_http_router_create(mpool);
    e->global_middleware = vox_vector_create(mpool);
    if (!e->router || !e->global_middleware) {
        /* mpool 分配的对象不做深度释放 */
        return NULL;
    }
    return e;
}

void vox_http_engine_destroy(vox_http_engine_t* engine) {
    VOX_UNUSED(engine);
    /* engine 使用 mpool 分配；此处不做深度释放 */
}

int vox_http_engine_use(vox_http_engine_t* engine, vox_http_handler_cb handler) {
    if (!engine || !handler) return -1;
    return vox_http_vec_push_handler(engine->mpool, engine->global_middleware, handler);
}

vox_http_group_t* vox_http_engine_group(vox_http_engine_t* engine, const char* prefix) {
    if (!engine || !prefix) return NULL;
    vox_http_group_t* g = (vox_http_group_t*)vox_mpool_alloc(engine->mpool, sizeof(vox_http_group_t));
    if (!g) return NULL;
    memset(g, 0, sizeof(*g));
    g->engine = engine;
    g->prefix = vox_string_from_cstr(engine->mpool, prefix);
    g->middleware = vox_vector_create(engine->mpool);
    if (!g->prefix || !g->middleware) {
        if (g->prefix) vox_string_destroy(g->prefix);
        if (g->middleware) vox_vector_destroy(g->middleware);
        vox_mpool_free(engine->mpool, g);
        return NULL;
    }
    return g;
}

int vox_http_group_use(vox_http_group_t* group, vox_http_handler_cb handler) {
    if (!group || !handler) return -1;
    return vox_http_vec_push_handler(group->engine->mpool, group->middleware, handler);
}

static char* vox_http_join_paths(vox_mpool_t* mpool, const char* prefix, const char* path) {
    if (!prefix) prefix = "";
    if (!path) path = "";
    size_t plen = strlen(prefix);
    size_t slen = strlen(path);

    /* 规范化：prefix 以 / 开头；prefix 不以 / 结尾；path 以 / 开头（除非为空） */
    const char* p = prefix;
    const char* s = path;
    if (plen == 0) {
        /* ok */
    }
    if (plen > 0 && p[0] != '/') {
        /* 为简单起见：要求传入的 prefix 以 / 开头 */
    }

    /* 去掉 prefix 末尾的 /（除非 prefix 仅 "/"） */
    while (plen > 1 && p[plen - 1] == '/') plen--;
    /* 去掉 path 开头的 /（避免双斜杠），但保留 path="/" 的情况 */
    while (slen > 1 && s[0] == '/') {
        s++;
        slen--;
    }

    /* 计算结果：prefix + "/" + path */
    size_t out_len = plen;
    if (slen > 0) {
        if (out_len == 0) {
            out_len += slen;
        } else {
            out_len += 1 + slen;
        }
    }

    char* out = (char*)vox_mpool_alloc(mpool, out_len + 1);
    if (!out) return NULL;

    size_t off = 0;
    if (plen > 0) {
        memcpy(out + off, p, plen);
        off += plen;
    }
    if (slen > 0) {
        if (off > 0) out[off++] = '/';
        memcpy(out + off, s, slen);
        off += slen;
    }
    out[off] = '\0';
    return out;
}

static int vox_http_build_chain(vox_mpool_t* mpool,
                                vox_vector_t* global_mw,
                                vox_vector_t* group_mw,
                                vox_http_handler_cb* handlers,
                                size_t handler_count,
                                vox_http_handler_cb** out_handlers,
                                size_t* out_count) {
    size_t gcnt = global_mw ? vox_vector_size(global_mw) : 0;
    size_t gcnt2 = group_mw ? vox_vector_size(group_mw) : 0;
    size_t total = gcnt + gcnt2 + handler_count;
    if (total == 0) return -1;

    vox_http_handler_cb* chain = (vox_http_handler_cb*)vox_mpool_alloc(mpool, total * sizeof(vox_http_handler_cb));
    if (!chain) return -1;

    size_t idx = 0;
    for (size_t i = 0; i < gcnt; i++) chain[idx++] = *(vox_http_handler_cb*)vox_vector_get(global_mw, i);
    for (size_t i = 0; i < gcnt2; i++) chain[idx++] = *(vox_http_handler_cb*)vox_vector_get(group_mw, i);
    for (size_t i = 0; i < handler_count; i++) chain[idx++] = handlers[i];

    *out_handlers = chain;
    *out_count = total;
    return 0;
}

int vox_http_engine_add_route(vox_http_engine_t* engine,
                              vox_http_method_t method,
                              const char* path,
                              vox_http_handler_cb* handlers,
                              size_t handler_count) {
    if (!engine || !path || !handlers || handler_count == 0) return -1;
    vox_http_handler_cb* chain = NULL;
    size_t chain_count = 0;
    if (vox_http_build_chain(engine->mpool, engine->global_middleware, NULL, handlers, handler_count, &chain, &chain_count) != 0) {
        return -1;
    }
    return vox_http_router_add(engine->router, method, path, chain, chain_count);
}

int vox_http_group_add_route(vox_http_group_t* group,
                             vox_http_method_t method,
                             const char* path,
                             vox_http_handler_cb* handlers,
                             size_t handler_count) {
    if (!group || !path || !handlers || handler_count == 0) return -1;
    vox_http_engine_t* engine = group->engine;
    if (!engine) return -1;

    char* full = vox_http_join_paths(engine->mpool, vox_string_cstr(group->prefix), path);
    if (!full) return -1;

    vox_http_handler_cb* chain = NULL;
    size_t chain_count = 0;
    if (vox_http_build_chain(engine->mpool, engine->global_middleware, group->middleware, handlers, handler_count, &chain, &chain_count) != 0) {
        return -1;
    }
    return vox_http_router_add(engine->router, method, full, chain, chain_count);
}

int vox_http_engine_get(vox_http_engine_t* engine, const char* path, vox_http_handler_cb* handlers, size_t handler_count) {
    return vox_http_engine_add_route(engine, VOX_HTTP_METHOD_GET, path, handlers, handler_count);
}
int vox_http_engine_post(vox_http_engine_t* engine, const char* path, vox_http_handler_cb* handlers, size_t handler_count) {
    return vox_http_engine_add_route(engine, VOX_HTTP_METHOD_POST, path, handlers, handler_count);
}
int vox_http_group_get(vox_http_group_t* group, const char* path, vox_http_handler_cb* handlers, size_t handler_count) {
    return vox_http_group_add_route(group, VOX_HTTP_METHOD_GET, path, handlers, handler_count);
}
int vox_http_group_post(vox_http_group_t* group, const char* path, vox_http_handler_cb* handlers, size_t handler_count) {
    return vox_http_group_add_route(group, VOX_HTTP_METHOD_POST, path, handlers, handler_count);
}

vox_http_router_t* vox_http_engine_get_router(vox_http_engine_t* engine) {
    return engine ? engine->router : NULL;
}

vox_vector_t* vox_http_engine_get_global_middleware(vox_http_engine_t* engine) {
    return engine ? engine->global_middleware : NULL;
}

vox_mpool_t* vox_http_engine_get_mpool(vox_http_engine_t* engine) {
    return engine ? engine->mpool : NULL;
}

vox_loop_t* vox_http_engine_get_loop(vox_http_engine_t* engine) {
    return engine ? engine->loop : NULL;
}

void vox_http_engine_set_user_data(vox_http_engine_t* engine, void* user_data) {
    if (engine) engine->user_data = user_data;
}

void* vox_http_engine_get_user_data(const vox_http_engine_t* engine) {
    return engine ? engine->user_data : NULL;
}

