# VoxLib WebSocket 模块

WebSocket 模块提供**独立的 WebSocket 服务端与客户端**，支持 WS 和 WSS，基于 `vox_loop` 异步 I/O，协议核心（帧编解码、解析器）可复用。

## 特性

- **RFC 6455**：完整 WebSocket 协议（分片、控制帧、掩码、关闭握手）
- **WS / WSS**：明文与 TLS（需 `vox_ssl` / OpenSSL）
- **异步 I/O**：基于 `vox_tcp` / `vox_tls` 与 `vox_loop`
- **帧层**：解析器 + 构建器（`vox_websocket.h`），服务端/客户端共用；自动处理分片、Ping-Pong
- **消息级 API**：send_text / send_binary / send_ping / close
- **UTF-8 校验**：文本消息可选校验
- **独立于 HTTP**：本模块为独立 WS 服务；若需在 HTTP 服务内做 Upgrade，使用 `http/vox_http_ws.h`

## 模块结构

```
websocket/
├── vox_websocket.h/c           # 协议核心：帧解析器、帧构建、掩码、UTF-8 校验
├── vox_websocket_server.h/c   # 服务端：listen、连接回调、发送/关闭
├── vox_websocket_client.h/c   # 客户端：connect、发送/关闭
└── README.md
```

## 协议核心（vox_websocket.h）

- **操作码**：`VOX_WS_OP_TEXT`、`VOX_WS_OP_BINARY`、`VOX_WS_OP_CLOSE`、`VOX_WS_OP_PING`、`VOX_WS_OP_PONG`、`VOX_WS_OP_CONTINUATION`
- **关闭码**：`VOX_WS_CLOSE_NORMAL`、`VOX_WS_CLOSE_GOING_AWAY` 等（见 `vox_ws_close_code_t`）
- **帧结构**：`vox_ws_frame_t`（fin、opcode、masked、payload_len、mask_key、payload）
- **解析器**：`vox_ws_parser_create(mpool)`、`vox_ws_parser_feed`、`vox_ws_parser_parse_frame`、`vox_ws_parser_reset`/`destroy`
- **构建**：`vox_ws_build_frame(mpool, opcode, payload, len, masked, out_frame, out_len)`、`vox_ws_build_close_frame`
- **工具**：`vox_ws_mask_payload`、`vox_ws_generate_mask_key`、`vox_ws_validate_utf8`

## 服务端（vox_websocket_server）

### 创建与监听

```c
vox_ws_server_config_t config = {0};
config.loop = loop;
config.on_connection = on_connection;
config.on_message = on_message;
config.on_close = on_close;
config.on_error = on_error;
config.user_data = my_data;
config.max_message_size = 0;  /* 0 表示不限制 */

vox_ws_server_t* server = vox_ws_server_create(&config);

vox_socket_addr_t addr;
vox_socket_parse_address("0.0.0.0", 8080, &addr);  /* 或 vox_socket_addr_from_host 等 */
vox_ws_server_listen(server, &addr, 128);            /* WS */
/* 或 vox_ws_server_listen_ssl(server, &addr, 128, ssl_ctx);  WSS */
vox_loop_run(loop, VOX_RUN_DEFAULT);
vox_ws_server_close(server);
vox_ws_server_destroy(server);
```

### 连接回调与发送

- **vox_ws_on_connection_cb**：新连接建立
- **vox_ws_on_message_cb**：收到消息（data/len/type：VOX_WS_MSG_TEXT / VOX_WS_MSG_BINARY）
- **vox_ws_on_close_cb**：连接关闭（code、reason）
- **vox_ws_on_error_cb**：错误

在连接上发送/关闭：

- **vox_ws_connection_send_text(conn, text, len)** / **vox_ws_connection_send_binary(conn, data, len)**
- **vox_ws_connection_send_ping(conn, data, len)**
- **vox_ws_connection_close(conn, code, reason)**
- **vox_ws_connection_get_user_data** / **vox_ws_connection_set_user_data**
- **vox_ws_connection_getpeername(conn, addr)**：对端地址

