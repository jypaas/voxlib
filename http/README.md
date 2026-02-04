# VoxLib HTTP 模块

HTTP 模块提供 **HTTP/HTTPS 服务端**、**异步 HTTP 客户端**、**WebSocket（WS/WSS）**、**路由与中间件**、以及 **HTTP/Multipart 解析** 等能力，与 `vox_loop` 集成，支持 TLS（OpenSSL）。

## 特性

- **服务端**：基于 `vox_tcp` / `vox_tls`，支持 HTTP 与 HTTPS（WSS 通过 Upgrade）
- **路由**：支持静态路径与 `:param` 参数，可与 group 前缀组合
- **中间件链**：Gin 风格 handler 链，`next()` / `abort()` 控制流程；内置 logger、CORS、错误处理、Basic/Bearer 认证、请求体限制、限流
- **延迟响应**：`defer` + `finish`，便于在异步回调（如 DB/Redis）完成后再发送响应
- **HTTP 解析器**：请求/响应解析，支持流式、头部/body 回调
- **HTTP 客户端**：异步 GET/POST 等，支持 http/https、DNS、超时
- **WebSocket**：服务端 Upgrade、消息级 API（帧/分片/Ping-Pong/Close 由库处理）
- **Gzip**：压缩/解压（需 `VOX_USE_ZLIB`）
- **Multipart**：`multipart/form-data` 解析

## 模块结构

```
http/
├── vox_http_server.h/c      # 服务端入口：listen_tcp / listen_tls
├── vox_http_engine.h/c      # Engine：路由树、全局中间件、group
├── vox_http_router.h/c      # 路由：add、match、:param
├── vox_http_context.h/c    # Context：request/response、next/abort、defer/finish、辅助 API
├── vox_http_middleware.h/c # 中间件定义与内置中间件
├── vox_http_parser.h/c     # HTTP 消息解析器
├── vox_http_client.h/c     # 异步 HTTP/HTTPS 客户端
├── vox_http_ws.h/c         # WebSocket（服务端 Upgrade、send/close）
├── vox_http_gzip.h/c       # Gzip 压缩/解压（VOX_USE_ZLIB）
├── vox_http_multipart_parser.h/c # Multipart 解析
└── vox_http_internal.h     # 内部声明（不对外）
```

## 快速开始：HTTP 服务端

1. 创建 `vox_loop` 与 `vox_http_engine_t`
2. 注册全局中间件（可选）：`vox_http_engine_use(engine, handler)`
3. 注册路由：`vox_http_engine_get/post(engine, path, handlers, count)` 或 `vox_http_engine_add_route`
4. 可选：`vox_http_engine_group(engine, prefix)` 创建带前缀的路由组，组内可再挂中间件与路由
5. 创建 `vox_http_server_t`，`vox_http_server_listen_tcp(server, addr, backlog)` 或 `vox_http_server_listen_tls(server, ssl_ctx, addr, backlog)`
6. 运行 `vox_loop_run(loop, VOX_RUN_DEFAULT)`

```c
vox_loop_t* loop = vox_loop_create();
vox_http_engine_t* engine = vox_http_engine_create(loop);
vox_http_engine_use(engine, vox_http_middleware_logger);

vox_http_handler_cb hs[] = { hello_handler };
vox_http_engine_get(engine, "/hello", hs, 1);

vox_http_group_t* api = vox_http_engine_group(engine, "/api");
vox_http_handler_cb user_handlers[] = { user_handler };
vox_http_group_get(api, "/user/:id", user_handlers, 1);

vox_http_server_t* server = vox_http_server_create(engine);
vox_http_server_listen_tcp(server, &addr, 128);
vox_loop_run(loop, VOX_RUN_DEFAULT);
```

## Engine 与路由

- **vox_http_engine_create(loop)** / **vox_http_engine_destroy(engine)**
- **vox_http_engine_use(engine, handler)**：添加全局中间件（按注册顺序执行）
- **vox_http_engine_group(engine, prefix)**：创建路由组，路径为 `prefix + path`
- **vox_http_group_use(group, handler)**：组内中间件
- **vox_http_engine_add_route(engine, method, path, handlers, count)**：添加路由；path 支持静态段、`:param`、单段 `*` 通配
- **vox_http_engine_get/post(engine, path, handlers, count)**：便捷方法
- **vox_http_group_add_route** / **vox_http_group_get/post**：组内路由

