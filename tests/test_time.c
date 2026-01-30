/* ============================================================
 * test_time.c - vox_time 模块测试
 * ============================================================ */

#include "test_runner.h"
#include "../vox_time.h"
#include "../vox_os.h"
#include <string.h>

/* 测试获取当前时间 */
static void test_time_now(vox_mpool_t* mpool) {
    VOX_UNUSED(mpool);
    vox_time_t t1 = vox_time_now();
    TEST_ASSERT_NE(t1, 0, "获取当前时间失败");
    
    vox_time_t t2 = vox_time_now();
    TEST_ASSERT_NE(t2, 0, "获取当前时间失败");
    TEST_ASSERT_EQ(vox_time_compare(t1, t2) <= 0, 1, "时间应该递增或相等");
}

/* 测试单调时间 */
static void test_time_monotonic(vox_mpool_t* mpool) {
    VOX_UNUSED(mpool);
    vox_time_t t1 = vox_time_monotonic();
    TEST_ASSERT_NE(t1, 0, "获取单调时间失败");
    
    vox_time_t t2 = vox_time_monotonic();
    TEST_ASSERT_NE(t2, 0, "获取单调时间失败");
    TEST_ASSERT_EQ(vox_time_compare(t1, t2) <= 0, 1, "单调时间应该递增或相等");
}

/* 测试时间格式化 */
static void test_time_format(vox_mpool_t* mpool) {
    VOX_UNUSED(mpool);
    vox_time_t t = vox_time_now();
    char buf[128];
    
    vox_time_format(t, buf, sizeof(buf));
    TEST_ASSERT_NE(strlen(buf), 0, "格式化时间失败");
    
    vox_time_format_iso8601(t, buf, sizeof(buf));
    TEST_ASSERT_NE(strlen(buf), 0, "ISO8601格式化失败");
}

/* 测试时间运算 */
static void test_time_arithmetic(vox_mpool_t* mpool) {
    VOX_UNUSED(mpool);
    vox_time_t t1 = vox_time_now();
    
    /* 测试加法 */
    vox_time_t t2 = vox_time_add(t1, VOX_TIME_SEC(10));
    TEST_ASSERT_EQ(vox_time_compare(t2, t1) > 0, 1, "加法后时间应该更大");
    
    /* 测试减法 */
    vox_time_t t3 = vox_time_sub(t1, VOX_TIME_SEC(5));
    TEST_ASSERT_EQ(vox_time_compare(t3, t1) < 0, 1, "减法后时间应该更小");
    
    /* 测试时间差 */
    int64_t diff = vox_time_diff_sec(t2, t1);
    TEST_ASSERT_EQ(diff >= 9 && diff <= 11, 1, "时间差计算不正确");
}

/* 测试时间戳转换 */
static void test_time_timestamp(vox_mpool_t* mpool) {
    VOX_UNUSED(mpool);
    int64_t sec = 1000000000;  /* 2001-09-09 01:46:40 UTC */
    vox_time_t t = vox_time_from_sec(sec);
    
    int64_t sec2 = vox_time_to_sec(t);
    TEST_ASSERT_EQ(sec, sec2, "时间戳转换失败");
    
    int64_t ms = 1000000000000;
    vox_time_t t2 = vox_time_from_ms(ms);
    int64_t ms2 = vox_time_to_ms(t2);
    TEST_ASSERT_EQ(ms, ms2, "毫秒时间戳转换失败");
}

/* 测试时间组件获取 */
static void test_time_components(vox_mpool_t* mpool) {
    VOX_UNUSED(mpool);
    vox_time_t t = vox_time_now();
    
    int year = vox_time_year(t);
    TEST_ASSERT_EQ(year >= 2000 && year <= 2100, 1, "年份获取失败");
    
    int month = vox_time_month(t);
    TEST_ASSERT_EQ(month >= 1 && month <= 12, 1, "月份获取失败");
    
    int day = vox_time_day(t);
    TEST_ASSERT_EQ(day >= 1 && day <= 31, 1, "日期获取失败");
    
    int hour = vox_time_hour(t);
    TEST_ASSERT_EQ(hour >= 0 && hour <= 23, 1, "小时获取失败");
    
    int minute = vox_time_minute(t);
    TEST_ASSERT_EQ(minute >= 0 && minute <= 59, 1, "分钟获取失败");
    
    int second = vox_time_second(t);
    TEST_ASSERT_EQ(second >= 0 && second <= 59, 1, "秒获取失败");
}

/* 测试时间结构体 */
static void test_time_struct(vox_mpool_t* mpool) {
    VOX_UNUSED(mpool);
    vox_time_t t = vox_time_now();
    vox_time_struct_t tm;
    
    TEST_ASSERT_EQ(vox_time_to_struct(t, &tm), 0, "转换为结构体失败");
    TEST_ASSERT_EQ(tm.year >= 2000 && tm.year <= 2100, 1, "结构体年份不正确");
    TEST_ASSERT_EQ(tm.month >= 1 && tm.month <= 12, 1, "结构体月份不正确");
    
    vox_time_t t2 = vox_time_from_struct(&tm);
    TEST_ASSERT_NE(t2, -1, "从结构体创建时间失败");
}

/* 测试套件 */
test_case_t test_time_cases[] = {
    {"now", test_time_now},
    {"monotonic", test_time_monotonic},
    {"format", test_time_format},
    {"arithmetic", test_time_arithmetic},
    {"timestamp", test_time_timestamp},
    {"components", test_time_components},
    {"struct", test_time_struct},
};

test_suite_t test_time_suite = {
    "vox_time",
    test_time_cases,
    sizeof(test_time_cases) / sizeof(test_time_cases[0])
};
