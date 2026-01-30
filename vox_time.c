/**
 * vox_time.c - 跨平台时间处理模块实现
 */

#include "vox_time.h"
#include "vox_os.h"
#include <time.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#ifdef VOX_OS_WINDOWS
    #include <synchapi.h>
#else
    #include <sys/time.h>
    #include <unistd.h>
#endif

/* ===== 获取当前时间（微秒） ===== */
vox_time_t vox_time_now(void) {
#ifdef VOX_OS_WINDOWS
    FILETIME ft;
    ULARGE_INTEGER uli;
    GetSystemTimeAsFileTime(&ft);
    uli.LowPart = ft.dwLowDateTime;
    uli.HighPart = ft.dwHighDateTime;
    /* Windows文件时间从1601年开始，100纳秒为单位 */
    /* 转换为Unix时间戳（微秒） */
    return (vox_time_t)((uli.QuadPart / 10) - 11644473600000000ULL);
#else
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (vox_time_t)tv.tv_sec * 1000000 + tv.tv_usec;
#endif
}

/* ===== 获取单调时间（微秒） ===== */
vox_time_t vox_time_monotonic(void) {
#ifdef VOX_OS_WINDOWS
    static LARGE_INTEGER frequency = {0};
    LARGE_INTEGER counter;
    
    if (frequency.QuadPart == 0) {
        QueryPerformanceFrequency(&frequency);
    }
    
    QueryPerformanceCounter(&counter);
    return (vox_time_t)((counter.QuadPart * 1000000) / frequency.QuadPart);
#elif defined(CLOCK_MONOTONIC)
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (vox_time_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
#else
    /* 回退到系统时间 */
    return vox_time_now();
#endif
}

/* ===== 时间格式化 ===== */
void vox_time_format(vox_time_t t, char *buf, size_t size) {
    time_t sec = (time_t)(t / 1000000);
    int32_t usec = (int32_t)(t % 1000000);
    
    struct tm tm_info;
#ifdef VOX_OS_WINDOWS
    localtime_s(&tm_info, &sec);
#else
    localtime_r(&sec, &tm_info);
#endif
    
    snprintf(buf, size, "%04d-%02d-%02d %02d:%02d:%02d.%06d",
             tm_info.tm_year + 1900,
             tm_info.tm_mon + 1,
             tm_info.tm_mday,
             tm_info.tm_hour,
             tm_info.tm_min,
             tm_info.tm_sec,
             usec);
}

/* ===== 获取UTC时间（微秒） ===== */
vox_time_t vox_time_utc(void) {
#ifdef VOX_OS_WINDOWS
    FILETIME ft;
    ULARGE_INTEGER uli;
    GetSystemTimeAsFileTime(&ft);
    uli.LowPart = ft.dwLowDateTime;
    uli.HighPart = ft.dwHighDateTime;
    /* Windows文件时间从1601年开始，100纳秒为单位 */
    /* 转换为Unix时间戳（微秒） */
    return (vox_time_t)((uli.QuadPart / 10) - 11644473600000000ULL);
#else
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (vox_time_t)tv.tv_sec * 1000000 + tv.tv_usec;
#endif
}

/* ===== 获取GMT时间（微秒） ===== */
/* GMT和UTC在大多数情况下相同，但提供独立函数以保持API一致性 */
vox_time_t vox_time_gmt(void) {
    return vox_time_utc();
}

/* ===== 时间格式化（ISO 8601） ===== */
void vox_time_format_iso8601(vox_time_t t, char *buf, size_t size) {
    time_t sec = (time_t)(t / 1000000);
    int32_t usec = (int32_t)(t % 1000000);
    
    struct tm tm_info;
#ifdef VOX_OS_WINDOWS
    gmtime_s(&tm_info, &sec);
#else
    gmtime_r(&sec, &tm_info);
#endif
    
    if (usec > 0) {
        snprintf(buf, size, "%04d-%02d-%02dT%02d:%02d:%02d.%06dZ",
                 tm_info.tm_year + 1900,
                 tm_info.tm_mon + 1,
                 tm_info.tm_mday,
                 tm_info.tm_hour,
                 tm_info.tm_min,
                 tm_info.tm_sec,
                 usec);
    } else {
        snprintf(buf, size, "%04d-%02d-%02dT%02d:%02d:%02dZ",
                 tm_info.tm_year + 1900,
                 tm_info.tm_mon + 1,
                 tm_info.tm_mday,
                 tm_info.tm_hour,
                 tm_info.tm_min,
                 tm_info.tm_sec);
    }
}

/* ===== 时间格式化（GMT） ===== */
void vox_time_format_gmt(vox_time_t t, char *buf, size_t size) {
    time_t sec = (time_t)(t / 1000000);
    int32_t usec = (int32_t)(t % 1000000);
    
    struct tm tm_info;
#ifdef VOX_OS_WINDOWS
    gmtime_s(&tm_info, &sec);
#else
    gmtime_r(&sec, &tm_info);
#endif
    
    if (usec > 0) {
        snprintf(buf, size, "%04d-%02d-%02d %02d:%02d:%02d.%06d GMT",
                 tm_info.tm_year + 1900,
                 tm_info.tm_mon + 1,
                 tm_info.tm_mday,
                 tm_info.tm_hour,
                 tm_info.tm_min,
                 tm_info.tm_sec,
                 usec);
    } else {
        snprintf(buf, size, "%04d-%02d-%02d %02d:%02d:%02d GMT",
                 tm_info.tm_year + 1900,
                 tm_info.tm_mon + 1,
                 tm_info.tm_mday,
                 tm_info.tm_hour,
                 tm_info.tm_min,
                 tm_info.tm_sec);
    }
}

/* ===== 时间格式化（自定义格式） ===== */
int vox_time_format_custom(vox_time_t t, const char *fmt, char *buf, size_t size) {
    if (!fmt || !buf || size == 0) return -1;
    
    time_t sec = (time_t)(t / 1000000);
    int32_t usec = (int32_t)(t % 1000000);
    
    struct tm tm_info;
#ifdef VOX_OS_WINDOWS
    localtime_s(&tm_info, &sec);
#else
    localtime_r(&sec, &tm_info);
#endif
    
    char* dst = buf;
    const char* src = fmt;
    size_t remaining = size - 1;  /* 保留一个字符给 '\0' */
    
    while (*src && remaining > 0) {
        if (*src == '%' && src[1]) {
            src++;  /* 跳过 % */
            switch (*src) {
                case 'Y':  /* 年份（4位） */
                    dst += snprintf(dst, remaining, "%04d", tm_info.tm_year + 1900);
                    break;
                case 'y':  /* 年份（2位） */
                    dst += snprintf(dst, remaining, "%02d", (tm_info.tm_year + 1900) % 100);
                    break;
                case 'm':  /* 月份 */
                    dst += snprintf(dst, remaining, "%02d", tm_info.tm_mon + 1);
                    break;
                case 'd':  /* 日期 */
                    dst += snprintf(dst, remaining, "%02d", tm_info.tm_mday);
                    break;
                case 'H':  /* 小时（24小时制） */
                    dst += snprintf(dst, remaining, "%02d", tm_info.tm_hour);
                    break;
                case 'M':  /* 分钟 */
                    dst += snprintf(dst, remaining, "%02d", tm_info.tm_min);
                    break;
                case 'S':  /* 秒 */
                    dst += snprintf(dst, remaining, "%02d", tm_info.tm_sec);
                    break;
                case 'f':  /* 微秒 */
                    dst += snprintf(dst, remaining, "%06d", usec);
                    break;
                case 'w':  /* 星期几（0-6） */
                    dst += snprintf(dst, remaining, "%d", tm_info.tm_wday);
                    break;
                case '%':  /* 转义的 % */
                    *dst++ = '%';
                    remaining--;
                    break;
                default:
                    *dst++ = '%';
                    *dst++ = *src;
                    remaining -= 2;
                    break;
            }
            src++;
            remaining = size - (dst - buf) - 1;
        } else {
            *dst++ = *src++;
            remaining--;
        }
    }
    
    *dst = '\0';
    return (int)(dst - buf);
}

/* ===== 时间解析 ===== */
vox_time_t vox_time_parse_iso8601(const char *str) {
    if (!str) return -1;
    
    struct tm tm_info = {0};
    int year, month, day, hour, minute, second, usec = 0;
    char tz = 0;
    
    /* 解析格式：YYYY-MM-DDTHH:MM:SS[.ffffff][Z] */
    if (sscanf(str, "%d-%d-%dT%d:%d:%d.%d%c", 
               &year, &month, &day, &hour, &minute, &second, &usec, &tz) >= 6 ||
        sscanf(str, "%d-%d-%dT%d:%d:%d%c", 
               &year, &month, &day, &hour, &minute, &second, &tz) >= 6) {
        tm_info.tm_year = year - 1900;
        tm_info.tm_mon = month - 1;
        tm_info.tm_mday = day;
        tm_info.tm_hour = hour;
        tm_info.tm_min = minute;
        tm_info.tm_sec = second;
        tm_info.tm_isdst = -1;
        
        time_t sec;
#ifdef VOX_OS_WINDOWS
        sec = _mkgmtime(&tm_info);
#else
        /* timegm 不是标准函数，使用回退实现 */
        #ifdef __USE_GNU
            sec = timegm(&tm_info);
        #else
            /* 回退：使用 mktime 并手动调整时区 */
            /* 先假设是本地时间，转换为UTC */
            time_t local_sec = mktime(&tm_info);
            if (local_sec == -1) return -1;
            
            /* 获取本地时间和UTC时间的差值 */
            struct tm local_tm_copy = tm_info;
            time_t local_time = mktime(&local_tm_copy);
            
            /* 计算时区偏移 */
            struct tm* utc_tm = gmtime(&local_time);
            if (!utc_tm) return -1;
            time_t utc_from_local = mktime(utc_tm);
            int tz_offset = (int)(local_time - utc_from_local);
            
            sec = local_sec - tz_offset;
        #endif
#endif
        if (sec == -1) return -1;
        
        return (vox_time_t)sec * 1000000 + usec;
    }
    
    return -1;
}

vox_time_t vox_time_parse(const char *str) {
    if (!str) return -1;
    
    struct tm tm_info = {0};
    int year, month, day, hour, minute, second, usec = 0;
    
    /* 解析格式：YYYY-MM-DD HH:MM:SS[.ffffff] */
    if (sscanf(str, "%d-%d-%d %d:%d:%d.%d", 
               &year, &month, &day, &hour, &minute, &second, &usec) >= 6 ||
        sscanf(str, "%d-%d-%d %d:%d:%d", 
               &year, &month, &day, &hour, &minute, &second) >= 6) {
        tm_info.tm_year = year - 1900;
        tm_info.tm_mon = month - 1;
        tm_info.tm_mday = day;
        tm_info.tm_hour = hour;
        tm_info.tm_min = minute;
        tm_info.tm_sec = second;
        tm_info.tm_isdst = -1;
        
        time_t sec = mktime(&tm_info);
        if (sec == -1) return -1;
        
        return (vox_time_t)sec * 1000000 + usec;
    }
    
    return -1;
}

/* ===== 时间结构体操作 ===== */
int vox_time_to_struct(vox_time_t t, vox_time_struct_t *tm) {
    if (!tm) return -1;
    
    time_t sec = (time_t)(t / 1000000);
    int32_t usec = (int32_t)(t % 1000000);
    
    struct tm tm_info;
#ifdef VOX_OS_WINDOWS
    localtime_s(&tm_info, &sec);
#else
    localtime_r(&sec, &tm_info);
#endif
    
    tm->year = tm_info.tm_year + 1900;
    tm->month = tm_info.tm_mon + 1;
    tm->day = tm_info.tm_mday;
    tm->hour = tm_info.tm_hour;
    tm->minute = tm_info.tm_min;
    tm->second = tm_info.tm_sec;
    tm->microsecond = usec;
    tm->weekday = tm_info.tm_wday;
    tm->yearday = tm_info.tm_yday + 1;
    
    return 0;
}

vox_time_t vox_time_from_struct(const vox_time_struct_t *tm) {
    if (!tm) return -1;
    
    struct tm tm_info = {0};
    tm_info.tm_year = tm->year - 1900;
    tm_info.tm_mon = tm->month - 1;
    tm_info.tm_mday = tm->day;
    tm_info.tm_hour = tm->hour;
    tm_info.tm_min = tm->minute;
    tm_info.tm_sec = tm->second;
    tm_info.tm_isdst = -1;
    
    time_t sec = mktime(&tm_info);
    if (sec == -1) return -1;
    
    return (vox_time_t)sec * 1000000 + tm->microsecond;
}

int vox_time_to_struct_utc(vox_time_t t, vox_time_struct_t *tm) {
    if (!tm) return -1;
    
    time_t sec = (time_t)(t / 1000000);
    int32_t usec = (int32_t)(t % 1000000);
    
    struct tm tm_info;
#ifdef VOX_OS_WINDOWS
    gmtime_s(&tm_info, &sec);
#else
    gmtime_r(&sec, &tm_info);
#endif
    
    tm->year = tm_info.tm_year + 1900;
    tm->month = tm_info.tm_mon + 1;
    tm->day = tm_info.tm_mday;
    tm->hour = tm_info.tm_hour;
    tm->minute = tm_info.tm_min;
    tm->second = tm_info.tm_sec;
    tm->microsecond = usec;
    tm->weekday = tm_info.tm_wday;
    tm->yearday = tm_info.tm_yday + 1;
    
    return 0;
}

vox_time_t vox_time_from_struct_utc(const vox_time_struct_t *tm) {
    if (!tm) return -1;
    
    struct tm tm_info = {0};
    tm_info.tm_year = tm->year - 1900;
    tm_info.tm_mon = tm->month - 1;
    tm_info.tm_mday = tm->day;
    tm_info.tm_hour = tm->hour;
    tm_info.tm_min = tm->minute;
    tm_info.tm_sec = tm->second;
    tm_info.tm_isdst = 0;
    
    time_t sec;
#ifdef VOX_OS_WINDOWS
    sec = _mkgmtime(&tm_info);
    #else
        /* timegm 不是标准函数，使用回退实现 */
        #ifdef __USE_GNU
            sec = timegm(&tm_info);
        #else
            /* 回退：使用 mktime 并手动调整时区 */
            time_t local_sec = mktime(&tm_info);
            if (local_sec == -1) return -1;
            
            /* 获取本地时间和UTC时间的差值 */
            struct tm* utc_tm = gmtime(&local_sec);
            if (!utc_tm) return -1;
            struct tm utc_tm_copy = *utc_tm;
            time_t utc_from_local = mktime(&utc_tm_copy);
            int tz_offset = (int)(local_sec - utc_from_local);
            
            sec = local_sec - tz_offset;
        #endif
    #endif
    if (sec == -1) return -1;
    
    return (vox_time_t)sec * 1000000 + tm->microsecond;
}

/* ===== GMT时间结构体操作 ===== */
int vox_time_to_struct_gmt(vox_time_t t, vox_time_struct_t *tm) {
    /* GMT和UTC相同，直接调用UTC函数 */
    return vox_time_to_struct_utc(t, tm);
}

vox_time_t vox_time_from_struct_gmt(const vox_time_struct_t *tm) {
    /* GMT和UTC相同，直接调用UTC函数 */
    return vox_time_from_struct_utc(tm);
}

/* ===== 时间运算 ===== */
vox_time_t vox_time_add(vox_time_t t, int64_t delta) {
    return t + delta;
}

vox_time_t vox_time_sub(vox_time_t t, int64_t delta) {
    return t - delta;
}

int vox_time_compare(vox_time_t t1, vox_time_t t2) {
    if (t1 < t2) return -1;
    if (t1 > t2) return 1;
    return 0;
}

int64_t vox_time_diff_sec(vox_time_t t1, vox_time_t t2) {
    return (t1 - t2) / 1000000;
}

/* ===== 时间差（毫秒） ===== */
int64_t vox_time_diff_ms(vox_time_t t1, vox_time_t t2) {
    return (t1 - t2) / 1000;
}

int64_t vox_time_diff_us(vox_time_t t1, vox_time_t t2) {
    return t1 - t2;
}

/* ===== 时间戳转换 ===== */
vox_time_t vox_time_from_sec(int64_t sec) {
    return (vox_time_t)sec * 1000000;
}

vox_time_t vox_time_from_ms(int64_t ms) {
    return (vox_time_t)ms * 1000;
}

int64_t vox_time_to_sec(vox_time_t t) {
    return t / 1000000;
}

int64_t vox_time_to_ms(vox_time_t t) {
    return t / 1000;
}

/* ===== 时间组件获取 ===== */
int vox_time_year(vox_time_t t) {
    time_t sec = (time_t)(t / 1000000);
    struct tm tm_info;
#ifdef VOX_OS_WINDOWS
    localtime_s(&tm_info, &sec);
#else
    localtime_r(&sec, &tm_info);
#endif
    return tm_info.tm_year + 1900;
}

int vox_time_month(vox_time_t t) {
    time_t sec = (time_t)(t / 1000000);
    struct tm tm_info;
#ifdef VOX_OS_WINDOWS
    localtime_s(&tm_info, &sec);
#else
    localtime_r(&sec, &tm_info);
#endif
    return tm_info.tm_mon + 1;
}

int vox_time_day(vox_time_t t) {
    time_t sec = (time_t)(t / 1000000);
    struct tm tm_info;
#ifdef VOX_OS_WINDOWS
    localtime_s(&tm_info, &sec);
#else
    localtime_r(&sec, &tm_info);
#endif
    return tm_info.tm_mday;
}

int vox_time_hour(vox_time_t t) {
    time_t sec = (time_t)(t / 1000000);
    struct tm tm_info;
#ifdef VOX_OS_WINDOWS
    localtime_s(&tm_info, &sec);
#else
    localtime_r(&sec, &tm_info);
#endif
    return tm_info.tm_hour;
}

int vox_time_minute(vox_time_t t) {
    time_t sec = (time_t)(t / 1000000);
    struct tm tm_info;
#ifdef VOX_OS_WINDOWS
    localtime_s(&tm_info, &sec);
#else
    localtime_r(&sec, &tm_info);
#endif
    return tm_info.tm_min;
}

int vox_time_second(vox_time_t t) {
    time_t sec = (time_t)(t / 1000000);
    struct tm tm_info;
#ifdef VOX_OS_WINDOWS
    localtime_s(&tm_info, &sec);
#else
    localtime_r(&sec, &tm_info);
#endif
    return tm_info.tm_sec;
}

int vox_time_microsecond(vox_time_t t) {
    return (int)(t % 1000000);
}

int vox_time_weekday(vox_time_t t) {
    time_t sec = (time_t)(t / 1000000);
    struct tm tm_info;
#ifdef VOX_OS_WINDOWS
    localtime_s(&tm_info, &sec);
#else
    localtime_r(&sec, &tm_info);
#endif
    return tm_info.tm_wday;
}

/* ===== 睡眠 ===== */
void vox_time_sleep_sec(int64_t sec) {
    if (sec <= 0) return;
    
#ifdef VOX_OS_WINDOWS
    Sleep((DWORD)(sec * 1000));
#else
    sleep((unsigned int)sec);
#endif
}

void vox_time_sleep_ms(int64_t ms) {
    if (ms <= 0) return;
    
#ifdef VOX_OS_WINDOWS
    Sleep((DWORD)ms);
#else
    usleep((useconds_t)(ms * 1000));
#endif
}

void vox_time_sleep_us(int64_t us) {
    if (us <= 0) return;
    
#ifdef VOX_OS_WINDOWS
    Sleep((DWORD)((us + 999) / 1000));  /* 向上取整到毫秒 */
#else
    usleep((useconds_t)us);
#endif
}