路由匹配时，path 须为纯路径（不含 query）；匹配结果包含 handlers 与解析出的 `:param` 键值。

**详细说明**：路径规则、路由组、中间件链顺序及完整示例见 [README_ROUTING.md](README_ROUTING.md)。

## Context（请求/响应）

- **vox_http_context_request(ctx)**：只读请求（method、path、query、headers、body、is_upgrade）
- **vox_http_context_response(ctx)**：可写响应（status、headers、body）
- **vox_http_context_next(ctx)**：执行下一个 handler
- **vox_http_context_abort(ctx)**：终止后续 handler
- **vox_http_context_is_aborted(ctx)**：是否已终止

**路径参数与请求头/查询**：

- **vox_http_context_param(ctx, name)**：路径参数（如 `:id`）
- **vox_http_context_get_header(ctx, name)**：请求头（大小写不敏感）
- **vox_http_context_get_query(ctx, name)**：查询字符串参数

**写响应**：

- **vox_http_context_status(ctx, status)**：状态码
- **vox_http_context_header(ctx, name, value)**：响应头
- **vox_http_context_write(ctx, data, len)** / **vox_http_context_write_cstr(ctx, cstr)**：写 body
- **vox_http_context_build_response(ctx, out)**：将 status/headers/body 构建为 HTTP/1.x 报文到 `out`（一般由 server 内部调用）

**延迟响应（defer）**：在 handler 中先 **vox_http_context_defer(ctx)**，则 handler 返回后不会立即发送响应；在异步回调（如 DB/Redis，需在 loop 线程）里设置好 status/headers/body 后调用 **vox_http_context_finish(ctx)** 再发送。

- **vox_http_context_defer(ctx)** / **vox_http_context_is_deferred(ctx)** / **vox_http_context_finish(ctx)**

**其它**：**vox_http_context_get_loop(ctx)** / **vox_http_context_get_mpool(ctx)**；**vox_http_context_set_user_data** / **vox_http_context_get_user_data** 便于绑定业务对象。

## 中间件（vox_http_middleware）

- **vox_http_middleware_logger**：Combined Log Format 风格访问日志
- **vox_http_middleware_cors**：CORS 默认配置（Origin *、常用方法/头）
- **vox_http_middleware_error_handler**：错误状态码（>=400）且 body 为空时写默认错误页
- **vox_http_middleware_basic_auth_create(mpool, config)**：Basic 认证（username/password/realm）
- **vox_http_middleware_bearer_auth_create(mpool, config)**：Bearer Token 认证（validator + user_data）
- **vox_http_middleware_body_limit_create(mpool, max_size)**：请求体大小限制
- **vox_http_middleware_rate_limit_create(mpool, config)**：限流（max_requests、window_ms、message）

中间件内可调用 `vox_http_context_next(ctx)` 进入下一层；若不再执行后续 handler 可调用 `vox_http_context_abort(ctx)` 并自行写响应。

## HTTP 解析器（vox_http_parser）

用于流式解析 HTTP 请求或响应：

- **vox_http_parser_create(mpool, config, callbacks)**：type 可为 BOTH/REQUEST/RESPONSE；可限制 max_header_size、max_headers、max_url_size
- **vox_http_parser_execute(parser, data, len)**：喂入数据，返回已消费字节数（可能小于 len，便于处理 pipeline）
- **vox_http_parser_reset(parser)**：解析下一条消息
- **vox_http_parser_is_complete(parser)** / **vox_http_parser_has_error(parser)** / **vox_http_parser_get_error(parser)**
- **vox_http_parser_get_method** / **get_http_major/minor** / **get_status_code** / **get_content_length** / **is_chunked** / **is_connection_close** / **is_connection_keep_alive** / **is_upgrade**

回调：on_message_begin、on_url、on_status、on_header_field、on_header_value、on_headers_complete、on_body、on_message_complete、on_error。

## HTTP 客户端（vox_http_client）

异步 HTTP/HTTPS 客户端，支持 http/https、DNS、连接超时：

- **vox_http_client_create(loop)** / **vox_http_client_destroy(client)**
- **vox_http_client_request(client, request, callbacks, user_data, out_req)**：发起请求；request 含 method、url、headers、body、ssl_ctx、connection_timeout_ms
- **vox_http_client_cancel(req)** / **vox_http_client_close(req)**：取消或关闭底层连接

