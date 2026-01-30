/*
 * vox_http_router.c - 路由实现（Radix Tree）
 * 先搭骨架，后续在 router-radix-tree todo 中补齐完整匹配逻辑。
 */

#include "vox_http_router.h"
#include "vox_http_internal.h"
#include "../vox_vector.h"
#include <string.h>

typedef struct vox_http_rnode {
    bool is_param;
    /* 静态段：segment 指向 mpool 拷贝；参数段：param_name 指向 mpool 拷贝 */
    char* segment;
    size_t segment_len;
    char* param_name;
    size_t param_name_len;

    /* element: vox_http_rnode_t* */
    vox_vector_t* static_children;
    struct vox_http_rnode* param_child;

    vox_http_handler_cb* handlers;
    size_t handler_count;
} vox_http_rnode_t;

struct vox_http_router {
    vox_mpool_t* mpool;
    vox_http_rnode_t* roots[VOX_HTTP_METHOD_PATCH + 1];
};

static char* vox_http_mpool_strdup(vox_mpool_t* mpool, const char* s, size_t len) {
    if (!mpool) return NULL;
    if (!s) len = 0;
    char* out = (char*)vox_mpool_alloc(mpool, len + 1);
    if (!out) return NULL;
    if (len > 0) memcpy(out, s, len);
    out[len] = '\0';
    return out;
}

static vox_http_rnode_t* vox_http_rnode_create(vox_mpool_t* mpool) {
    vox_http_rnode_t* n = (vox_http_rnode_t*)vox_mpool_alloc(mpool, sizeof(vox_http_rnode_t));
    if (!n) return NULL;
    memset(n, 0, sizeof(*n));
    n->static_children = vox_vector_create(mpool);
    if (!n->static_children) return NULL;
    return n;
}

static vox_http_rnode_t* vox_http_rnode_find_static_child(vox_http_rnode_t* node, const char* seg, size_t seg_len) {
    if (!node || !node->static_children) return NULL;
    size_t cnt = vox_vector_size(node->static_children);
    for (size_t i = 0; i < cnt; i++) {
        vox_http_rnode_t* c = (vox_http_rnode_t*)vox_vector_get(node->static_children, i);
        if (!c || c->is_param) continue;
        if (c->segment_len == seg_len && seg_len > 0 && memcmp(c->segment, seg, seg_len) == 0) return c;
        if (c->segment_len == 0 && seg_len == 0) return c;
    }
    return NULL;
}

static vox_http_rnode_t* vox_http_rnode_add_static_child(vox_mpool_t* mpool, vox_http_rnode_t* node, const char* seg, size_t seg_len) {
    if (!mpool || !node) return NULL;
    vox_http_rnode_t* c = vox_http_rnode_create(mpool);
    if (!c) return NULL;
    c->is_param = false;
    c->segment = vox_http_mpool_strdup(mpool, seg, seg_len);
    c->segment_len = seg_len;
    if (!c->segment) return NULL;
    if (vox_vector_push(node->static_children, c) != 0) return NULL;
    return c;
}

static vox_http_rnode_t* vox_http_rnode_get_or_add_param_child(vox_mpool_t* mpool, vox_http_rnode_t* node, const char* name, size_t name_len) {
    if (!mpool || !node) return NULL;
    if (node->param_child) {
        /* 允许同一层只有一个 param；若 name 不同则视为冲突 */
        if (node->param_child->param_name_len != name_len ||
            (name_len > 0 && memcmp(node->param_child->param_name, name, name_len) != 0)) {
            return NULL;
        }
        return node->param_child;
    }
    vox_http_rnode_t* c = vox_http_rnode_create(mpool);
    if (!c) return NULL;
    c->is_param = true;
    c->param_name = vox_http_mpool_strdup(mpool, name, name_len);
    c->param_name_len = name_len;
    if (!c->param_name) return NULL;
    node->param_child = c;
    return c;
}

static void vox_http_trim_trailing_slash(const char** path, size_t* len) {
    if (!path || !*path || !len) return;
    while (*len > 1 && (*path)[*len - 1] == '/') (*len)--;
}

vox_http_router_t* vox_http_router_create(vox_mpool_t* mpool) {
    if (!mpool) return NULL;
    vox_http_router_t* r = (vox_http_router_t*)vox_mpool_alloc(mpool, sizeof(vox_http_router_t));
    if (!r) return NULL;
    memset(r, 0, sizeof(*r));
    r->mpool = mpool;
    for (int i = 0; i <= VOX_HTTP_METHOD_PATCH; i++) {
        r->roots[i] = vox_http_rnode_create(mpool);
        if (!r->roots[i]) return NULL;
    }
    return r;
}

