/* ============================================================
 * vox_log.h - 日志模块头文件
 * ============================================================ */

#ifndef VOX_LOG_H
#define VOX_LOG_H

#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>

/* 日志级别 */
typedef enum {
    VOX_LOG_FATAL = 0,
    VOX_LOG_ERROR,
    VOX_LOG_WARN,
    VOX_LOG_INFO,
    VOX_LOG_DEBUG,
    VOX_LOG_TRACE
} vox_log_level_t;

/* 日志输出回调 */
typedef void (*vox_log_callback_t)(const char *level, 
                                    const char *file,
                                    int line,
                                    const char *func,
                                    const char *msg,
                                    void *userdata);

/* 设置日志级别 */
void vox_log_set_level(vox_log_level_t level);
vox_log_level_t vox_log_get_level(void);

/* 设置日志回调 */
void vox_log_set_callback(vox_log_callback_t callback, void *userdata);

/* 日志输出 */
void vox_log_write(vox_log_level_t level,
                            const char *file,
                            int line,
                            const char *func,
                            const char *fmt, ...);

/* 日志宏 */
#define VOX_LOG_TRACE(...) vox_log_write(VOX_LOG_TRACE, __FILE__, __LINE__, __func__, __VA_ARGS__)
#define VOX_LOG_DEBUG(...) vox_log_write(VOX_LOG_DEBUG, __FILE__, __LINE__, __func__, __VA_ARGS__)
#define VOX_LOG_INFO(...)  vox_log_write(VOX_LOG_INFO,  __FILE__, __LINE__, __func__, __VA_ARGS__)
#define VOX_LOG_WARN(...)  vox_log_write(VOX_LOG_WARN,  __FILE__, __LINE__, __func__, __VA_ARGS__)
#define VOX_LOG_ERROR(...) vox_log_write(VOX_LOG_ERROR, __FILE__, __LINE__, __func__, __VA_ARGS__)
#define VOX_LOG_FATAL(...) vox_log_write(VOX_LOG_FATAL, __FILE__, __LINE__, __func__, __VA_ARGS__)

#endif /* VOX_LOG_H */
