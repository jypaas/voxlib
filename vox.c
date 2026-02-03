/*
 * vox.c - VoxLib 统一入口
 * 提供库级 API：初始化、反初始化、版本查询、启动模式（多线程/多进程）。
 */

#include "vox.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#ifdef VOX_OS_WINDOWS
    #include <windows.h>
#else
    #include <unistd.h>
    #include <sys/wait.h>
    #include <fcntl.h>
    #include <signal.h>
#endif

#define VOX_VERSION_STR "1.0.0"
#define VOX_START_WORKER_ARG "--vox-worker"

static int g_ref_count;

/* 当前 worker 下标：进程模式下子进程内有效；线程模式用 TLS */
static uint32_t g_worker_index = VOX_START_INVALID_WORKER_INDEX;
static vox_mpool_t* g_start_mpool = NULL;
static vox_tls_key_t* g_worker_index_key = NULL;

/* 取路径最后一段（basename），用于进程名前缀；无 / 则返回原串，空则返回 "vox" */
static const char* argv0_basename(const char* argv0) {
    const char* p = argv0;
    if (argv0) {
        const char* last = argv0;
        while (*argv0) {
            if (*argv0 == '/' || *argv0 == '\\') last = argv0 + 1;
            argv0++;
        }
        p = *last ? last : p;
    }
    return (p && *p) ? p : "vox";
}

/* 获取 CPU 核心数，用于 worker_count==0 时的默认值 */
static uint32_t get_cpu_count(void) {
#ifdef VOX_OS_WINDOWS
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return (uint32_t)si.dwNumberOfProcessors;
#else
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return (n > 0) ? (uint32_t)n : 1u;
#endif
}

/* 解析 --mode=thread|process, --workers=N 或 --worker=N, --daemon, --respawn */
static void parse_one(const char* arg, vox_start_options_t* out) {
    if (strncmp(arg, "--mode=", 7) == 0) {
        const char* v = arg + 7;
        if (strcmp(v, "thread") == 0) out->mode = (vox_start_mode_t)VOX_START_MODE_THREAD;
        else if (strcmp(v, "process") == 0) out->mode = (vox_start_mode_t)VOX_START_MODE_PROCESS;
        else if (strcmp(v, "listener_workers") == 0) out->mode = (vox_start_mode_t)VOX_START_MODE_LISTENER_WORKERS;
    } else if (strncmp(arg, "--workers=", 10) == 0) {
        unsigned long n = strtoul(arg + 10, NULL, 10);
        if (n > 0 && n <= 0xFFFFFFFFu) out->worker_count = (uint32_t)n;
    } else if (strncmp(arg, "--worker=", 9) == 0) {
        unsigned long n = strtoul(arg + 9, NULL, 10);
        if (n > 0 && n <= 0xFFFFFFFFu) out->worker_count = (uint32_t)n;
    } else if (strcmp(arg, "--daemon") == 0) {
        out->daemon = true;
    } else if (strcmp(arg, "--respawn") == 0) {
        out->respawn_workers = true;
    }
}

/* 检查是否为子进程/工作进程（由库内部传入的 --vox-worker=i） */
static int parse_worker_index(int argc, char** argv, int* out_index) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], VOX_START_WORKER_ARG) == 0 && i + 1 < argc) {
            unsigned long idx = strtoul(argv[i + 1], NULL, 10);
            if (idx <= 0xFFFFFFFFu) {
                *out_index = (int)idx;
                return 1;
            }
        }
        if (strncmp(argv[i], VOX_START_WORKER_ARG "=", sizeof(VOX_START_WORKER_ARG "=") - 1) == 0) {
            unsigned long idx = strtoul(argv[i] + sizeof(VOX_START_WORKER_ARG "=") - 1, NULL, 10);
            if (idx <= 0xFFFFFFFFu) {
                *out_index = (int)idx;
                return 1;
            }
        }
    }
    return 0;
}

void vox_init(void)
{
    if (g_ref_count == 0) {
        if (vox_socket_init() != 0) {
            return; /* socket 初始化失败，不增加引用计数 */
        }
    }
    ++g_ref_count;
}