## 客户端（vox_websocket_client）

### 创建与连接

```c
vox_ws_client_config_t config = {0};
config.loop = loop;
config.url = "ws://localhost:8080";  /* 或 wss://... */
config.on_connect = on_connect;
config.on_message = on_message;
config.on_close = on_close;
config.on_error = on_error;
config.user_data = my_data;
config.max_message_size = 0;
/* WSS 时可选：config.ssl_ctx = ssl_ctx; */

vox_ws_client_t* client = vox_ws_client_create(&config);
vox_ws_client_connect(client);
vox_loop_run(loop, VOX_RUN_DEFAULT);
vox_ws_client_destroy(client);
```

### 状态与发送

- **vox_ws_client_get_state(client)**：`VOX_WS_CLIENT_CONNECTING` / `HANDSHAKING` / `OPEN` / `CLOSING` / `CLOSED`
- **vox_ws_client_send_text(client, text, len)** / **vox_ws_client_send_binary(client, data, len)**
- **vox_ws_client_send_ping(client, data, len)**
- **vox_ws_client_close(client, code, reason)**
- **vox_ws_client_get_user_data** / **vox_ws_client_set_user_data**

## 示例程序

在项目根目录构建后，可运行（具体目标名以 CMake 为准）：

| 示例 | 说明 |
|------|------|
| `websocket_echo_server` | 独立 WS/WSS Echo 服务端（支持 `--ssl`、`--port`） |
| `websocket_echo_client` | 独立 WS/WSS Echo 客户端（参数为 URL，如 `ws://localhost:8080`） |
| `ws_echo_example` | 基于 **HTTP 模块** 的 WebSocket（`http/vox_http_ws.h`，在 HTTP 路由内 Upgrade） |

## 与 HTTP 模块的 WebSocket 区别

| 方式 | 位置 | 适用场景 |
|------|------|----------|
| **独立 WebSocket**（本模块） | `websocket/` | 纯 WebSocket 服务，直接 TCP/TLS 监听 |
| **HTTP WebSocket** | `http/vox_http_ws.h` | 与 HTTP 服务共存，通过 HTTP Upgrade 到 WS/WSS，和路由/中间件集成 |

若要在同一端口同时提供 HTTP 与 WebSocket，应使用 HTTP 模块的 `vox_http_ws_upgrade`；若只做 WebSocket，可用本模块的 `vox_ws_server_*`。

## WSS / SSL

- **服务端**：`vox_ssl_context_t* ssl_ctx` 创建并加载证书/私钥后，调用 `vox_ws_server_listen_ssl(server, &addr, backlog, ssl_ctx)`
- **客户端**：config 中设置 `config.use_ssl = true`（或由 URL `wss://` 解析），可选 `config.ssl_ctx`（自定义客户端 SSL 上下文）

证书生成可参考项目根目录 `cert/` 或主文档 TLS 说明。

## 协程适配

配合 `coroutine/vox_coroutine_ws.h` 可在协程内用 `*_await` 连接 WebSocket（基于 HTTP 客户端升级）：`vox_coroutine_ws_connect_await`、`vox_coroutine_ws_recv_await`、`vox_coroutine_ws_send_text_await` 等。详见 `coroutine/README.md` 与示例 `coroutine_clients_example`。

## 注意事项

1. **掩码**：客户端发往服务端的帧必须掩码；本模块客户端发送时已自动掩码。
2. **生命周期**：回调中 `data/len` 仅在回调内有效；需在回调外使用请拷贝。
3. **压缩**：`enable_compression` 在服务端配置中保留但当前未实现（permessage-deflate）。
4. **最大消息**：`max_message_size` 为 0 表示不限制；可按需设置以防滥用。
5. **内存**：服务端/客户端内部使用独立内存池，不占用 `vox_loop` 的 mpool。

## 依赖

- **核心**：`vox_loop`、`vox_tcp`、`vox_mpool`、`vox_string`、`vox_socket` 等（见主库 CMake）
- **WSS**：`vox_tls`、`vox_ssl`（VOX_USE_OPENSSL）
