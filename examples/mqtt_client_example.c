/*
 * mqtt_client_example.c - MQTT 客户端示例（支持 3.1.1 / 5，pub / sub）
 *
 * 用法：
 *   mqtt_client_example [选项] <host> [port]
 *
 * 选项：
 *   -5, --mqtt5           使用 MQTT 5（默认 3.1.1）
 *   -i, --id <client_id>   客户端 ID（默认 vox_mqtt_example_<pid>）
 *   -s, --sub <topic>     订阅主题，可多次指定
 *   -P, --pub <topic> <msg> 发布一条消息，可多次指定
 *   -k, --keepalive <sec>  保活秒数（默认 60）
 *
 * 示例：
 *   仅订阅： mqtt_client_example -s sensor/temp -s sensor/humid localhost 1883
 *   仅发布： mqtt_client_example -P test/topic "hello" localhost
 *   先发布再订阅： mqtt_client_example -P test/topic "hi" -s test/# localhost
 *   MQTT 5： mqtt_client_example -5 -s test/# localhost
 */

#include "../vox_loop.h"
#include "../vox_socket.h"
#include "../vox_log.h"
#include "../mqtt/vox_mqtt_client.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

#define MAX_SUB_TOPICS  64
#define MAX_PUB_PAIRS  64

#ifdef _WIN32
#include <process.h>
#define getpid() (int)_getpid()
#else
#include <unistd.h>
#endif

static vox_loop_t* g_loop = NULL;
static vox_mqtt_client_t* g_client = NULL;
static int g_do_sub = 0;
static int g_do_pub = 0;
static int g_pub_done = 0;
static int g_sub_done = 0;

/* 订阅主题列表（指针指向 argv，无 malloc） */
static const char* g_sub_topics[MAX_SUB_TOPICS];
static int g_sub_count = 0;

/* 发布列表：topic, msg 交替，共 g_pub_count 对 */
static const char* g_pub_topic_msg[MAX_PUB_PAIRS * 2];
static int g_pub_count = 0;

static void on_connect(vox_mqtt_client_t* client, int status, void* user_data) {
    (void)user_data;
    if (status != 0) {
        fprintf(stderr, "[mqtt client] connect failed, status=%d\n", status);
        if (g_loop) vox_loop_stop(g_loop);
        return;
    }
    printf("[mqtt client] connected\n");

    /* 先执行订阅 */
    for (int i = 0; i < g_sub_count; i++) {
        const char* topic = g_sub_topics[i];
        size_t len = strlen(topic);
        if (vox_mqtt_client_subscribe(client, topic, len, 1, NULL, NULL) != 0) {
            fprintf(stderr, "[mqtt client] subscribe failed: %s\n", topic);
        } else {
            printf("[mqtt client] subscribed: %s\n", topic);
        }
    }
    if (g_sub_count > 0) g_sub_done = 1;

    /* 再执行发布 */
    for (int i = 0; i < g_pub_count; i++) {
        const char* topic = g_pub_topic_msg[i * 2];
        const char* msg  = g_pub_topic_msg[i * 2 + 1];
        if (!topic || !msg) continue;
        if (vox_mqtt_client_publish(client, topic, strlen(topic), msg, strlen(msg), 1, false) != 0) {
            fprintf(stderr, "[mqtt client] publish failed: %s\n", topic);
        } else {
            printf("[mqtt client] published: %s -> %s\n", topic, msg);
        }
    }
    if (g_pub_count > 0) g_pub_done = 1;

    /* 若仅发布且未订阅，发布完后断开并退出 */
    if (g_pub_done && !g_do_sub) {
        vox_mqtt_client_disconnect(client);
        if (g_loop) vox_loop_stop(g_loop);
    }
}

static void on_message(vox_mqtt_client_t* client,
    const char* topic, size_t topic_len,
    const void* payload, size_t payload_len,
    uint8_t qos, bool retain, void* user_data) {
    (void)client;
    (void)user_data;
    (void)qos;
    (void)retain;
    printf("[msg] %.*s -> %.*s\n", (int)topic_len, topic, (int)payload_len, (const char*)payload);
}

static void on_disconnect(vox_mqtt_client_t* client, void* user_data) {
    (void)client;
    (void)user_data;
    printf("[mqtt client] disconnected\n");
}

static void on_error(vox_mqtt_client_t* client, const char* message, void* user_data) {
    (void)client;
    (void)user_data;
    fprintf(stderr, "[mqtt client] error: %s\n", message ? message : "unknown");
}

static void sigint_cb(int sig) {
    (void)sig;
    if (g_loop) vox_loop_stop(g_loop);
}

static void print_usage(const char* prog) {
    fprintf(stderr,
        "Usage: %s [options] <host> [port]\n"
        "  -5, --mqtt5           Use MQTT 5 (default: 3.1.1)\n"
        "  -i, --id <id>         Client ID\n"
        "  -s, --sub <topic>     Subscribe (repeatable)\n"
        "  -P, --pub <topic> <msg> Publish (repeatable)\n"
        "  -k, --keepalive <sec> Keepalive seconds (default 60)\n"
        "  -r, --reconnect      Auto reconnect on disconnect\n"
        "  -R, --reconnect-ms <ms> Reconnect interval ms (default 5000)\n"
        "  -h, --help            Show this help\n"
        "Examples:\n"
        "  %s -s sensor/temp localhost 1883\n"
        "  %s -r -s test/# localhost\n"
        "  %s -5 -s test/# -P test/one msg localhost\n",
        prog, prog, prog, prog);
}