void vox_http_router_destroy(vox_http_router_t* router) {
    /* router 内部使用 mpool 分配；这里不做深度释放 */
    VOX_UNUSED(router);
}

int vox_http_router_add(vox_http_router_t* router,
                        vox_http_method_t method,
                        const char* path,
                        vox_http_handler_cb* handlers,
                        size_t handler_count) {
    if (!router || !path || !handlers || handler_count == 0) return -1;
    if (method <= VOX_HTTP_METHOD_UNKNOWN || method > VOX_HTTP_METHOD_PATCH) return -1;
    if (path[0] != '/') return -1;

    size_t path_len = strlen(path);
    vox_http_trim_trailing_slash(&path, &path_len);

    vox_http_rnode_t* node = router->roots[method];
    if (!node) return -1;

    /* 逐段插入：/a/b/:id */
    size_t i = 1; /* skip leading '/' */
    while (i <= path_len) {
        /* 找到当前段 [seg_start, seg_end) */
        size_t seg_start = i;
        while (i < path_len && path[i] != '/') i++;
        size_t seg_len = (i > seg_start) ? (i - seg_start) : 0;

        if (seg_len == 0) {
            /* 处理 '//' 或末尾 '/' 的情况（末尾已 trim）；这里直接跳过 */
            i++;
            continue;
        }

        if (path[seg_start] == ':') {
            /* param 段 */
            const char* name = path + seg_start + 1;
            size_t name_len = seg_len - 1;
            if (name_len == 0) return -1;
            node = vox_http_rnode_get_or_add_param_child(router->mpool, node, name, name_len);
            if (!node) return -1;
        } else {
            /* static 段 */
            vox_http_rnode_t* c = vox_http_rnode_find_static_child(node, path + seg_start, seg_len);
            if (!c) c = vox_http_rnode_add_static_child(router->mpool, node, path + seg_start, seg_len);
            if (!c) return -1;
            node = c;
        }

        i++; /* skip '/' */
    }

    /* 终点写 handlers */
    node->handlers = handlers;
    node->handler_count = handler_count;
    return 0;
}

int vox_http_router_match(vox_http_router_t* router,
                          vox_http_method_t method,
                          const char* path,
                          size_t path_len,
                          vox_mpool_t* mpool,
                          vox_http_route_match_t* out) {
    if (out) memset(out, 0, sizeof(*out));
    if (!router || !path || path_len == 0 || !out || !mpool) return -1;
    if (method <= VOX_HTTP_METHOD_UNKNOWN || method > VOX_HTTP_METHOD_PATCH) return -1;
    if (path[0] != '/') return -1;

    vox_http_trim_trailing_slash(&path, &path_len);

    vox_http_rnode_t* node = router->roots[method];
    if (!node) return -1;

    /* 预估 param 数：按 ':' 的数量估算 */
    size_t param_cap = 0;
    for (size_t k = 0; k < path_len; k++) if (path[k] == ':') param_cap++; /* 仅用于 add；match 这里按 '/' 数估算更稳 */
    VOX_UNUSED(param_cap);
    size_t params_count = 0;
    size_t params_cap2 = 4;
    vox_http_param_t* params = NULL;

    size_t i = 1;
    while (i <= path_len) {
        size_t seg_start = i;
        while (i < path_len && path[i] != '/') i++;
        size_t seg_len = (i > seg_start) ? (i - seg_start) : 0;

        if (seg_len == 0) {
            i++;
            continue;
        }

        /* 先匹配 static，其次 param */
        vox_http_rnode_t* c = vox_http_rnode_find_static_child(node, path + seg_start, seg_len);
        if (c) {
            node = c;
            i++;
            continue;
        }
        if (node->param_child) {
            node = node->param_child;
            if (params_count == 0) {
                params = (vox_http_param_t*)vox_mpool_alloc(mpool, params_cap2 * sizeof(vox_http_param_t));
                if (!params) return -1;
            } else if (params_count >= params_cap2) {
                /* mpool 无 realloc：一次性扩容到新数组并拷贝 */
                size_t new_cap = params_cap2 * 2;
                vox_http_param_t* np = (vox_http_param_t*)vox_mpool_alloc(mpool, new_cap * sizeof(vox_http_param_t));
                if (!np) return -1;
                memcpy(np, params, params_count * sizeof(vox_http_param_t));
                params = np;
                params_cap2 = new_cap;
            }
            params[params_count].name = (vox_strview_t){ node->param_name, node->param_name_len };
            params[params_count].value = (vox_strview_t){ path + seg_start, seg_len };
            params_count++;
            i++;
            continue;
        }
        return -1;
    }

    if (!node->handlers || node->handler_count == 0) return -1;
    out->handlers = node->handlers;
    out->handler_count = node->handler_count;
    out->params = params;
    out->param_count = params_count;
    return 0;
}

