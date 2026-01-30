/*
 * vox_os.h - 跨平台操作系统定义
 * 提供平台检测、类型定义和兼容性宏
 */

#ifndef VOX_OS_H
#define VOX_OS_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ===== 平台检测 ===== */

/* Windows 平台 */
#if defined(_WIN32) || defined(_WIN64) || defined(__WIN32__) || defined(__TOS_WIN__) || defined(__WINDOWS__)
    #ifndef VOX_OS_WINDOWS
        #define VOX_OS_WINDOWS 1
    #endif
    #ifndef VOX_OS_WIN
        #define VOX_OS_WIN 1
    #endif
#endif

/* Linux 平台 */
#if defined(__linux__) || defined(__linux) || defined(linux)
    #ifndef VOX_OS_LINUX
        #define VOX_OS_LINUX 1
    #endif
    #ifndef VOX_OS_UNIX
        #define VOX_OS_UNIX 1
    #endif
#endif

/* macOS / Darwin 平台 */
#if defined(__APPLE__) && defined(__MACH__)
    #ifndef VOX_OS_MACOS
        #define VOX_OS_MACOS 1
    #endif
    #ifndef VOX_OS_DARWIN
        #define VOX_OS_DARWIN 1
    #endif
    #ifndef VOX_OS_UNIX
        #define VOX_OS_UNIX 1
    #endif
#endif

/* FreeBSD 平台 */
#if defined(__FreeBSD__) || defined(__FreeBSD)
    #ifndef VOX_OS_FREEBSD
        #define VOX_OS_FREEBSD 1
    #endif
    #ifndef VOX_OS_UNIX
        #define VOX_OS_UNIX 1
    #endif
#endif

/* NetBSD 平台 */
#if defined(__NetBSD__) || defined(__NetBSD)
    #ifndef VOX_OS_NETBSD
        #define VOX_OS_NETBSD 1
    #endif
    #ifndef VOX_OS_UNIX
        #define VOX_OS_UNIX 1
    #endif
#endif

/* OpenBSD 平台 */
#if defined(__OpenBSD__) || defined(__OPENBSD)
    #ifndef VOX_OS_OPENBSD
        #define VOX_OS_OPENBSD 1
    #endif
    #ifndef VOX_OS_UNIX
        #define VOX_OS_UNIX 1
    #endif
#endif

/* Android 平台 */
#if defined(__ANDROID__)
    #ifndef VOX_OS_ANDROID
        #define VOX_OS_ANDROID 1
    #endif
    #ifndef VOX_OS_LINUX
        #define VOX_OS_LINUX 1
    #endif
    #ifndef VOX_OS_UNIX
        #define VOX_OS_UNIX 1
    #endif
#endif

/* iOS 平台 */
#if defined(__APPLE__) && defined(__MACH__) && defined(__ENVIRONMENT_IPHONE_OS_VERSION_MIN_REQUIRED__)
    #ifndef VOX_OS_IOS
        #define VOX_OS_IOS 1
    #endif
#endif

/* ===== 架构检测 ===== */

/* x86 / x86_64 */
#if defined(__x86_64__) || defined(_M_X64) || defined(__amd64__) || defined(__amd64)
    #ifndef VOX_ARCH_X86_64
        #define VOX_ARCH_X86_64 1
    #endif
#elif defined(__i386__) || defined(_M_IX86) || defined(__i386)
    #ifndef VOX_ARCH_X86
        #define VOX_ARCH_X86 1
    #endif
#endif

/* ARM */
#if defined(__arm__) || defined(_M_ARM) || defined(__ARM_ARCH)
    #ifndef VOX_ARCH_ARM
        #define VOX_ARCH_ARM 1
    #endif
#endif

/* ARM64 / AArch64 */
#if defined(__aarch64__) || defined(_M_ARM64) || defined(__arm64__)
    #ifndef VOX_ARCH_ARM64
        #define VOX_ARCH_ARM64 1
    #endif
#endif

/* 64位平台 */
#if defined(__LP64__) || defined(_WIN64) || defined(__x86_64__) || defined(__aarch64__)
    #ifndef VOX_ARCH_64BIT
        #define VOX_ARCH_64BIT 1
    #endif
#else
    #ifndef VOX_ARCH_32BIT
        #define VOX_ARCH_32BIT 1
    #endif
#endif

/* ===== 编译器检测 ===== */

/* MSVC */
#if defined(_MSC_VER)
    #ifndef VOX_COMPILER_MSVC
        #define VOX_COMPILER_MSVC 1
    #endif
    #define VOX_COMPILER_MSVC_VERSION _MSC_VER
#endif

/* GCC */
#if defined(__GNUC__) && !defined(__clang__)
    #ifndef VOX_COMPILER_GCC
        #define VOX_COMPILER_GCC 1
    #endif
    #define VOX_COMPILER_GCC_VERSION (__GNUC__ * 100 + __GNUC_MINOR__)
#endif

/* Clang */
#if defined(__clang__)
    #ifndef VOX_COMPILER_CLANG
        #define VOX_COMPILER_CLANG 1
    #endif
    #define VOX_COMPILER_CLANG_VERSION (__clang_major__ * 100 + __clang_minor__)
#endif

/* ===== 平台特性检测 ===== */

/* 字节序 */
#if defined(__BYTE_ORDER__)
    #if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
        #ifndef VOX_LITTLE_ENDIAN
            #define VOX_LITTLE_ENDIAN 1
        #endif
    #elif __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
        #ifndef VOX_BIG_ENDIAN
            #define VOX_BIG_ENDIAN 1
        #endif
    #endif
