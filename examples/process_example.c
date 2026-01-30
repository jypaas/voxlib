/*
 * process_example.c - 进程管理示例程序
 * 演示 vox_process 的基本用法
 */

#include "../vox_process.h"
#include "../vox_mpool.h"
#include "../vox_os.h"  /* 包含平台特定头文件 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#ifndef VOX_OS_WINDOWS
    #include <unistd.h>
#endif

/* 测试基本进程创建和等待 */
void test_basic_process(void) {
    printf("\n=== 测试基本进程创建和等待 ===\n");
    
    vox_mpool_t* mpool = vox_mpool_create();
    if (!mpool) {
        fprintf(stderr, "创建内存池失败\n");
        return;
    }
    
#ifdef VOX_OS_WINDOWS
    const char* command = "cmd.exe";
    const char* argv[] = {"/c", "echo", "Hello from child process!", NULL};
#else
    const char* command = "echo";
    const char* argv[] = {"Hello from child process!", NULL};
#endif
    
    vox_process_t* proc = vox_process_create(mpool, command, argv, NULL);
    if (!proc) {
        fprintf(stderr, "创建进程失败\n");
        vox_mpool_destroy(mpool);
        return;
    }
    
    printf("进程创建成功，PID: %llu\n", (unsigned long long)vox_process_get_id(proc));
    
    /* 等待进程结束 */
    vox_process_status_t status;
    if (vox_process_wait(proc, &status, 0) == 0) {
        if (status.exited) {
            printf("进程正常退出，退出码: %d\n", status.exit_code);
        } else if (status.signaled) {
            printf("进程被信号终止，信号: %d\n", status.signal);
        }
    } else {
        printf("等待进程失败\n");
    }
    
    vox_process_destroy(proc);
    vox_mpool_destroy(mpool);
}

/* 测试进程输出捕获 */
void test_process_output(void) {
    printf("\n=== 测试进程输出捕获 ===\n");
    
    vox_mpool_t* mpool = vox_mpool_create();
    if (!mpool) {
        fprintf(stderr, "创建内存池失败\n");
        return;
    }
    
    vox_process_options_t opts;
    memset(&opts, 0, sizeof(opts));
    opts.stdout_redirect = VOX_PROCESS_REDIRECT_PIPE;
    opts.stderr_redirect = VOX_PROCESS_REDIRECT_PIPE;
    
#ifdef VOX_OS_WINDOWS
    const char* command = "cmd.exe";
    const char* argv[] = {"/c", "echo", "Standard output", "&&", "echo", "Standard error", ">&2", NULL};
#else
    const char* command = "sh";
    const char* argv[] = {"-c", "echo 'Standard output' && echo 'Standard error' >&2", NULL};
#endif
    
    vox_process_t* proc = vox_process_create(mpool, command, argv, &opts);
    if (!proc) {
        fprintf(stderr, "创建进程失败");
        #ifndef VOX_OS_WINDOWS
            if (errno != 0) {
                fprintf(stderr, ": %s (errno=%d)", strerror(errno), errno);
            }
        #endif
        fprintf(stderr, "\n");
        vox_mpool_destroy(mpool);
        return;
    }
    
    printf("读取进程输出...\n");
    
    /* 读取标准输出 */
    char buffer[1024];
    int64_t bytes_read = vox_process_read_stdout(proc, buffer, sizeof(buffer) - 1);
    if (bytes_read > 0) {
        buffer[bytes_read] = '\0';
        printf("标准输出: %s", buffer);
    }
    
    /* 等待进程结束 */
    vox_process_status_t status;
    vox_process_wait(proc, &status, 0);
    
    vox_process_destroy(proc);
    vox_mpool_destroy(mpool);
}

/* 测试进程输入 */
void test_process_input(void) {
    printf("\n=== 测试进程输入 ===\n");
    
    vox_mpool_t* mpool = vox_mpool_create();
    if (!mpool) {
        fprintf(stderr, "创建内存池失败\n");
        return;
    }
    
    vox_process_options_t opts;
    memset(&opts, 0, sizeof(opts));
    opts.stdin_redirect = VOX_PROCESS_REDIRECT_PIPE;
    opts.stdout_redirect = VOX_PROCESS_REDIRECT_PIPE;
    
#ifdef VOX_OS_WINDOWS
    const char* command = "findstr";
    const char* argv[] = {"test", NULL};
#else
    const char* command = "grep";
    const char* argv[] = {"test", NULL};
#endif
    
    vox_process_t* proc = vox_process_create(mpool, command, argv, &opts);
    if (!proc) {
        fprintf(stderr, "创建进程失败");
        #ifndef VOX_OS_WINDOWS
            if (errno != 0) {
                fprintf(stderr, ": %s (errno=%d)", strerror(errno), errno);
            }
        #endif
        fprintf(stderr, "\n");
        vox_mpool_destroy(mpool);
        return;
    }
    
    /* 向进程写入数据 */
    const char* input = "This is a test line\nAnother line\n";
    int64_t bytes_written = vox_process_write_stdin(proc, input, strlen(input));
    printf("向进程写入 %lld 字节\n", (long long)bytes_written);
    
    /* 关闭标准输入 */
    vox_process_close_stdin(proc);
    
    /* 读取输出 */
    char buffer[1024];
    int64_t bytes_read = vox_process_read_stdout(proc, buffer, sizeof(buffer) - 1);
    if (bytes_read > 0) {
        buffer[bytes_read] = '\0';
        printf("进程输出: %s", buffer);
    }
    
    /* 等待进程结束 */
    vox_process_status_t status;
    vox_process_wait(proc, &status, 0);
    
    vox_process_destroy(proc);
    vox_mpool_destroy(mpool);
}