回调：on_connect、on_status、on_header、on_headers_complete、on_body、on_complete、on_error。当前实现面向 HTTP/1.1 单次请求（默认 Connection: close）。HTTPS 依赖 `vox_tls`（OpenSSL）。

## WebSocket（vox_http_ws）

服务端在 HTTP handler 中完成 Upgrade 后，使用消息级 API：

- **vox_http_ws_upgrade(ctx, callbacks)**：在 handler 内调用，完成握手并切换为 WS；callbacks 含 on_connect、on_message、on_close、on_error、user_data
- **vox_http_ws_send_text(ws, text, len)** / **vox_http_ws_send_binary(ws, data, len)**：发送文本/二进制
- **vox_http_ws_close(ws, code, reason)**：发送关闭帧并关闭连接

库内处理帧、分片、Ping-Pong、Close；服务端发送不需要 mask。

## Gzip（vox_http_gzip）

需定义 `VOX_USE_ZLIB` 并链接 zlib：

- **vox_http_gzip_compress(mpool, input, input_len, output)**：压缩为 gzip
- **vox_http_gzip_decompress(mpool, input, input_len, output)**：解压
- **vox_http_supports_gzip(headers)**：请求头是否接受 gzip
- **vox_http_is_gzip_encoded(...)**：响应头是否为 gzip

## Multipart 解析器（vox_http_multipart_parser）

用于解析 `multipart/form-data` / `multipart/mixed`：

- 创建解析器并设置回调（part_begin、header_field/value、name、filename、headers_complete、body、part_complete 等）
- **vox_http_multipart_parser_execute(parser, boundary, data, len)**：流式喂入数据

详见 `vox_http_multipart_parser.h`。

## 服务端 API 小结

- **vox_http_server_create(engine)** / **vox_http_server_destroy(server)**
- **vox_http_server_listen_tcp(server, addr, backlog)**：HTTP
- **vox_http_server_listen_tls(server, ssl_ctx, addr, backlog)**：HTTPS（WSS 通过同一端口 Upgrade）
- **vox_http_server_close(server)**：停止并关闭所有连接

## 示例程序

在项目根目录构建后，可运行（具体目标名以 CMake 为准）：

| 示例 | 说明 |
|------|------|
| `http_server_example` | 基本服务端、GET /hello、GET /api/user/:id、group + 中间件 |
| `http_middleware_example` | logger、CORS、error_handler、Basic/Bearer 认证、body_limit、rate_limit |
| `http_parser_example` | HTTP 解析器用法 |
| `http_client_example` | 异步 HTTP 客户端 |
| `https_server_example` | HTTPS（需 VOX_USE_OPENSSL） |
| `ws_echo_example` | WebSocket Echo 服务端 |
| `http_server_db_async_example` | HTTP + 异步 DB（含 defer/finish） |
| `http_server_db_sync_example` | HTTP + 同步 DB |

## 协程适配

配合 `coroutine/vox_coroutine_http.h`、`vox_coroutine_ws.h` 可在协程内使用 `*_await` 发起 HTTP 请求或连接 WebSocket，详见 `coroutine/README.md`。

## 注意事项

1. **线程**：Engine/Server/Context 运行在 `vox_loop` 所在线程；defer 场景的 `finish` 须在 loop 线程调用（如配合 VOX_DB_CALLBACK_LOOP）。
2. **请求/响应生命周期**：request/response 及 path/query/header 的 strview 在对应连接/请求生命周期内有效；若在 defer 回调外使用需自行拷贝。
3. **单连接顺序**：同一连接上请求按顺序处理；WebSocket 连接占用后不再处理 HTTP 请求。
4. **HTTPS/WSS**：需启用 OpenSSL（VOX_USE_OPENSSL），并配置 `vox_ssl_context_t`（证书、私钥等）；WSS 与 HTTPS 共用同一 listen 端口，通过 Upgrade 区分。
5. **JSON/文件响应**：无内置 send_json/send_file；可用 `vox_http_context_status` + `vox_http_context_header("Content-Type", ...)` + `vox_http_context_write` 自行封装。

## 依赖

- **核心**：`vox_loop`、`vox_tcp`、`vox_mpool`、`vox_string`、`vox_vector`、`vox_socket` 等（见主库 CMake）
- **HTTPS/WSS**：`vox_tls`、`vox_ssl`（VOX_USE_OPENSSL）
- **Gzip**：zlib（VOX_USE_ZLIB，可选）