void vox_fini(void)
{
    if (g_ref_count > 0) {
        --g_ref_count;
        if (g_ref_count == 0) {
            vox_socket_cleanup();
        }
    }
}

const char* vox_version(void)
{
    return VOX_VERSION_STR;
}

static int vox_start_parse(int argc, char** argv, vox_start_options_t* out_options)
{
    if (!out_options) return -1;
    out_options->mode = (vox_start_mode_t)VOX_START_MODE_THREAD;
    out_options->worker_count = 0;
    out_options->daemon = false;
    out_options->respawn_workers = false;
    for (int i = 1; i < argc; i++)
        parse_one(argv[i], out_options);
    if (out_options->worker_count == 0)
        out_options->worker_count = get_cpu_count();
    if (out_options->worker_count == 0)
        out_options->worker_count = 1;
    return 0;
}

uint32_t vox_start_worker_index(void)
{
    if (g_worker_index_key) {
        void* v = vox_tls_get(g_worker_index_key);
        return (v != NULL) ? (uint32_t)(uintptr_t)v : VOX_START_INVALID_WORKER_INDEX;
    }
    return g_worker_index;
}

/* 线程模式：包装用，用于设置 TLS worker_index 后调用 worker_func */
typedef struct {
    uint32_t index;
    vox_worker_func_t worker_func;
    void* user_data;
} thread_worker_wrapper_t;

static int thread_worker_entry(void* user_data)
{
    thread_worker_wrapper_t* w = (thread_worker_wrapper_t*)user_data;
    if (g_worker_index_key)
        vox_tls_set(g_worker_index_key, (void*)(uintptr_t)w->index);
    return w->worker_func(w->user_data);
}

/* 多线程模式：创建 N 个线程执行 worker_func，等待全部结束 */
static int run_thread_mode(uint32_t n, vox_worker_func_t worker_func, void* user_data)
{
    vox_mpool_config_t mcfg = {0};
    vox_mpool_t* mpool = vox_mpool_create_with_config(&mcfg);
    if (!mpool) return -1;
    if (!g_worker_index_key) {
        g_start_mpool = vox_mpool_create_with_config(&(vox_mpool_config_t){0});
        if (g_start_mpool) g_worker_index_key = vox_tls_key_create(g_start_mpool, NULL);
    }
    thread_worker_wrapper_t* wrappers = (thread_worker_wrapper_t*)vox_mpool_alloc(mpool, (size_t)n * sizeof(thread_worker_wrapper_t));
    if (!wrappers) { vox_mpool_destroy(mpool); return -1; }
    vox_thread_t** threads = (vox_thread_t**)vox_mpool_alloc(mpool, (size_t)n * sizeof(vox_thread_t*));
    if (!threads) {
        vox_mpool_free(mpool, wrappers);
        vox_mpool_destroy(mpool);
        return -1;
    }
    memset(threads, 0, (size_t)n * sizeof(vox_thread_t*));
    for (uint32_t i = 0; i < n; i++) {
        wrappers[i].index = i;
        wrappers[i].worker_func = worker_func;
        wrappers[i].user_data = user_data;
        threads[i] = vox_thread_create(mpool, thread_worker_entry, &wrappers[i]);
        if (!threads[i]) {
            for (uint32_t j = 0; j < i; j++) vox_thread_join(threads[j], NULL);
            vox_mpool_free(mpool, wrappers);
            vox_mpool_free(mpool, threads);
            vox_mpool_destroy(mpool);
            return -1;
        }
    }
    int first_nonzero = 0;
    for (uint32_t i = 0; i < n; i++) {
        int code = 0;
        vox_thread_join(threads[i], &code);
        if (code != 0 && first_nonzero == 0) first_nonzero = code;
    }
    vox_mpool_free(mpool, wrappers);
    vox_mpool_free(mpool, threads);
    vox_mpool_destroy(mpool);
    return first_nonzero;
}

