/*
 * vox_time.h - 时间处理模块
 * 提供跨平台的时间操作接口
*/

#ifndef VOX_TIME_H
#define VOX_TIME_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 时间类型（微秒） */
typedef int64_t vox_time_t;

/* 时间结构体 */
typedef struct {
    int year;        /* 年份（如 2024） */
    int month;       /* 月份（1-12） */
    int day;         /* 日期（1-31） */
    int hour;        /* 小时（0-23） */
    int minute;      /* 分钟（0-59） */
    int second;      /* 秒（0-59） */
    int microsecond; /* 微秒（0-999999） */
    int weekday;     /* 星期几（0=周日，1=周一，...，6=周六） */
    int yearday;     /* 一年中的第几天（1-366） */
} vox_time_struct_t;

/* ===== 时间获取 ===== */

/**
 * 获取当前时间（微秒，Unix时间戳）
 * @return 返回当前时间的微秒数
 */
vox_time_t vox_time_now(void);

/**
 * 获取单调时间（微秒，不受系统时间调整影响）
 * @return 返回单调时间的微秒数
 */
vox_time_t vox_time_monotonic(void);

/**
 * 获取UTC时间（微秒）
 * @return 返回UTC时间的微秒数
 */
vox_time_t vox_time_utc(void);

/**
 * 获取GMT时间（微秒，GMT和UTC相同）
 * @return 返回GMT时间的微秒数
 */
vox_time_t vox_time_gmt(void);

/* ===== 时间格式化 ===== */

/**
 * 时间格式化（默认格式：YYYY-MM-DD HH:MM:SS.ffffff）
 * @param t 时间（微秒）
 * @param buf 输出缓冲区
 * @param size 缓冲区大小
 */
void vox_time_format(vox_time_t t, char *buf, size_t size);

/**
 * 时间格式化（ISO 8601格式：YYYY-MM-DDTHH:MM:SS.ffffffZ）
 * @param t 时间（微秒）
 * @param buf 输出缓冲区
 * @param size 缓冲区大小
 */
void vox_time_format_iso8601(vox_time_t t, char *buf, size_t size);

/**
 * 时间格式化（GMT格式：YYYY-MM-DD HH:MM:SS.ffffff GMT）
 * @param t 时间（微秒）
 * @param buf 输出缓冲区
 * @param size 缓冲区大小
 */
void vox_time_format_gmt(vox_time_t t, char *buf, size_t size);

/**
 * 时间格式化（自定义格式）
 * @param t 时间（微秒）
 * @param fmt 格式字符串（类似strftime，支持：%Y %m %d %H %M %S %f %w等）
 * @param buf 输出缓冲区
 * @param size 缓冲区大小
 * @return 成功返回写入的字符数，失败返回-1
 */
int vox_time_format_custom(vox_time_t t, const char *fmt, char *buf, size_t size);

/* ===== 时间解析 ===== */

/**
 * 解析时间字符串（ISO 8601格式：YYYY-MM-DDTHH:MM:SS[.ffffff][Z]）
 * @param str 时间字符串
 * @return 成功返回时间（微秒），失败返回-1
 */
vox_time_t vox_time_parse_iso8601(const char *str);

/**
 * 解析时间字符串（格式：YYYY-MM-DD HH:MM:SS[.ffffff]）
 * @param str 时间字符串
 * @return 成功返回时间（微秒），失败返回-1
 */
vox_time_t vox_time_parse(const char *str);

/* ===== 时间结构体操作 ===== */

/**
 * 将时间转换为结构体（本地时间）
 * @param t 时间（微秒）
 * @param tm 输出时间结构体
 * @return 成功返回0，失败返回-1
 */
int vox_time_to_struct(vox_time_t t, vox_time_struct_t *tm);

/**
 * 从结构体创建时间（本地时间）
 * @param tm 时间结构体
 * @return 成功返回时间（微秒），失败返回-1
 */
vox_time_t vox_time_from_struct(const vox_time_struct_t *tm);

/**
 * 将时间转换为结构体（UTC时间）
 * @param t 时间（微秒）
 * @param tm 输出时间结构体
 * @return 成功返回0，失败返回-1
 */
int vox_time_to_struct_utc(vox_time_t t, vox_time_struct_t *tm);

/**
 * 从结构体创建时间（UTC时间）
 * @param tm 时间结构体
 * @return 成功返回时间（微秒），失败返回-1
 */
vox_time_t vox_time_from_struct_utc(const vox_time_struct_t *tm);

/**
 * 将时间转换为结构体（GMT时间）
 * @param t 时间（微秒）
 * @param tm 输出时间结构体
 * @return 成功返回0，失败返回-1
 */
int vox_time_to_struct_gmt(vox_time_t t, vox_time_struct_t *tm);

/**
 * 从结构体创建时间（GMT时间）
 * @param tm 时间结构体
 * @return 成功返回时间（微秒），失败返回-1
 */