int main(int argc, char** argv) {
    const char* host = NULL;
    uint16_t port = 1883;
    int use_mqtt5 = 0;
    char client_id_buf[80];
    const char* client_id = NULL;  /* NULL 表示用默认 client_id_buf */
    uint16_t keepalive = 60;

    /* 解析参数 */
    int i = 1;
    while (i < argc) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        }
        if (strcmp(argv[i], "-5") == 0 || strcmp(argv[i], "--mqtt5") == 0) {
            use_mqtt5 = 1;
            i++;
            continue;
        }
        if (strcmp(argv[i], "-i") == 0 || strcmp(argv[i], "--id") == 0) {
            if (i + 1 >= argc) { fprintf(stderr, "missing value for %s\n", argv[i]); return 1; }
            client_id = argv[++i];
            i++;
            continue;
        }
        if (strcmp(argv[i], "-s") == 0 || strcmp(argv[i], "--sub") == 0) {
            if (i + 1 >= argc) { fprintf(stderr, "missing topic for %s\n", argv[i]); return 1; }
            if (g_sub_count >= MAX_SUB_TOPICS) { fprintf(stderr, "too many -s (max %d)\n", MAX_SUB_TOPICS); return 1; }
            g_sub_topics[g_sub_count++] = argv[++i];
            g_do_sub = 1;
            i++;
            continue;
        }
        if (strcmp(argv[i], "-P") == 0 || strcmp(argv[i], "--pub") == 0) {
            if (i + 2 >= argc) { fprintf(stderr, "missing topic and message for %s\n", argv[i]); return 1; }
            if (g_pub_count >= MAX_PUB_PAIRS) { fprintf(stderr, "too many -P (max %d)\n", MAX_PUB_PAIRS); return 1; }
            g_pub_topic_msg[g_pub_count * 2]     = argv[i + 1];
            g_pub_topic_msg[g_pub_count * 2 + 1] = argv[i + 2];
            g_pub_count++;
            g_do_pub = 1;
            i += 3;
            continue;
        }
        if (strcmp(argv[i], "-k") == 0 || strcmp(argv[i], "--keepalive") == 0) {
            if (i + 1 >= argc) { fprintf(stderr, "missing value for %s\n", argv[i]); return 1; }
            int k = atoi(argv[++i]);
            keepalive = (k > 0 && k <= 65535) ? (uint16_t)k : 60;
            i++;
            continue;
        }
        if (argv[i][0] == '-') {
            fprintf(stderr, "unknown option: %s\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        }
        host = argv[i++];
        if (i < argc && argv[i][0] != '-') {
            int p = atoi(argv[i]);
            port = (p > 0 && p <= 65535) ? (uint16_t)p : 1883;
            i++;
        }
        break;
    }

    if (!host) {
        fprintf(stderr, "missing host\n");
        print_usage(argv[0]);
        return 1;
    }

    /* 若未指定 -s/-P，默认订阅 test/# 便于演示 */
    if (!g_do_sub && !g_do_pub) {
        g_do_sub = 1;
        g_sub_count = 1;
        g_sub_topics[0] = "test/#";
    }

    if (!client_id) {
        snprintf(client_id_buf, sizeof(client_id_buf), "vox_mqtt_example_%d", getpid());
        client_id = client_id_buf;
    }

    if (vox_socket_init() != 0) {
        fprintf(stderr, "vox_socket_init failed\n");
        return 1;
    }

    g_loop = vox_loop_create();
    if (!g_loop) {
        vox_socket_cleanup();
        return 1;
    }

    g_client = vox_mqtt_client_create(g_loop);
    if (!g_client) {
        vox_loop_destroy(g_loop);
        vox_socket_cleanup();
        return 1;
    }

    vox_mqtt_client_set_message_cb(g_client, on_message, NULL);
    vox_mqtt_client_set_disconnect_cb(g_client, on_disconnect, NULL);
    vox_mqtt_client_set_error_cb(g_client, on_error, NULL);

    vox_mqtt_connect_options_t opts = { 0 };
    opts.client_id       = client_id;
    opts.client_id_len   = 0;
    opts.keepalive       = keepalive;
    opts.clean_session   = true;
    opts.use_mqtt5       = (bool)use_mqtt5;
    opts.username        = NULL;
    opts.password        = NULL;
    opts.will_topic      = NULL;
    opts.ws_path         = NULL;

    printf("[mqtt client] connecting to %s:%u (%s)\n", host, port, use_mqtt5 ? "MQTT 5" : "MQTT 3.1.1");

    if (vox_mqtt_client_connect(g_client, host, port, &opts, on_connect, NULL) != 0) {
        fprintf(stderr, "[mqtt client] connect start failed\n");
        vox_mqtt_client_destroy(g_client);
        vox_loop_destroy(g_loop);
        vox_socket_cleanup();
        return 1;
    }

#ifdef _WIN32
    signal(SIGINT, sigint_cb);
#else
    signal(SIGINT, sigint_cb);
    signal(SIGPIPE, SIG_IGN);
#endif

    vox_loop_run(g_loop, VOX_RUN_DEFAULT);

    vox_mqtt_client_destroy(g_client);
    vox_loop_destroy(g_loop);
    vox_socket_cleanup();
    return 0;
}