/* 同上，工作函数接收 worker_index */
typedef struct {
    uint32_t index;
    vox_worker_func_ex_t worker_func_ex;
    void* user_data;
} thread_worker_ex_wrapper_t;

static int thread_worker_ex_entry(void* user_data)
{
    thread_worker_ex_wrapper_t* w = (thread_worker_ex_wrapper_t*)user_data;
    if (g_worker_index_key)
        vox_tls_set(g_worker_index_key, (void*)(uintptr_t)w->index);
    return w->worker_func_ex(w->index, w->user_data);
}

static int run_thread_mode_ex(uint32_t n, vox_worker_func_ex_t worker_func_ex, void* user_data)
{
    vox_mpool_config_t mcfg = {0};
    vox_mpool_t* mpool = vox_mpool_create_with_config(&mcfg);
    if (!mpool) return -1;
    if (!g_worker_index_key) {
        g_start_mpool = vox_mpool_create_with_config(&(vox_mpool_config_t){0});
        if (g_start_mpool) g_worker_index_key = vox_tls_key_create(g_start_mpool, NULL);
    }
    thread_worker_ex_wrapper_t* wrappers = (thread_worker_ex_wrapper_t*)vox_mpool_alloc(mpool, (size_t)n * sizeof(thread_worker_ex_wrapper_t));
    if (!wrappers) { vox_mpool_destroy(mpool); return -1; }
    vox_thread_t** threads = (vox_thread_t**)vox_mpool_alloc(mpool, (size_t)n * sizeof(vox_thread_t*));
    if (!threads) {
        vox_mpool_free(mpool, wrappers);
        vox_mpool_destroy(mpool);
        return -1;
    }
    memset(threads, 0, (size_t)n * sizeof(vox_thread_t*));
    for (uint32_t i = 0; i < n; i++) {
        wrappers[i].index = i;
        wrappers[i].worker_func_ex = worker_func_ex;
        wrappers[i].user_data = user_data;
        threads[i] = vox_thread_create(mpool, thread_worker_ex_entry, &wrappers[i]);
        if (!threads[i]) {
            for (uint32_t j = 0; j < i; j++) vox_thread_join(threads[j], NULL);
            vox_mpool_free(mpool, wrappers);
            vox_mpool_free(mpool, threads);
            vox_mpool_destroy(mpool);
            return -1;
        }
    }
    int first_nonzero = 0;
    for (uint32_t i = 0; i < n; i++) {
        int code = 0;
        vox_thread_join(threads[i], &code);
        if (code != 0 && first_nonzero == 0) first_nonzero = code;
    }
    vox_mpool_free(mpool, wrappers);
    vox_mpool_free(mpool, threads);
    vox_mpool_destroy(mpool);
    return first_nonzero;
}

#ifdef VOX_OS_UNIX
/* master+respawn 模式用：收到 SIGINT/SIGTERM 退出主循环；SIGCHLD 表示有 worker 退出需 respawn */
static volatile int g_master_quit;
static volatile int g_sigchld_pending;
static void on_master_quit(int sig) { (void)sig; g_master_quit = 1; }
static void on_sigchld(int sig) { (void)sig; g_sigchld_pending = 1; }

/* 守护进程：fork 脱离终端，子进程 setsid + chdir / + 关闭标准流（仅多进程模式前调用） */
static int daemonize(void) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid > 0) _exit(0);
    if (setsid() < 0) _exit(1);
    if (chdir("/") != 0) { /* 忽略 */ }
    {
        int fd = open("/dev/null", O_RDWR);
        if (fd >= 0) {
            dup2(fd, 0);
            dup2(fd, 1);
            dup2(fd, 2);
            if (fd > 2) close(fd);
        }
    }
    return 0;
}

