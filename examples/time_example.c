/*
 * time_example.c - 时间操作示例程序
 * 演示 vox_time 的各种时间操作功能
 */

#include "../vox_time.h"
#include <stdio.h>
#include <string.h>

int main(void) {
    printf("=== 获取当前时间 ===\n");
    vox_time_t now = vox_time_now();
    char time_str[64];
    vox_time_format(now, time_str, sizeof(time_str));
    printf("当前时间（本地）: %s\n", time_str);
    
    vox_time_t utc = vox_time_utc();
    vox_time_format_iso8601(utc, time_str, sizeof(time_str));
    printf("UTC时间: %s\n", time_str);
    
    vox_time_t gmt = vox_time_gmt();
    vox_time_format_gmt(gmt, time_str, sizeof(time_str));
    printf("GMT时间: %s\n", time_str);
    
    vox_time_t monotonic = vox_time_monotonic();
    printf("单调时间: %lld 微秒\n", (long long)monotonic);
    
    printf("\n=== 时间格式化 ===\n");
    vox_time_format(now, time_str, sizeof(time_str));
    printf("默认格式（本地）: %s\n", time_str);
    
    vox_time_format_iso8601(now, time_str, sizeof(time_str));
    printf("ISO 8601格式（UTC）: %s\n", time_str);
    
    vox_time_format_gmt(now, time_str, sizeof(time_str));
    printf("GMT格式: %s\n", time_str);
    
    char custom_str[128];
    vox_time_format_custom(now, "%Y年%m月%d日 %H:%M:%S", custom_str, sizeof(custom_str));
    printf("自定义格式: %s\n", custom_str);
    
    printf("\n=== 时间组件获取 ===\n");
    printf("年份: %d\n", vox_time_year(now));
    printf("月份: %d\n", vox_time_month(now));
    printf("日期: %d\n", vox_time_day(now));
    printf("小时: %d\n", vox_time_hour(now));
    printf("分钟: %d\n", vox_time_minute(now));
    printf("秒: %d\n", vox_time_second(now));
    printf("微秒: %d\n", vox_time_microsecond(now));
    printf("星期几: %d (0=周日)\n", vox_time_weekday(now));
    
    printf("\n=== 时间结构体操作 ===\n");
    vox_time_struct_t tm;
    if (vox_time_to_struct(now, &tm) == 0) {
        printf("本地时间结构体: %04d-%02d-%02d %02d:%02d:%02d.%06d (星期%d)\n",
               tm.year, tm.month, tm.day, tm.hour, tm.minute, tm.second, 
               tm.microsecond, tm.weekday);
        
        /* 修改时间 */
        tm.hour += 1;
        vox_time_t future = vox_time_from_struct(&tm);
        vox_time_format(future, time_str, sizeof(time_str));
        printf("1小时后: %s\n", time_str);
    }
    
    /* GMT时间结构体 */
    vox_time_struct_t gmt_tm;
    if (vox_time_to_struct_gmt(now, &gmt_tm) == 0) {
        printf("GMT时间结构体: %04d-%02d-%02d %02d:%02d:%02d.%06d (星期%d)\n",
               gmt_tm.year, gmt_tm.month, gmt_tm.day, gmt_tm.hour, gmt_tm.minute, 
               gmt_tm.second, gmt_tm.microsecond, gmt_tm.weekday);
    }
    
    /* UTC时间结构体 */
    vox_time_struct_t utc_tm;
    if (vox_time_to_struct_utc(now, &utc_tm) == 0) {
        printf("UTC时间结构体: %04d-%02d-%02d %02d:%02d:%02d.%06d (星期%d)\n",
               utc_tm.year, utc_tm.month, utc_tm.day, utc_tm.hour, utc_tm.minute, 
               utc_tm.second, utc_tm.microsecond, utc_tm.weekday);
    }
    
    printf("\n=== 时间运算 ===\n");
    vox_time_t t1 = now;
    vox_time_t t2 = vox_time_add(t1, VOX_TIME_HOUR(2));  /* 2小时后 */
    vox_time_format(t2, time_str, sizeof(time_str));
    printf("当前时间 + 2小时: %s\n", time_str);
    
    vox_time_t t3 = vox_time_sub(t1, VOX_TIME_DAY(1));  /* 1天前 */
    vox_time_format(t3, time_str, sizeof(time_str));
    printf("当前时间 - 1天: %s\n", time_str);
    
    int cmp = vox_time_compare(t1, t2);
    printf("时间比较 (t1 vs t2): %d\n", cmp);
    
    printf("\n=== 时间差计算 ===\n");
    int64_t diff_sec = vox_time_diff_sec(t2, t1);
    int64_t diff_ms = vox_time_diff_ms(t2, t1);
    int64_t diff_us = vox_time_diff_us(t2, t1);
    printf("时间差: %lld 秒, %lld 毫秒, %lld 微秒\n", 
           (long long)diff_sec, (long long)diff_ms, (long long)diff_us);
    
    printf("\n=== 时间戳转换 ===\n");
    int64_t sec = vox_time_to_sec(now);
    int64_t ms = vox_time_to_ms(now);
    printf("Unix时间戳: %lld 秒, %lld 毫秒\n", (long long)sec, (long long)ms);
    
    vox_time_t from_sec = vox_time_from_sec(sec);
    vox_time_t from_ms = vox_time_from_ms(ms);
    printf("从秒创建: %lld 微秒\n", (long long)from_sec);
    printf("从毫秒创建: %lld 微秒\n", (long long)from_ms);
    
    printf("\n=== 时间解析 ===\n");
    const char* time_str1 = "2024-01-15 12:30:45";
    vox_time_t parsed = vox_time_parse(time_str1);
    if (parsed > 0) {
        vox_time_format(parsed, time_str, sizeof(time_str));
        printf("解析 '%s': %s\n", time_str1, time_str);
    }
    
    const char* iso_str = "2024-01-15T12:30:45.123456Z";
    vox_time_t parsed_iso = vox_time_parse_iso8601(iso_str);
    if (parsed_iso > 0) {
        vox_time_format_iso8601(parsed_iso, time_str, sizeof(time_str));
        printf("解析 ISO 8601 '%s': %s\n", iso_str, time_str);
    }
    
    printf("\n=== 时间宏定义 ===\n");
    printf("1秒 = %lld 微秒\n", (long long)VOX_TIME_SEC(1));
    printf("1毫秒 = %lld 微秒\n", (long long)VOX_TIME_MS(1));
    printf("1分钟 = %lld 微秒\n", (long long)VOX_TIME_MIN(1));
    printf("1小时 = %lld 微秒\n", (long long)VOX_TIME_HOUR(1));
    printf("1天 = %lld 微秒\n", (long long)VOX_TIME_DAY(1));
    
    printf("\n=== 性能测试（时间差） ===\n");
    vox_time_t start = vox_time_monotonic();
    
    /* 模拟一些工作 */
    for (int i = 0; i < 1000000; i++) {
        volatile int x = i * 2;
        (void)x;
    }
    
    vox_time_t end = vox_time_monotonic();
    int64_t elapsed_us = vox_time_diff_us(end, start);
    int64_t elapsed_ms = vox_time_diff_ms(end, start);
    printf("循环1000000次耗时: %lld 微秒 (%lld 毫秒)\n", 
           (long long)elapsed_us, (long long)elapsed_ms);
    
    printf("\n=== 睡眠测试 ===\n");
    printf("睡眠 100 毫秒...\n");
    vox_time_t sleep_start = vox_time_monotonic();
    vox_time_sleep_ms(100);
    vox_time_t sleep_end = vox_time_monotonic();
    int64_t sleep_elapsed = vox_time_diff_ms(sleep_end, sleep_start);
    printf("实际睡眠时间: %lld 毫秒\n", (long long)sleep_elapsed);
    
    printf("\n所有测试完成！\n");
    return 0;
}
