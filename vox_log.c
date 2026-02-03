/* ============================================================
 * vox_log.c - 日志模块实现
 * ============================================================ */

#include "vox_log.h"
#include "vox_time.h"
#include <stdio.h>
#include <string.h>

#define VOX_LOG_MAX_MESSAGE_SIZE 4096

static vox_log_level_t g_log_level = VOX_LOG_INFO;
static vox_log_callback_t g_log_callback = NULL;
static void *g_log_userdata = NULL;

static vox_log_options_t g_log_options = {
    1,  /* show_time */
    1,  /* show_file_line */
    1   /* show_func */
};

static const char *level_strings[] = {
    "FATAL", "ERROR", "WARN", "INFO", "DEBUG", "TRACE"
};

static const char *level_colors[] = {
    "\x1b[0;35m", "\x1b[0;31m", "\x1b[0;33m", "\x1b[0;32m", "\x1b[0;37m", "\x1b[0;38m"
};

void vox_log_set_level(vox_log_level_t level) {
    g_log_level = level;
}

vox_log_level_t vox_log_get_level(void) {
    return g_log_level;
}

void vox_log_set_options(const vox_log_options_t *opts) {
    if (opts) {
        g_log_options.show_time = opts->show_time ? 1 : 0;
        g_log_options.show_file_line = opts->show_file_line ? 1 : 0;
        g_log_options.show_func = opts->show_func ? 1 : 0;
    }
}

void vox_log_get_options(vox_log_options_t *opts) {
    if (opts) {
        opts->show_time = g_log_options.show_time;
        opts->show_file_line = g_log_options.show_file_line;
        opts->show_func = g_log_options.show_func;
    }
}

void vox_log_set_callback(vox_log_callback_t callback, void *userdata) {
    g_log_callback = callback;
    g_log_userdata = userdata;
}

void vox_log_write(vox_log_level_t level,
                   const char *file,
                   int line,
                   const char *func,
                   const char *fmt, ...) {
    if (level > g_log_level) {
        return;
    }

    char msg[VOX_LOG_MAX_MESSAGE_SIZE];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);

    if (g_log_callback) {
        g_log_callback(level_strings[level], file, line, func, msg, g_log_userdata);
    } else {
        fprintf(stderr, "%s%s", level_colors[level], level_strings[level]);
        if (g_log_options.show_time) {
            vox_time_t now = vox_time_now();
            char time_str[32];
            vox_time_format(now, time_str, sizeof(time_str));
            fprintf(stderr, " [%s]", time_str);
        }
        if (g_log_options.show_file_line) {
            const char *filename = strrchr(file, '/');
            if (!filename) filename = strrchr(file, '\\');
            filename = filename ? filename + 1 : file;
            fprintf(stderr, " %s:%d", filename, line);
        }
        if (g_log_options.show_func) {
            fprintf(stderr, " %s", func);
        }
        fprintf(stderr, " - %s\x1b[0m\n", msg);
    }
}