vox_time_t vox_time_from_struct_gmt(const vox_time_struct_t *tm);

/* ===== 时间运算 ===== */

/**
 * 时间加法
 * @param t 时间（微秒）
 * @param delta 增量（微秒）
 * @return 返回新的时间
 */
vox_time_t vox_time_add(vox_time_t t, int64_t delta);

/**
 * 时间减法
 * @param t 时间（微秒）
 * @param delta 减量（微秒）
 * @return 返回新的时间
 */
vox_time_t vox_time_sub(vox_time_t t, int64_t delta);

/**
 * 时间比较
 * @param t1 时间1
 * @param t2 时间2
 * @return t1 < t2 返回-1，t1 == t2 返回0，t1 > t2 返回1
 */
int vox_time_compare(vox_time_t t1, vox_time_t t2);

/**
 * 时间差（秒）
 * @param t1 时间1
 * @param t2 时间2
 * @return 返回时间差（秒）
 */
int64_t vox_time_diff_sec(vox_time_t t1, vox_time_t t2);

/**
 * 时间差（毫秒）
 * @param t1 时间1
 * @param t2 时间2
 * @return 返回时间差（毫秒）
 */
int64_t vox_time_diff_ms(vox_time_t t1, vox_time_t t2);

/**
 * 时间差（微秒）
 * @param t1 时间1
 * @param t2 时间2
 * @return 返回时间差（微秒）
 */
int64_t vox_time_diff_us(vox_time_t t1, vox_time_t t2);

/* ===== 时间戳转换 ===== */

/**
 * 从Unix时间戳（秒）创建时间
 * @param sec Unix时间戳（秒）
 * @return 返回时间（微秒）
 */
vox_time_t vox_time_from_sec(int64_t sec);

/**
 * 从Unix时间戳（毫秒）创建时间
 * @param ms Unix时间戳（毫秒）
 * @return 返回时间（微秒）
 */
vox_time_t vox_time_from_ms(int64_t ms);

/**
 * 转换为Unix时间戳（秒）
 * @param t 时间（微秒）
 * @return 返回Unix时间戳（秒）
 */
int64_t vox_time_to_sec(vox_time_t t);

/**
 * 转换为Unix时间戳（毫秒）
 * @param t 时间（微秒）
 * @return 返回Unix时间戳（毫秒）
 */
int64_t vox_time_to_ms(vox_time_t t);

/* ===== 时间组件获取 ===== */

/**
 * 获取年份
 * @param t 时间（微秒）
 * @return 返回年份
 */
int vox_time_year(vox_time_t t);

/**
 * 获取月份（1-12）
 * @param t 时间（微秒）
 * @return 返回月份
 */
int vox_time_month(vox_time_t t);

/**
 * 获取日期（1-31）
 * @param t 时间（微秒）
 * @return 返回日期
 */
int vox_time_day(vox_time_t t);

/**
 * 获取小时（0-23）
 * @param t 时间（微秒）
 * @return 返回小时
 */
int vox_time_hour(vox_time_t t);

/**
 * 获取分钟（0-59）
 * @param t 时间（微秒）
 * @return 返回分钟
 */
int vox_time_minute(vox_time_t t);

/**
 * 获取秒（0-59）
 * @param t 时间（微秒）
 * @return 返回秒
 */
int vox_time_second(vox_time_t t);

/**
 * 获取微秒（0-999999）
 * @param t 时间（微秒）
 * @return 返回微秒
 */
int vox_time_microsecond(vox_time_t t);

/**
 * 获取星期几（0=周日，1=周一，...，6=周六）
 * @param t 时间（微秒）
 * @return 返回星期几
 */
int vox_time_weekday(vox_time_t t);

/* ===== 睡眠 ===== */

/**
 * 睡眠（秒）
 * @param sec 秒数
 */
void vox_time_sleep_sec(int64_t sec);

/**
 * 睡眠（毫秒）
 * @param ms 毫秒数
 */
void vox_time_sleep_ms(int64_t ms);

/**
 * 睡眠（微秒）
 * @param us 微秒数
 */
void vox_time_sleep_us(int64_t us);

/* ===== 时间转换宏 ===== */

/**
 * 毫秒转微秒
 */
#define VOX_TIME_MS(ms) ((vox_time_t)(ms) * 1000)

/**
 * 秒转微秒
 */
#define VOX_TIME_SEC(s) ((vox_time_t)(s) * 1000000)

/**
 * 分钟转微秒
 */
#define VOX_TIME_MIN(m) ((vox_time_t)(m) * 60000000)

/**
 * 小时转微秒
 */
#define VOX_TIME_HOUR(h) ((vox_time_t)(h) * 3600000000LL)

/**
 * 天转微秒
 */
#define VOX_TIME_DAY(d) ((vox_time_t)(d) * 86400000000LL)

#ifdef __cplusplus
}
#endif

#endif /* VOX_TIME_H */
