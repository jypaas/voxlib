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

/* 自动重连选项 */
static int g_auto_reconnect = 0;
static uint32_t g_reconnect_delay_ms = 1000;
static uint32_t g_max_reconnect_attempts = 0;  /* 0 = 无限重试 */

/* Will Message 选项 */
static const char* g_will_topic = NULL;
static const char* g_will_message = NULL;
static uint8_t g_will_qos = 0;

/* QoS 选项 */
static uint8_t g_pub_qos = 1;  /* 默认 QoS 1 */
static uint8_t g_sub_qos = 1;  /* 默认 QoS 1 */

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
        /* 如果启用了自动重连，不要停止循环，让重连机制处理 */
        if (!g_auto_reconnect && g_loop) {
            vox_loop_stop(g_loop);
        }
        return;
    }
    printf("[mqtt client] connected\n");

    /* 先执行订阅 */
    for (int i = 0; i < g_sub_count; i++) {
        const char* topic = g_sub_topics[i];
        size_t len = strlen(topic);
        if (vox_mqtt_client_subscribe(client, topic, len, g_sub_qos, NULL, NULL) != 0) {
            fprintf(stderr, "[mqtt client] subscribe failed: %s\n", topic);
        } else {
            printf("[mqtt client] subscribed: %s (QoS %u)\n", topic, g_sub_qos);
        }
    }
    if (g_sub_count > 0) g_sub_done = 1;

    /* 再执行发布 */
    for (int i = 0; i < g_pub_count; i++) {
        const char* topic = g_pub_topic_msg[i * 2];
        const char* msg  = g_pub_topic_msg[i * 2 + 1];
        if (!topic || !msg) continue;
        if (vox_mqtt_client_publish(client, topic, strlen(topic), msg, strlen(msg), g_pub_qos, false) != 0) {
            fprintf(stderr, "[mqtt client] publish failed: %s\n", topic);
        } else {
            printf("[mqtt client] published: %s -> %s (QoS %u)\n", topic, msg, g_pub_qos);
        }
    }
    if (g_pub_count > 0) g_pub_done = 1;

    /* 若仅发布且未订阅，发布完后断开并退出
     * 注意：QoS 1/2 需要等待握手完成，所以只有 QoS 0 才立即断开 */
    if (g_pub_done && !g_do_sub && g_pub_qos == 0) {
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
    printf("[msg] topic=%.*s, qos=%u, retain=%d, payload=%.*s\n",
        (int)topic_len, topic, qos, retain ? 1 : 0,
        (int)payload_len, (const char*)payload);
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
        "Options:\n"
        "  -5, --mqtt5           Use MQTT 5 (default: 3.1.1)\n"
        "  -i, --id <id>         Client ID\n"
        "  -s, --sub <topic>     Subscribe topic (repeatable)\n"
        "  -P, --pub <topic> <msg> Publish message (repeatable)\n"
        "  -k, --keepalive <sec> Keepalive seconds (default 60)\n"
        "  -q, --qos <0|1|2>     QoS for publish/subscribe (default 1)\n"
        "  -r, --reconnect       Enable auto reconnect\n"
        "  -R, --reconnect-delay <ms> Reconnect delay ms (default 1000)\n"
        "  -M, --max-reconnect <n> Max reconnect attempts (0=infinite, default 0)\n"
        "  -w, --will <topic> <msg> <qos> Set will message\n"
        "  -h, --help            Show this help\n"
        "Examples:\n"
        "  Subscribe:  %s -s sensor/temp localhost 1883\n"
        "  Publish:    %s -P test/topic \"hello\" localhost\n"
        "  QoS 2:      %s -q 2 -P test/qos2 \"reliable\" localhost\n"
        "  Reconnect:  %s -r -s test/# localhost\n"
        "  Will msg:   %s -w offline/client \"disconnected\" 1 -s test/# localhost\n",
        prog, prog, prog, prog, prog, prog);
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
        if (strcmp(argv[i], "-q") == 0 || strcmp(argv[i], "--qos") == 0) {
            if (i + 1 >= argc) { fprintf(stderr, "missing value for %s\n", argv[i]); return 1; }
            int q = atoi(argv[++i]);
            if (q >= 0 && q <= 2) {
                g_pub_qos = g_sub_qos = (uint8_t)q;
            }
            i++;
            continue;
        }
        if (strcmp(argv[i], "-r") == 0 || strcmp(argv[i], "--reconnect") == 0) {
            g_auto_reconnect = 1;
            i++;
            continue;
        }
        if (strcmp(argv[i], "-R") == 0 || strcmp(argv[i], "--reconnect-delay") == 0) {
            if (i + 1 >= argc) { fprintf(stderr, "missing value for %s\n", argv[i]); return 1; }
            g_reconnect_delay_ms = (uint32_t)atoi(argv[++i]);
            i++;
            continue;
        }
        if (strcmp(argv[i], "-M") == 0 || strcmp(argv[i], "--max-reconnect") == 0) {
            if (i + 1 >= argc) { fprintf(stderr, "missing value for %s\n", argv[i]); return 1; }
            g_max_reconnect_attempts = (uint32_t)atoi(argv[++i]);
            i++;
            continue;
        }
        if (strcmp(argv[i], "-w") == 0 || strcmp(argv[i], "--will") == 0) {
            if (i + 3 >= argc) { fprintf(stderr, "missing topic, message, qos for %s\n", argv[i]); return 1; }
            g_will_topic = argv[i + 1];
            g_will_message = argv[i + 2];
            g_will_qos = (uint8_t)atoi(argv[i + 3]);
            if (g_will_qos > 2) g_will_qos = 0;
            i += 4;
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

    /* Will Message 配置 */
    if (g_will_topic && g_will_message) {
        opts.will_topic = g_will_topic;
        opts.will_topic_len = strlen(g_will_topic);
        opts.will_msg = g_will_message;
        opts.will_msg_len = strlen(g_will_message);
        opts.will_qos = g_will_qos;
        opts.will_retain = false;
        printf("[mqtt client] will message: topic=%s, msg=%s, qos=%u\n",
            g_will_topic, g_will_message, g_will_qos);
    } else {
        opts.will_topic = NULL;
    }

    /* 自动重连配置 */
    opts.enable_auto_reconnect = g_auto_reconnect;
    opts.max_reconnect_attempts = g_max_reconnect_attempts;
    opts.initial_reconnect_delay_ms = g_reconnect_delay_ms;
    opts.max_reconnect_delay_ms = 60000;
    if (g_auto_reconnect) {
        printf("[mqtt client] auto reconnect enabled: delay=%ums, max_attempts=%u\n",
            g_reconnect_delay_ms, g_max_reconnect_attempts);
    }

    opts.ws_path = NULL;

    printf("[mqtt client] connecting to %s:%u (%s, QoS=%u)\n",
        host, port, use_mqtt5 ? "MQTT 5" : "MQTT 3.1.1", g_pub_qos);
    if (g_will_topic) {
        printf("[mqtt client] will message configured\n");
    }
    if (g_auto_reconnect) {
        printf("[mqtt client] auto reconnect: delay=%ums, max=%u attempts\n",
            opts.initial_reconnect_delay_ms, opts.max_reconnect_attempts);
    }

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
