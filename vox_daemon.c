/*
 * vox_daemon.c - 守护进程控制实现（pid 文件、stop/status/restart/reload）
 */

#include "vox_daemon.h"
#include "vox_process.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#ifdef VOX_OS_UNIX
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#endif

#define VOX_DAEMON_PID_PATH_MAX 384
#define VOX_DAEMON_STOP_WAIT_ITER 30
#define VOX_DAEMON_STOP_SLEEP_US 200000
#define VOX_DAEMON_RELOAD_POLL_ITER 300
#define VOX_DAEMON_RELOAD_POLL_US 100000

#ifndef SIGTERM
#define SIGTERM 15
#endif

static int read_pid_file(const char* path) {
	FILE* f = fopen(path, "r");
	if (!f) return -1;
	int pid = -1;
	if (fscanf(f, "%d", &pid) != 1) pid = -1;
	fclose(f);
	return pid;
}

int vox_daemon_pid_path(char* buf, size_t size, const char* exe_dir, const char* pid_file_name) {
	if (!buf || size == 0) return -1;
	buf[0] = '\0';
	const char* name = (pid_file_name && pid_file_name[0]) ? pid_file_name : "vox.pid";
#ifdef VOX_OS_WINDOWS
	if (name[0] && name[1] == ':') {
		(void)exe_dir;
		snprintf(buf, size, "%s", name);
		return 0;
	}
#else
	if (name[0] == '/') {
		(void)exe_dir;
		snprintf(buf, size, "%s", name);
		return 0;
	}
#endif
	if (!exe_dir || !exe_dir[0]) {
		snprintf(buf, size, "%s", name);
		return 0;
	}
	snprintf(buf, size, "%s/%s", exe_dir, name);
	return 0;
}

int vox_daemon_read_pid(const char* path) {
	if (!path) return -1;
	return read_pid_file(path);
}

static char g_written_pid_path[VOX_DAEMON_PID_PATH_MAX];

static void atexit_unlink_pid(void) {
	if (g_written_pid_path[0]) {
		int pid = read_pid_file(g_written_pid_path);
		if (pid > 0 && (vox_process_id_t)pid == vox_process_get_current_id())
			(void)remove(g_written_pid_path);
	}
}

int vox_daemon_write_pid_file(const char* path) {
	if (!path) return -1;
	if (strlen(path) >= sizeof(g_written_pid_path)) return -1;
	FILE* f = fopen(path, "w");
	if (!f) return -1;
#ifdef VOX_OS_UNIX
	fprintf(f, "%d\n", (int)getpid());
#else
	fprintf(f, "%lu\n", (unsigned long)vox_process_get_current_id());
#endif
	fclose(f);
	memcpy(g_written_pid_path, path, strlen(path) + 1);
	atexit(atexit_unlink_pid);
	return 0;
}

#ifdef VOX_OS_UNIX
static int process_exists(int pid) {
	return kill(pid, 0) == 0;
}
#else
static int process_exists(int pid) {
	(void)pid;
	/* Windows: 可改用 OpenProcess(pid) 判断，此处简化 */
	return 1;
}
#endif

int vox_daemon_cmd_stop(const char* path) {
	if (!path) return -1;
	int pid = read_pid_file(path);
	if (pid <= 0) return 0;
	if (!process_exists(pid)) {
		(void)remove(path);
		return 0;
	}
	if (vox_process_signal_send((vox_process_id_t)pid, SIGTERM) != 0)
		return -1;
	for (int i = 0; i < VOX_DAEMON_STOP_WAIT_ITER; i++) {
#ifdef VOX_OS_UNIX
		usleep(VOX_DAEMON_STOP_SLEEP_US);
#else
		Sleep(VOX_DAEMON_STOP_SLEEP_US / 1000);
#endif
		if (!process_exists(pid)) break;
	}
	if (process_exists(pid))
		return -1;
	(void)remove(path);
	return 0;
}

int vox_daemon_cmd_status(const char* path, int* out_pid) {
	if (!path) return -1;
	int pid = read_pid_file(path);
	if (pid <= 0) {
		if (out_pid) *out_pid = -1;
		return 0;
	}
	if (!process_exists(pid)) {
		if (out_pid) *out_pid = -1;
		return 0;
	}
	if (out_pid) *out_pid = pid;
	return 1;
}

int vox_daemon_cmd_restart(const char* path, vox_daemon_run_fn run_server, void* user_data) {
	if (!path || !run_server) return -1;
	(void)vox_daemon_cmd_stop(path);
	return run_server(user_data);
}

#ifdef VOX_OS_UNIX
static int wait_for_new_master(const char* path, int old_pid, int timeout_sec) {
	for (int i = 0; i < timeout_sec * 10; i++) {
		usleep(VOX_DAEMON_RELOAD_POLL_US);
		int pid = read_pid_file(path);
		if (pid > 0 && pid != old_pid && process_exists(pid))
			return 0;
	}
	return -1;
}
#endif

int vox_daemon_cmd_reload(const char* path, vox_daemon_run_fn run_server, void* user_data) {
	if (!path || !run_server) return -1;
#ifdef VOX_OS_UNIX
	int old_pid = read_pid_file(path);
	if (old_pid <= 0 || !process_exists(old_pid))
		return vox_daemon_cmd_stop(path) == 0 ? 0 : -1;

	pid_t pid = fork();
	if (pid < 0) return -1;
	if (pid == 0) {
		int code = run_server(user_data);
		_exit(code >= 0 ? code : 255);
	}
	if (wait_for_new_master(path, old_pid, 30) != 0) {
		vox_process_signal_send((vox_process_id_t)pid, SIGTERM);
		waitpid(pid, NULL, 0);
		return -1;
	}
	vox_process_signal_send((vox_process_id_t)old_pid, SIGTERM);
	for (int i = 0; i < VOX_DAEMON_STOP_WAIT_ITER; i++) {
		usleep(VOX_DAEMON_STOP_SLEEP_US);
		if (!process_exists(old_pid)) break;
	}
	return 0;
#else
	(void)run_server;
	(void)user_data;
	(void)vox_daemon_cmd_stop(path);
	return 0;
#endif
}