/* 多进程模式（Unix）：fork N 次，子进程执行 worker_func 后 _exit；respawn 为 true 时 master 常驻并 SIGCHLD respawn */
static int run_process_mode_unix(uint32_t n, const char* argv0, vox_worker_func_t worker_func, void* user_data, bool respawn,
                                 vox_on_master_ready_t on_master_ready, void* on_master_ready_data)
{
    const char* prog = argv0_basename(argv0);
    char t[24];
    snprintf(t, sizeof(t), "%s master", prog);
    vox_process_setname(t);
    vox_mpool_config_t mcfg = {0};
    vox_mpool_t* mpool = vox_mpool_create_with_config(&mcfg);
    if (!mpool) return -1;
    pid_t* pids = (pid_t*)vox_mpool_alloc(mpool, (size_t)n * sizeof(pid_t));
    if (!pids) {
        vox_mpool_destroy(mpool);
        return -1;
    }
    memset(pids, 0, (size_t)n * sizeof(pid_t));
    for (uint32_t i = 0; i < n; i++) {
        pid_t pid = fork();
        if (pid < 0) {
            while (i > 0) waitpid(pids[--i], NULL, 0);
            vox_mpool_free(mpool, pids);
            vox_mpool_destroy(mpool);
            return -1;
        }
        if (pid == 0) {
            g_worker_index = i;
            {
                char wname[24];
                snprintf(wname, sizeof(wname), "%s worker", prog);
                vox_process_setname(wname);
            }
            int code = worker_func(user_data);
            _exit(code >= 0 ? code : 255);
        }
        pids[i] = pid;
    }
    int first_nonzero = 0;
    if (respawn) {
        g_master_quit = 0;
        g_sigchld_pending = 0;
        vox_process_signal_register(SIGCHLD, on_sigchld);
        vox_process_signal_register(SIGINT, on_master_quit);
        vox_process_signal_register(SIGTERM, on_master_quit);
        if (on_master_ready)
            on_master_ready(on_master_ready_data);
        while (!g_master_quit) {
            if (g_sigchld_pending) {
                g_sigchld_pending = 0;
                for (;;) {
                    int status = 0;
                    pid_t dead = waitpid(-1, &status, WNOHANG);
                    if (dead <= 0) break;
                    for (uint32_t k = 0; k < n; k++) {
                        if (pids[k] == dead) {
                            if (!g_master_quit) {
                                pid_t pid = fork();
                                if (pid == 0) {
                                    g_worker_index = k;
                                    {
                                        char wname[24];
                                        snprintf(wname, sizeof(wname), "%s worker", prog);
                                        vox_process_setname(wname);
                                    }
                                    int code = worker_func(user_data);
                                    _exit(code >= 0 ? code : 255);
                                }
                                if (pid > 0) pids[k] = pid;
                            }
                            break;
                        }
                    }
                }
            }
            usleep(100000);
        }
        for (uint32_t i = 0; i < n; i++) {
            if (pids[i] > 0) {
                vox_process_signal_send((vox_process_id_t)pids[i], SIGTERM);
                waitpid(pids[i], NULL, 0);
            }
        }
    } else {
        for (uint32_t i = 0; i < n; i++) {
            int status = 0;
            if (waitpid(pids[i], &status, 0) >= 0) {
                if (WIFEXITED(status)) {
                    int c = WEXITSTATUS(status);
                    if (c != 0 && first_nonzero == 0) first_nonzero = c;
                }
            }
        }
    }
    vox_mpool_free(mpool, pids);
    vox_mpool_destroy(mpool);
    return first_nonzero;
}