/* 测试便捷函数 */
void test_execute_function(void) {
    printf("\n=== 测试便捷函数 ===\n");
    
    vox_mpool_t* mpool = vox_mpool_create();
    if (!mpool) {
        fprintf(stderr, "创建内存池失败\n");
        return;
    }
    
#ifdef VOX_OS_WINDOWS
    const char* command = "cmd.exe";
    const char* argv[] = {"/c", "echo", "Hello World", NULL};
#else
    const char* command = "echo";
    const char* argv[] = {"Hello World", NULL};
#endif
    
    char* output = NULL;
    size_t output_size = 0;
    int exit_code = 0;
    
    if (vox_process_execute(mpool, command, argv, &output, &output_size, &exit_code) == 0) {
        printf("执行成功，退出码: %d\n", exit_code);
        if (output && output_size > 0) {
            printf("输出内容: %.*s", (int)output_size, output);
        }
        if (output) {
            vox_mpool_free(mpool, output);
        }
    } else {
        printf("执行失败");
        #ifndef VOX_OS_WINDOWS
            if (errno != 0) {
                printf(": %s (errno=%d)", strerror(errno), errno);
            }
        #endif
        printf("\n");
    }
    
    vox_mpool_destroy(mpool);
}

/* 测试环境变量 */
void test_environment(void) {
    printf("\n=== 测试环境变量 ===\n");
    
    vox_mpool_t* mpool = vox_mpool_create();
    if (!mpool) {
        fprintf(stderr, "创建内存池失败\n");
        return;
    }
    
    /* 设置环境变量 */
    const char* test_var = "VOX_TEST_VAR";
    const char* test_value = "test_value_123";
    
    if (vox_process_setenv(test_var, test_value) == 0) {
        printf("设置环境变量成功: %s=%s\n", test_var, test_value);
    } else {
        printf("设置环境变量失败\n");
    }
    
    /* 获取环境变量 */
    char* value = vox_process_getenv(mpool, test_var);
    if (value) {
        printf("获取环境变量: %s=%s\n", test_var, value);
        vox_mpool_free(mpool, value);
    } else {
        printf("获取环境变量失败\n");
    }
    
    /* 删除环境变量 */
    if (vox_process_unsetenv(test_var) == 0) {
        printf("删除环境变量成功\n");
    }
    
    /* 验证删除 */
    value = vox_process_getenv(mpool, test_var);
    if (value) {
        printf("警告：环境变量仍然存在: %s\n", value);
        vox_mpool_free(mpool, value);
    } else {
        printf("环境变量已成功删除\n");
    }
    
    vox_mpool_destroy(mpool);
}

/* 测试进程ID */
void test_process_ids(void) {
    printf("\n=== 测试进程ID ===\n");
    
    vox_process_id_t current_pid = vox_process_get_current_id();
    vox_process_id_t parent_pid = vox_process_get_parent_id();
    
    printf("当前进程ID: %llu\n", (unsigned long long)current_pid);
    printf("父进程ID: %llu\n", (unsigned long long)parent_pid);
}

/* 测试进程状态检查 */
void test_process_status(void) {
    printf("\n=== 测试进程状态检查 ===\n");
    
    vox_mpool_t* mpool = vox_mpool_create();
    if (!mpool) {
        fprintf(stderr, "创建内存池失败\n");
        return;
    }
    
#ifdef VOX_OS_WINDOWS
    const char* command = "cmd.exe";
    const char* argv[] = {"/c", "timeout", "/t", "2", "/nobreak", ">nul", NULL};
#else
    const char* command = "sleep";
    const char* argv[] = {"2", NULL};
#endif
    
    vox_process_t* proc = vox_process_create(mpool, command, argv, NULL);
    if (!proc) {
        fprintf(stderr, "创建进程失败\n");
        vox_mpool_destroy(mpool);
        return;
    }
    
    printf("进程创建成功，PID: %llu\n", (unsigned long long)vox_process_get_id(proc));
    
    /* 检查进程状态 */
    if (vox_process_is_running(proc)) {
        printf("进程正在运行\n");
    } else {
        printf("进程已退出\n");
    }
    
    /* 获取进程状态（不等待） */
    vox_process_status_t status;
    int result = vox_process_get_status(proc, &status);
    if (result == 1) {
        printf("进程仍在运行\n");
    } else if (result == 0) {
        printf("进程已退出，退出码: %d\n", status.exit_code);
    }
    
    /* 等待进程结束 */
    printf("等待进程结束...\n");
    if (vox_process_wait(proc, &status, 0) == 0) {
        printf("进程已结束，退出码: %d\n", status.exit_code);
    }
    
    /* 再次检查状态 */
    if (vox_process_is_running(proc)) {
        printf("警告：进程应该已经退出\n");
    } else {
        printf("进程状态检查正确：已退出\n");
    }
    
    vox_process_destroy(proc);
    vox_mpool_destroy(mpool);
}

