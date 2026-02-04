/*
 * vox_daemon.h - 守护进程控制公共接口
 * 提供 start/stop/restart/reload/status 所需的 pid 文件与信号操作。
 */

#ifndef VOX_DAEMON_H
#define VOX_DAEMON_H

#include "vox_os.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 运行服务回调：在子进程（reload）或当前进程（restart/start）中执行，不返回直到服务退出 */
typedef int (*vox_daemon_run_fn)(void* user_data);

/**
 * 生成 pid 文件路径
 * @param buf 输出缓冲区
 * @param size buf 大小
 * @param exe_dir 可执行文件所在目录（用于相对 pid_file_name），NULL 时仅支持绝对路径
 * @param pid_file_name 配置的 pid 文件名；若为绝对路径则直接使用，否则与 exe_dir 拼接；NULL 或空时用 "vox.pid"
 * @return 0 成功，-1 失败
 */
int vox_daemon_pid_path(char* buf, size_t size, const char* exe_dir, const char* pid_file_name);

/**
 * 从 pid 文件读取进程 ID
 * @param path pid 文件路径
 * @return 进程 ID，失败或无效返回 -1
 */
int vox_daemon_read_pid(const char* path);

/**
 * 将当前进程 pid 写入文件，并注册 atexit：仅当退出时 pid 文件仍为本进程时才删除（避免 reload 后旧进程误删新进程的 pid 文件）
 * @param path pid 文件路径
 * @return 0 成功，-1 失败
 */
int vox_daemon_write_pid_file(const char* path);

/**
 * 命令 stop：向 pid 文件中的进程发 SIGTERM，等待退出，删除 pid 文件
 * @param path pid 文件路径
 * @return 0 成功（已停止或本就不在跑），-1 失败（如 kill 失败或未在规定时间内退出）
 */
int vox_daemon_cmd_stop(const char* path);

/**
 * 命令 status：检查 pid 文件与进程是否存在
 * @param path pid 文件路径
 * @param out_pid 若非 NULL，在 running 时写入 pid
 * @return 1 正在运行，0 未运行，-1 错误
 */
int vox_daemon_cmd_status(const char* path, int* out_pid);

/**
 * 命令 restart：先 stop，再在当前进程中执行 run_server(user_data)，返回 run_server 的返回值
 * @param path pid 文件路径
 * @param run_server 服务入口（阻塞直到服务退出）
 * @param user_data 透传
 * @return run_server 的返回值，或 -1（如 stop 失败且未执行 run_server）
 */
int vox_daemon_cmd_restart(const char* path, vox_daemon_run_fn run_server, void* user_data);

/**
 * 命令 reload：不中断服务地重载（仅 Unix：先起新进程再停旧；Windows 退化为 stop 后由调用方再 start）
 * - Unix：fork，子进程执行 run_server 并常驻；父进程等待新 master 写 pid 文件后向旧 master 发 SIGTERM，然后返回 0（调用方应 exit(0)）
 * - Windows：仅执行 stop，返回 0，由调用方随后执行 start
 * @param path pid 文件路径
 * @param run_server 新服务入口（Unix 下在子进程内执行，不返回直到服务退出）
 * @param user_data 透传
 * @return 0 成功（Unix 父进程或 Windows 调用方应随后 exit/start），-1 失败
 */
int vox_daemon_cmd_reload(const char* path, vox_daemon_run_fn run_server, void* user_data);

#ifdef __cplusplus
}
#endif

#endif /* VOX_DAEMON_H */