static int run_process_mode_unix_ex(uint32_t n, const char* argv0, vox_worker_func_ex_t worker_func_ex, void* user_data, bool respawn,
                                    vox_on_master_ready_t on_master_ready, void* on_master_ready_data)
{
    const char* prog = argv0_basename(argv0);
    char t[24];
    snprintf(t, sizeof(t), "%s master", prog);
    vox_process_setname(t);
    vox_mpool_config_t mcfg = {0};
    vox_mpool_t* mpool = vox_mpool_create_with_config(&mcfg);
    if (!mpool) return -1;
    pid_t* pids = (pid_t*)vox_mpool_alloc(mpool, (size_t)n * sizeof(pid_t));
    if (!pids) {
        vox_mpool_destroy(mpool);
        return -1;
    }
    memset(pids, 0, (size_t)n * sizeof(pid_t));
    for (uint32_t i = 0; i < n; i++) {
        pid_t pid = fork();
        if (pid < 0) {
            while (i > 0) waitpid(pids[--i], NULL, 0);
            vox_mpool_free(mpool, pids);
            vox_mpool_destroy(mpool);
            return -1;
        }
        if (pid == 0) {
            g_worker_index = i;
            {
                char wname[24];
                snprintf(wname, sizeof(wname), "%s worker", prog);
                vox_process_setname(wname);
            }
            int code = worker_func_ex(i, user_data);
            _exit(code >= 0 ? code : 255);
        }
        pids[i] = pid;
    }
    int first_nonzero = 0;
    if (respawn) {
        g_master_quit = 0;
        g_sigchld_pending = 0;
        vox_process_signal_register(SIGCHLD, on_sigchld);
        vox_process_signal_register(SIGINT, on_master_quit);
        vox_process_signal_register(SIGTERM, on_master_quit);
        if (on_master_ready)
            on_master_ready(on_master_ready_data);
        while (!g_master_quit) {
            if (g_sigchld_pending) {
                g_sigchld_pending = 0;
                for (;;) {
                    int status = 0;
                    pid_t dead = waitpid(-1, &status, WNOHANG);
                    if (dead <= 0) break;
                    for (uint32_t k = 0; k < n; k++) {
                        if (pids[k] == dead) {
                            if (!g_master_quit) {
                                pid_t pid = fork();
                                if (pid == 0) {
                                    g_worker_index = k;
                                    {
                                        char wname[24];
                                        snprintf(wname, sizeof(wname), "%s worker", prog);
                                        vox_process_setname(wname);
                                    }
                                    int code = worker_func_ex(k, user_data);
                                    _exit(code >= 0 ? code : 255);
                                }
                                if (pid > 0) pids[k] = pid;
                            }
                            break;
                        }
                    }
                }
            }
            usleep(100000);
        }
        for (uint32_t i = 0; i < n; i++) {
            if (pids[i] > 0) {
                vox_process_signal_send((vox_process_id_t)pids[i], SIGTERM);
                waitpid(pids[i], NULL, 0);
            }
        }
    } else {
        for (uint32_t i = 0; i < n; i++) {
            int status = 0;
            if (waitpid(pids[i], &status, 0) >= 0) {
                if (WIFEXITED(status)) {
                    int c = WEXITSTATUS(status);
                    if (c != 0 && first_nonzero == 0) first_nonzero = c;
                }
            }
        }
    }
    vox_mpool_free(mpool, pids);
    vox_mpool_destroy(mpool);
    return first_nonzero;
}
#endif