/* 测试进程终止 */
void test_process_terminate(void) {
    printf("\n=== 测试进程终止 ===\n");
    
    vox_mpool_t* mpool = vox_mpool_create();
    if (!mpool) {
        fprintf(stderr, "创建内存池失败\n");
        return;
    }
    
#ifdef VOX_OS_WINDOWS
    const char* command = "cmd.exe";
    const char* argv[] = {"/c", "timeout", "/t", "10", "/nobreak", ">nul", NULL};
#else
    const char* command = "sleep";
    const char* argv[] = {"10", NULL};
#endif
    
    vox_process_t* proc = vox_process_create(mpool, command, argv, NULL);
    if (!proc) {
        fprintf(stderr, "创建进程失败\n");
        vox_mpool_destroy(mpool);
        return;
    }
    
    printf("进程创建成功，PID: %llu\n", (unsigned long long)vox_process_get_id(proc));
    printf("等待1秒后终止进程...\n");
    
#ifdef VOX_OS_WINDOWS
    Sleep(1000);
#else
    sleep(1);
#endif
    
    /* 正常终止 */
    if (vox_process_terminate(proc, false) == 0) {
        printf("发送终止信号成功\n");
    } else {
        printf("发送终止信号失败\n");
    }
    
    /* 等待进程结束 */
    vox_process_status_t status;
    if (vox_process_wait(proc, &status, 5000) == 0) {
        if (status.exited) {
            printf("进程已退出，退出码: %d\n", status.exit_code);
        } else if (status.signaled) {
            printf("进程被信号终止，信号: %d\n", status.signal);
        }
    } else {
        printf("等待进程超时，尝试强制终止...\n");
        vox_process_terminate(proc, true);
        vox_process_wait(proc, &status, 0);
    }
    
    vox_process_destroy(proc);
    vox_mpool_destroy(mpool);
}

/* 测试工作目录设置 */
void test_working_directory(void) {
    printf("\n=== 测试工作目录设置 ===\n");
    
    vox_mpool_t* mpool = vox_mpool_create();
    if (!mpool) {
        fprintf(stderr, "创建内存池失败\n");
        return;
    }
    
    vox_process_options_t opts;
    memset(&opts, 0, sizeof(opts));
    opts.stdout_redirect = VOX_PROCESS_REDIRECT_PIPE;
    
#ifdef VOX_OS_WINDOWS
    opts.working_dir = "C:\\";
    const char* command = "cmd.exe";
    const char* argv[] = {"/c", "cd", NULL};
#else
    opts.working_dir = "/";
    const char* command = "pwd";
    const char* argv[] = {NULL};  /* pwd 不需要参数 */
#endif
    
    vox_process_t* proc = vox_process_create(mpool, command, argv, &opts);
    if (!proc) {
        fprintf(stderr, "创建进程失败");
        #ifndef VOX_OS_WINDOWS
            if (errno != 0) {
                fprintf(stderr, ": %s (errno=%d)", strerror(errno), errno);
            }
        #endif
        fprintf(stderr, "\n");
        vox_mpool_destroy(mpool);
        return;
    }
    
    /* 读取输出 */
    char buffer[1024];
    int64_t bytes_read = vox_process_read_stdout(proc, buffer, sizeof(buffer) - 1);
    if (bytes_read > 0) {
        buffer[bytes_read] = '\0';
        printf("工作目录输出: %s", buffer);
    }
    
    /* 等待进程结束 */
    vox_process_status_t status;
    vox_process_wait(proc, &status, 0);
    
    vox_process_destroy(proc);
    vox_mpool_destroy(mpool);
}

int main(void) {
    printf("========================================\n");
    printf("    vox_process 示例程序\n");
    printf("========================================\n");
    
    /* 测试基本功能 */
    test_process_ids();
    test_environment();
    
    /* 测试进程创建和管理 */
    test_basic_process();
    test_process_status();
    test_process_terminate();
    
    /* 测试输入输出 */
    test_process_output();
    test_process_input();
    
    /* 测试便捷函数 */
    test_execute_function();
    
    /* 测试工作目录 */
    test_working_directory();
    
    printf("\n========================================\n");
    printf("    所有测试完成\n");
    printf("========================================\n");
    
    return 0;
}
