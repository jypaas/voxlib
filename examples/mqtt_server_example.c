/*
 * mqtt_server_example.c - MQTT 服务端示例
 *
 * 用法：mqtt_server_example [tcp_port] [ws_port]
 * 默认 tcp_port=1883；若提供 ws_port（如 8080）则同时监听 MQTT over WebSocket，path 为 /mqtt
 * 示例：mqtt_server_example 1883 8080  → TCP 1883 + WS 8080
 */

#include "../vox_loop.h"
#include "../vox_socket.h"
#include "../vox_log.h"
#include "../mqtt/vox_mqtt_server.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static vox_loop_t* g_loop;

static void on_connect(vox_mqtt_connection_t* conn, const char* client_id, size_t client_id_len, void* user_data) {
    (void)user_data;
    uint8_t ver = vox_mqtt_connection_get_protocol_version(conn);
    const char* proto = (ver == 5) ? "MQTT 5" : (ver == 4) ? "MQTT 3.1.1" : (ver == 3) ? "MQTT 3.1" : "MQTT";
    printf("[mqtt server] client connected: %.*s (%s)\n", (int)client_id_len, client_id, proto);
}

static void on_disconnect(vox_mqtt_connection_t* conn, void* user_data) {
    (void)conn;
    (void)user_data;
    printf("[mqtt server] client disconnected\n");
}

static void on_publish(vox_mqtt_connection_t* conn, const char* topic, size_t topic_len,
    const void* payload, size_t payload_len, uint8_t qos, void* user_data) {
    (void)conn;
    (void)user_data;
    printf("[mqtt server] publish topic=%.*s payload=%.*s qos=%u\n",
        (int)topic_len, topic, (int)payload_len, (const char*)payload, qos);
}

int main(int argc, char** argv) {
    uint16_t tcp_port = 1883;
    uint16_t ws_port = 0;
    if (argc >= 2) tcp_port = (uint16_t)atoi(argv[1]);
    if (argc >= 3) ws_port = (uint16_t)atoi(argv[2]);

    if (vox_socket_init() != 0) {
        fprintf(stderr, "vox_socket_init failed\n");
        return 1;
    }
    g_loop = vox_loop_create();
    if (!g_loop) return 1;

    vox_mqtt_server_config_t config = { 0 };
    config.loop = g_loop;
    config.mpool = NULL;
    config.on_connect = on_connect;
    config.on_disconnect = on_disconnect;
    config.on_publish = on_publish;

    vox_mqtt_server_t* server = vox_mqtt_server_create(&config);
    if (!server) {
        vox_loop_destroy(g_loop);
        return 1;
    }

    vox_socket_addr_t addr;
    if (vox_socket_parse_address("0.0.0.0", tcp_port, &addr) != 0) {
        VOX_LOG_ERROR("[mqtt server] invalid address");
        vox_mqtt_server_destroy(server);
        vox_loop_destroy(g_loop);
        return 1;
    }
    if (vox_mqtt_server_listen(server, &addr, 128) != 0) {
        VOX_LOG_ERROR("[mqtt server] TCP listen failed");
        vox_mqtt_server_destroy(server);
        vox_loop_destroy(g_loop);
        return 1;
    }
    printf("[mqtt server] TCP listening on port %u\n", tcp_port);

#if defined(VOX_USE_WEBSOCKET) && VOX_USE_WEBSOCKET
    if (ws_port > 0) {
        if (vox_socket_parse_address("0.0.0.0", ws_port, &addr) != 0) {
            VOX_LOG_ERROR("[mqtt server] invalid WS address");
            vox_mqtt_server_destroy(server);
            vox_loop_destroy(g_loop);
            return 1;
        }
        if (vox_mqtt_server_listen_ws(server, &addr, 128, "/mqtt") != 0) {
            VOX_LOG_ERROR("[mqtt server] WebSocket listen failed");
            vox_mqtt_server_destroy(server);
            vox_loop_destroy(g_loop);
            return 1;
        }
        printf("[mqtt server] WebSocket listening on port %u path /mqtt\n", ws_port);
    }
#else
    if (ws_port > 0) {
        fprintf(stderr, "[mqtt server] WebSocket not built in this binary, ignoring ws_port %u\n", ws_port);
    }
#endif

    vox_loop_run(g_loop, VOX_RUN_DEFAULT);
    vox_mqtt_server_destroy(server);
    vox_loop_destroy(g_loop);
    vox_socket_cleanup();
    return 0;
}