#ifdef VOX_OS_WINDOWS
/* 多进程模式（Windows）：spawn 本进程 N 次，传入 --vox-worker=i，子进程由 vox_start_main 识别并执行 worker_func */
static int run_process_mode_win(int argc, char** argv, uint32_t n,
                                vox_worker_func_t worker_func, void* user_data)
{
    (void)worker_func;
    (void)user_data;
    char exe_path[MAX_PATH];
    if (GetModuleFileNameA(NULL, exe_path, sizeof(exe_path)) == 0) return -1;
    vox_mpool_config_t mcfg = {0};
    vox_mpool_t* mpool = vox_mpool_create_with_config(&mcfg);
    if (!mpool) return -1;
    vox_process_t** procs = (vox_process_t**)vox_mpool_alloc(mpool, (size_t)n * sizeof(vox_process_t*));
    if (!procs) { vox_mpool_destroy(mpool); return -1; }
    memset(procs, 0, (size_t)n * sizeof(vox_process_t*));
    const char* worker_arg_fmt = VOX_START_WORKER_ARG "=%u";
    size_t extra_len = 32;
    char* worker_arg = (char*)vox_mpool_alloc(mpool, extra_len);
    if (!worker_arg) { vox_mpool_free(mpool, procs); vox_mpool_destroy(mpool); return -1; }
    vox_process_options_t popts = {0};
    popts.stdin_redirect = VOX_PROCESS_REDIRECT_NONE;
    popts.stdout_redirect = VOX_PROCESS_REDIRECT_NONE;
    popts.stderr_redirect = VOX_PROCESS_REDIRECT_NONE;
    int first_nonzero = 0;
    for (uint32_t i = 0; i < n; i++) {
        snprintf(worker_arg, extra_len, worker_arg_fmt, (unsigned)i);
        size_t na = (argc > 0 ? (size_t)argc : 0) + 2;
        const char** child_argv = (const char**)vox_mpool_alloc(mpool, (na + 1) * sizeof(char*));
        if (!child_argv) break;
        size_t k = 0;
        if (argv && argc > 0) {
            for (int j = 0; j < argc && argv[j]; j++) child_argv[k++] = argv[j];
        }
        child_argv[k++] = worker_arg;
        child_argv[k] = NULL;
        procs[i] = vox_process_create(mpool, exe_path, child_argv, &popts);
        if (!procs[i]) {
            while (i > 0) { vox_process_wait(procs[i - 1], NULL, 0); vox_process_destroy(procs[--i]); }
            vox_mpool_free(mpool, worker_arg);
            vox_mpool_free(mpool, procs);
            vox_mpool_destroy(mpool);
            return -1;
        }
    }
    for (uint32_t i = 0; i < n; i++) {
        vox_process_status_t st = {0};
        vox_process_wait(procs[i], &st, 0);
        if (st.exited && st.exit_code != 0 && first_nonzero == 0) first_nonzero = st.exit_code;
        vox_process_destroy(procs[i]);
    }
    vox_mpool_free(mpool, worker_arg);
    vox_mpool_free(mpool, procs);
    vox_mpool_destroy(mpool);
    return first_nonzero;
}
#endif

static int vox_start_run(int argc, char** argv,
                         const vox_start_options_t* options,
                         vox_worker_func_t worker_func,
                         void* user_data,
                         vox_on_master_ready_t on_master_ready,
                         void* on_master_ready_data)
{
    (void)argc;
    if (!options || !worker_func) return -1;
    uint32_t n = options->worker_count;
    if (n == 0) n = get_cpu_count();
    if (n == 0) n = 1;
    vox_init();
    int ret;
    if (options->mode == (vox_start_mode_t)VOX_START_MODE_THREAD) {
        ret = run_thread_mode(n, worker_func, user_data);
    } else {
#ifdef VOX_OS_UNIX
        if (options->daemon && daemonize() != 0) {
            vox_fini();
            return -1;
        }
        ret = run_process_mode_unix(n, argv && argv[0] ? argv[0] : "vox", worker_func, user_data, options->respawn_workers,
                                    on_master_ready, on_master_ready_data);
#else
        (void)options;
        (void)on_master_ready;
        (void)on_master_ready_data;
        ret = run_process_mode_win(argc, argv, n, worker_func, user_data);
#endif
    }
    vox_fini();
    return ret;
}

static int vox_start_run_ex(int argc, char** argv,
                            const vox_start_options_t* options,
                            vox_worker_func_ex_t worker_func_ex,
                            void* user_data,
                            vox_on_master_ready_t on_master_ready,
                            void* on_master_ready_data)
{
    (void)argc;
    if (!options || !worker_func_ex) return -1;
    uint32_t n = options->worker_count;
    if (n == 0) n = get_cpu_count();
    if (n == 0) n = 1;
    vox_init();
    int ret;
    if (options->mode == (vox_start_mode_t)VOX_START_MODE_THREAD) {
        ret = run_thread_mode_ex(n, worker_func_ex, user_data);
    } else {
#ifdef VOX_OS_UNIX
        if (options->daemon && daemonize() != 0) {
            vox_fini();
            return -1;
        }
        ret = run_process_mode_unix_ex(n, argv && argv[0] ? argv[0] : "vox", worker_func_ex, user_data, options->respawn_workers,
                                      on_master_ready, on_master_ready_data);
#else
        (void)options;
        (void)on_master_ready;
        (void)on_master_ready_data;
        ret = run_process_mode_win(argc, argv, n, NULL, NULL);
#endif
    }
    vox_fini();
    return ret;
}