#elif defined(_WIN32) || defined(__LITTLE_ENDIAN__)
    #ifndef VOX_LITTLE_ENDIAN
        #define VOX_LITTLE_ENDIAN 1
    #endif
#elif defined(__BIG_ENDIAN__)
    #ifndef VOX_BIG_ENDIAN
        #define VOX_BIG_ENDIAN 1
    #endif
#endif

/* ===== 调试和断言 ===== */

#ifdef VOX_ENABLE_ASSERT
    #include <assert.h>
    #define VOX_ASSERT(x) assert(x)
#else
    #define VOX_ASSERT(x) ((void)0)
#endif

/* ===== 未使用参数标记 ===== */

/**
 * 标记未使用的参数，避免编译器警告
 * 用法：VOX_UNUSED(param)
 */
#define VOX_UNUSED(x) ((void)(x))

/**
 * 标记未使用的函数，避免编译器警告
 * 用法：VOX_UNUSED_FUNC 放在函数定义前
 */
#if defined(__GNUC__) || defined(__clang__)
    #define VOX_UNUSED_FUNC __attribute__((unused))
#elif defined(_MSC_VER)
    #define VOX_UNUSED_FUNC __pragma(warning(suppress:4505))
#else
    #define VOX_UNUSED_FUNC
#endif

/* ssize_t 定义 */
#ifndef _SSIZE_T_DEFINED
    #ifdef VOX_OS_WINDOWS
        /* Windows 可能没有 ssize_t，需要自己定义 */
        #ifdef _WIN64
            typedef int64_t ssize_t;
        #else
            typedef int32_t ssize_t;
        #endif
        #define _SSIZE_T_DEFINED

        #ifndef strncasecmp
            #define strncasecmp _strnicmp
        #endif

        #ifndef strcasecmp
            #define strcasecmp _stricmp
        #endif
        /* 注意：snprintf 在较新的 Windows SDK 中已经可用，不需要宏定义 */
        /* 如果确实需要，可以使用条件编译，但要注意与标准库的冲突 */
        #if defined(_MSC_VER) && _MSC_VER < 1900
            /* 只有旧版本的 MSVC 才需要这个宏定义 */
            #ifndef snprintf    
                #define snprintf _snprintf
            #endif
        #endif
    #else
        /* Unix/Linux 系统，定义 POSIX 特性测试宏以确保 ssize_t 可用 */
        #ifndef _POSIX_C_SOURCE
            #define _POSIX_C_SOURCE 200809L
        #endif
        /* 包含系统头文件以获取 ssize_t */
        #include <sys/types.h>
        /* 如果 <sys/types.h> 没有定义 ssize_t，我们自己定义 */
        #ifndef _SSIZE_T
            #ifndef ssize_t
                typedef long ssize_t;
            #endif
        #endif
        #define _SSIZE_T_DEFINED
    #endif
#endif

/* 对齐宏：获取类型的对齐要求（C99 兼容，使用编译器扩展） */
#if defined(__GNUC__) || defined(__clang__)
    /* GCC/Clang 扩展 */
    #define VOX_ALIGNOF(type) __alignof__(type)
#elif defined(_MSC_VER)
    /* MSVC 扩展 */
    #define VOX_ALIGNOF(type) __alignof(type)
#else
    /* 回退：假设对齐到最大基本类型的大小（通常是8字节） */
    #define VOX_ALIGNOF(type) (sizeof(void*) > sizeof(double) ? sizeof(void*) : sizeof(double))
#endif

/* 向上对齐到对齐边界 */
#define VOX_ALIGN_SIZE(size, align) (((size) + (align) - 1) & ~((align) - 1))

/* ===== 平台特定头文件 ===== */

#ifdef VOX_OS_WINDOWS
    /* Windows 平台头文件 */
    /* 重要：必须在包含 windows.h 之前定义 WIN32_LEAN_AND_MEAN，
     * 以避免 windows.h 自动包含 winsock.h，导致与 winsock2.h 冲突 */
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    
    /* 重要：winsock2.h 必须在 windows.h 之前包含，以避免类型重定义 */
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #include <windows.h>
    #include <io.h>
    
    /* Windows Socket 库链接（在 .c 文件中使用 pragma comment） */
    /* 注意：pragma comment 不能在头文件中使用，需要在 .c 文件中单独添加 */
#else
/* Unix/Linux 平台头文件 */
    #include <strings.h>
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <netinet/tcp.h>
    #include <arpa/inet.h>
    #include <netdb.h>
    #include <unistd.h>
    #include <errno.h>
    #include <fcntl.h>
    #include <sys/time.h>
#endif

/* ===== 平台特定类型定义 ===== */

#ifdef VOX_OS_WINDOWS
    /* Windows Socket 类型定义 */
    #ifndef INVALID_SOCKET
        #define INVALID_SOCKET (SOCKET)(~0)
    #endif
    #ifndef SOCKET_ERROR
        #define SOCKET_ERROR (-1)
    #endif
#else
    /* Unix Socket 类型定义 */
    #ifndef INVALID_SOCKET
        #define INVALID_SOCKET -1
    #endif
    #ifndef SOCKET_ERROR
        #define SOCKET_ERROR -1
    #endif
    typedef int SOCKET;
#endif

/* ===== 平台特定宏定义 ===== */

#ifdef VOX_OS_WINDOWS
    /* VOX_CONTAINING_RECORD 宏：从结构成员指针获取包含该成员的结构指针 */
    #ifndef VOX_CONTAINING_RECORD
        #define VOX_CONTAINING_RECORD(address, type, field) \
            ((type *)((char *)(address) - (size_t)(&((type *)0)->field)))
    #endif
#endif

#ifdef __cplusplus
}
#endif

#endif /* VOX_OS_H */