/* 单监听+worker 模式：监听线程入口，调用用户 listener_func */
typedef struct {
    vox_loop_t* loop;
    vox_tpool_t* worker_pool;
    vox_listener_func_t listener_func;
    vox_tpool_task_func_t worker_task;
    void* user_data;
} listener_workers_ctx_t;

static int listener_thread_entry(void* user_data)
{
    listener_workers_ctx_t* ctx = (listener_workers_ctx_t*)user_data;
    ctx->listener_func(ctx->loop, ctx->worker_pool, ctx->worker_task, ctx->user_data);
    return 0;
}

static int vox_start_run_listener_workers(const vox_start_options_t* options,
                                          vox_listener_func_t listener_func,
                                          vox_tpool_task_func_t worker_task,
                                          void* user_data)
{
    if (!options || !listener_func || !worker_task) return -1;
    uint32_t n = options->worker_count;
    if (n == 0) n = get_cpu_count();
    if (n == 0) n = 1;
    vox_init();
    vox_loop_t* loop = vox_loop_create();
    if (!loop) { vox_fini(); return -1; }
    vox_tpool_config_t tcfg = {0};
    tcfg.thread_count = (size_t)n;
    vox_tpool_t* tpool = vox_tpool_create_with_config(&tcfg);
    if (!tpool) {
        vox_loop_destroy(loop);
        vox_fini();
        return -1;
    }
    vox_mpool_config_t mcfg = {0};
    vox_mpool_t* mpool = vox_mpool_create_with_config(&mcfg);
    if (!mpool) {
        vox_tpool_destroy(tpool);
        vox_loop_destroy(loop);
        vox_fini();
        return -1;
    }
    listener_workers_ctx_t ctx = {0};
    ctx.loop = loop;
    ctx.worker_pool = tpool;
    ctx.listener_func = listener_func;
    ctx.worker_task = worker_task;
    ctx.user_data = user_data;
    vox_thread_t* listener_thread = vox_thread_create(mpool, listener_thread_entry, &ctx);
    if (!listener_thread) {
        vox_tpool_shutdown(tpool);
        vox_tpool_destroy(tpool);
        vox_loop_destroy(loop);
        vox_mpool_destroy(mpool);
        vox_fini();
        return -1;
    }
    vox_thread_join(listener_thread, NULL);
    vox_tpool_shutdown(tpool);
    vox_tpool_destroy(tpool);
    vox_loop_destroy(loop);
    vox_mpool_destroy(mpool);
    vox_fini();
    return 0;
}

int vox_start(int argc, char** argv, vox_start_params_t* params)
{
    if (!params) return -1;
    int worker_index = -1;
    if (parse_worker_index(argc, argv, &worker_index)) {
        g_worker_index = (uint32_t)worker_index;
        {
            const char* prog = argv0_basename(argv && argv[0] ? argv[0] : "vox");
            char wname[24];
            snprintf(wname, sizeof(wname), "%s worker", prog);
            vox_process_setname(wname);
        }
        if (params->worker_func_ex)
            return params->worker_func_ex((uint32_t)worker_index, params->user_data);
        if (params->worker_func)
            return params->worker_func(params->user_data);
        return -1;
    }
    if (vox_start_parse(argc, argv, &params->options) != 0) return -1;
    if (params->options.mode == (vox_start_mode_t)VOX_START_MODE_LISTENER_WORKERS) {
        if (!params->listener_func || !params->worker_task) return -1;
        return vox_start_run_listener_workers(&params->options,
                params->listener_func, params->worker_task, params->user_data);
    }
    if (params->worker_func_ex)
        return vox_start_run_ex(argc, argv, &params->options,
                params->worker_func_ex, params->user_data,
                params->on_master_ready, params->user_data);
    if (params->worker_func)
        return vox_start_run(argc, argv, &params->options,
                params->worker_func, params->user_data,
                params->on_master_ready, params->user_data);
    return -1;
}
