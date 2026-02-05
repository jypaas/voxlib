cmake_minimum_required(VERSION 3.10)

# 允许子目录向主目标添加源与链接库
cmake_policy(SET CMP0076 NEW)   # target_sources 接受来自其他目录的相对路径
cmake_policy(SET CMP0079 NEW)   # target_link_libraries 可链接由其他目录创建的目标

# Enable ASM language for Unix platforms (x86_64/ARM64 context switching)
if(NOT WIN32)
    project(vox VERSION 1.0.0 LANGUAGES C ASM)
else()
    project(vox VERSION 1.0.0 LANGUAGES C)
endif()

# ===== C标准设置 =====
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

# ===== 输出目录设置 =====
# 将所有编译输出文件统一输出到 bin 目录
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${PROJECT_SOURCE_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${PROJECT_SOURCE_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${PROJECT_SOURCE_DIR}/bin)

# ===== 编译选项 =====
if(MSVC)
    add_compile_options(
            /W4
            /source-charset:utf-8
            /execution-charset:utf-8
        )
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
    
    # Debug 配置：添加调试信息和禁用优化
    # /Zi = 生成完整的调试信息
    # /Od = 禁用优化（便于调试）
    # /RTC1 = 运行时检查（检测未初始化的变量等）
    set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} /Zi /Od /RTC1")
    
    # 编译器优化选项（MSVC）
    # /O2 = 最大化速度优化（相当于 -O2）
    # /Ob2 = 内联函数扩展（任何合适的函数）
    # /Oi = 生成内联函数
    # /Ot = 优化速度而非大小
    # 注意：使用生成器表达式支持多配置生成器（Visual Studio）
    # 在 Release 和 RelWithDebInfo 配置中启用优化
    set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} /O2 /Ob2 /Oi /Ot")
    set(CMAKE_C_FLAGS_RELWITHDEBINFO "${CMAKE_C_FLAGS_RELWITHDEBINFO} /O2 /Ob2 /Oi /Ot /Zi")
else()
    add_compile_options(-Wall -Wextra -pedantic)
    
    # Debug 配置：添加调试信息和禁用优化
    # -g = 生成调试信息
    # -g3 = 生成最详细的调试信息（包括宏定义）
    # -O0 = 禁用优化（便于调试）
    # -fno-omit-frame-pointer = 保留帧指针（便于调试）
    if(CMAKE_BUILD_TYPE STREQUAL "Debug" OR CMAKE_BUILD_TYPE STREQUAL "")
        set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} -g3 -O0 -fno-omit-frame-pointer")
    endif()
    
    # 编译器优化选项（GCC/Clang）
    # -O3 = 最高级别优化
    # -march=native = 使用当前 CPU 的特定指令集（SSE、AVX 等）
    # -flto = 链接时优化（Link Time Optimization）
    # -ffast-math = 快速数学运算（可能影响精度，谨慎使用）
    # -funroll-loops = 循环展开
    # -finline-functions = 内联函数
    # 对于单配置生成器（Unix Makefiles），使用 CMAKE_BUILD_TYPE
    if(CMAKE_BUILD_TYPE STREQUAL "Release" OR CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
        add_compile_options(-O3 -march=native -funroll-loops -finline-functions)
        # RelWithDebInfo 也包含调试信息
        if(CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
            set(CMAKE_C_FLAGS_RELWITHDEBINFO "${CMAKE_C_FLAGS_RELWITHDEBINFO} -g")
        endif()
        # 可选：链接时优化（需要链接器也支持）
        # add_compile_options(-flto)
        # 可选：快速数学（可能影响浮点数精度）
        # add_compile_options(-ffast-math)
    endif()
endif()

# ===== 配置选项 =====
option(VOX_BUILD_TESTS "Build unit tests" OFF)
option(VOX_BUILD_EXAMPLES "Build examples" OFF)
option(VOX_BUILD_SHARED "Build shared library" OFF)
option(VOX_USE_IOURING "Use io_uring on Linux (requires liburing)" ON)
option(VOX_ENABLE_LOG "Enable logging" ON)
option(VOX_ENABLE_ASSERT "Enable assertions" ON)
option(VOX_USE_OPENSSL "Use OpenSSL for TLS support" ON)
option(VOX_USE_WOLFSSL "Use WolfSSL for TLS support" OFF)
option(VOX_USE_MBEDTLS "Use mbedTLS for TLS support" OFF)
option(VOX_USE_ZLIB "Use zlib for gzip compression" ON)

# ===== 模块开关（按需编译各模块）=====
option(VOX_USE_COROUTINE "Build coroutine module" ON)
option(VOX_USE_SSL "Build SSL/TLS module" ON)
option(VOX_USE_HTTP "Build HTTP module" ON)
option(VOX_USE_WEBSOCKET "Build WebSocket module" ON)
option(VOX_USE_REDIS "Build Redis module" ON)
option(VOX_USE_DB "Build DB module" ON)

# ===== DB 支持（可选，仅当 VOX_USE_DB=ON 时有效）=====
option(VOX_USE_SQLITE3 "Use SQLite3 for DB support" ON)
option(VOX_USE_DUCKDB "Use DuckDB for DB support" OFF)
option(VOX_USE_PGSQL "Use PostgreSQL (libpq) for DB support" OFF)
option(VOX_USE_MYSQL "Use MySQL (libmysqlclient) for DB support" OFF)

# ===== 包含目录 =====
#include_directories(${PROJECT_SOURCE_DIR}/include)
#include_directories(${PROJECT_SOURCE_DIR}/coroutine)

# ===== 核心源文件（始终编译；各模块由子目录 CMakeLists 按开关添加）=====
set(VOX_SOURCES
    vox.c
    vox_mpool.c
    vox_htable.c
    vox_rbtree.c
    vox_mheap.c
    vox_vector.c
    vox_string.c
    vox_queue.c
    vox_file.c
    vox_time.c
    vox_log.c
    vox_thread.c
    vox_mutex.c
    vox_atomic.c
    vox_tpool.c
    vox_socket.c
    vox_crypto.c
    vox_scanner.c
    vox_regex.c
    vox_json.c
    vox_xml.c
    vox_toml.c
    vox_select.c
    vox_process.c
    vox_daemon.c
    vox_loop.c
    vox_timer.c
    vox_backend.c
    vox_handle.c
    vox_tcp.c
    vox_udp.c
    vox_dns.c
    vox_fs.c
)

# ===== 平台特定的源文件 =====
if(WIN32)
    list(APPEND VOX_SOURCES vox_iocp.c)
elseif(APPLE)
    list(APPEND VOX_SOURCES vox_kqueue.c)
elseif(UNIX)
    list(APPEND VOX_SOURCES vox_epoll.c)
    if(VOX_USE_IOURING)
        list(APPEND VOX_SOURCES vox_uring.c)
    endif()
endif()

# ===== 构建库 =====
if(VOX_BUILD_SHARED)
    add_library(vox SHARED ${VOX_SOURCES})
    target_compile_definitions(vox PRIVATE VOX_BUILD_SHARED)
    set_target_properties(vox PROPERTIES
        VERSION ${PROJECT_VERSION}
        SOVERSION ${PROJECT_VERSION_MAJOR})
else()
    add_library(vox STATIC ${VOX_SOURCES})
endif()

# 供子目录 CMakeLists 使用的目标名
set(VOX_LIB_TARGET vox)

# ===== 按模块开关添加子目录（各模块在自己的 CMakeLists.txt 中向 vox 添加源）=====
if(VOX_USE_COROUTINE)
    add_subdirectory(coroutine)
endif()
if(VOX_USE_SSL)
    add_subdirectory(ssl)
endif()
if(VOX_USE_HTTP)
    add_subdirectory(http)
endif()
if(VOX_USE_WEBSOCKET)
    add_subdirectory(websocket)
endif()
if(VOX_USE_REDIS)
    add_subdirectory(redis)
endif()
if(VOX_USE_DB)
    add_subdirectory(db)
endif()

# ===== 编译器优化（针对库） =====
if(MSVC)
    # Debug 配置：添加调试信息
    target_compile_options(vox PRIVATE 
        $<$<CONFIG:Debug>:/Zi /Od /RTC1>)
    
    # MSVC 全程序优化（Whole Program Optimization）
    # 使用生成器表达式支持多配置生成器
    # /GL 启用全程序优化，需要配合链接器的 /LTCG
    target_compile_options(vox PRIVATE 
        $<$<CONFIG:Release>:/GL>
        $<$<CONFIG:RelWithDebInfo>:/GL /Zi>)
    # 链接时优化（Link Time Code Generation）
    set_target_properties(vox PROPERTIES
        INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE
        INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO TRUE)
    # 确保链接器也使用 LTCG（使用生成器表达式）
    set_target_properties(vox PROPERTIES
        LINK_FLAGS_RELEASE "/LTCG"
        LINK_FLAGS_RELWITHDEBINFO "/LTCG /DEBUG")
else()
    # Debug 配置：添加调试信息
    if(CMAKE_BUILD_TYPE STREQUAL "Debug" OR CMAKE_BUILD_TYPE STREQUAL "")
        target_compile_options(vox PRIVATE -g3 -O0 -fno-omit-frame-pointer)
    endif()
    
    # GCC/Clang 链接时优化（Link Time Optimization）
    if(CMAKE_BUILD_TYPE STREQUAL "Release" OR CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
        set_target_properties(vox PROPERTIES
            INTERPROCEDURAL_OPTIMIZATION TRUE)
        # RelWithDebInfo 也包含调试信息
        if(CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
            target_compile_options(vox PRIVATE -g)
        endif()
    endif()
endif()

# ===== 平台定义 =====
if(WIN32)
    set(VOX_PLATFORM_DEFINES VOX_OS_WINDOWS)
elseif(APPLE)
    set(VOX_PLATFORM_DEFINES VOX_OS_MACOS)
elseif(UNIX)
    set(VOX_PLATFORM_DEFINES VOX_OS_UNIX)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        list(APPEND VOX_PLATFORM_DEFINES VOX_OS_LINUX)
    endif()
endif()

# ===== 设置编译定义 =====
target_compile_definitions(vox PRIVATE
    ${VOX_PLATFORM_DEFINES}
    VOX_ENABLE_LOG=$<BOOL:${VOX_ENABLE_LOG}>
    VOX_ENABLE_ASSERT=$<BOOL:${VOX_ENABLE_ASSERT}>
)

# ===== io_uring 检测 =====
if(VOX_USE_IOURING AND UNIX AND NOT APPLE)
    find_library(URING_LIB uring)
    if(URING_LIB)
        message(STATUS "Found liburing: ${URING_LIB}")
        target_link_libraries(vox PRIVATE ${URING_LIB})
        target_compile_definitions(vox PRIVATE VOX_USE_IOURING=1)
    else()
        message(WARNING "liburing not found, io_uring backend will be disabled")
        set(VOX_USE_IOURING OFF)
    endif()
endif()

# ===== SSL 库支持（仅当启用 SSL 模块时查找/链接）=====
if(VOX_USE_SSL AND VOX_USE_OPENSSL)
    # 在 Windows 上，优先使用 vcpkg 查找 OpenSSL
    if(WIN32)
        # 检查是否使用了 vcpkg 工具链
        if(CMAKE_TOOLCHAIN_FILE AND CMAKE_TOOLCHAIN_FILE MATCHES "vcpkg")
            message(STATUS "Using vcpkg toolchain: ${CMAKE_TOOLCHAIN_FILE}")
            # vcpkg 会自动设置 VCPKG_TARGET_TRIPLET
            if(DEFINED VCPKG_TARGET_TRIPLET)
                message(STATUS "vcpkg triplet: ${VCPKG_TARGET_TRIPLET}")
            endif()
        else()
            # 提示用户可以使用 vcpkg
            message(STATUS "Tip: To use vcpkg for OpenSSL on Windows:")
            message(STATUS "  1. Install vcpkg: git clone https://github.com/Microsoft/vcpkg.git")
            message(STATUS "  2. Install OpenSSL: vcpkg install openssl:x64-windows")
            message(STATUS "  3. Configure CMake: cmake -DCMAKE_TOOLCHAIN_FILE=<vcpkg-root>/scripts/buildsystems/vcpkg.cmake ..")
        endif()
        
        # 如果设置了 VCPKG_ROOT 但没有设置工具链，尝试手动设置路径
        if(DEFINED ENV{VCPKG_ROOT} AND NOT CMAKE_TOOLCHAIN_FILE)
            # 尝试检测 triplet（默认为 x64-windows）
            if(NOT DEFINED VCPKG_TARGET_TRIPLET)
                if(CMAKE_SIZEOF_VOID_P EQUAL 8)
                    set(VCPKG_TARGET_TRIPLET "x64-windows")
                else()
                    set(VCPKG_TARGET_TRIPLET "x86-windows")
                endif()
            endif()
            set(CMAKE_PREFIX_PATH "$ENV{VCPKG_ROOT}/installed/${VCPKG_TARGET_TRIPLET}" ${CMAKE_PREFIX_PATH})
            message(STATUS "Using VCPKG_ROOT: $ENV{VCPKG_ROOT} (triplet: ${VCPKG_TARGET_TRIPLET})")
        endif()
    endif()
    
    # 查找 OpenSSL（vcpkg 会提供相应的配置文件）
    # 使用 QUIET 模式，如果找不到再给出详细错误信息
    find_package(OpenSSL QUIET)
    
    if(OpenSSL_FOUND)
        message(STATUS "Found OpenSSL:")
        message(STATUS "  Version: ${OPENSSL_VERSION}")
        if(OPENSSL_INCLUDE_DIR)
            message(STATUS "  Include dir: ${OPENSSL_INCLUDE_DIR}")
        endif()
        if(WIN32 AND OPENSSL_LIBRARIES)
            message(STATUS "  Libraries: ${OPENSSL_LIBRARIES}")
        endif()
        if(OPENSSL_ROOT_DIR)
            message(STATUS "  Root dir: ${OPENSSL_ROOT_DIR}")
        endif()
        
        # 链接 OpenSSL 库
        # 优先使用现代 CMake 目标（OpenSSL 1.1.0+，vcpkg 提供）
        if(TARGET OpenSSL::SSL AND TARGET OpenSSL::Crypto)
            target_link_libraries(vox PRIVATE OpenSSL::SSL OpenSSL::Crypto)
            message(STATUS "  Using modern CMake targets (OpenSSL::SSL, OpenSSL::Crypto)")
        elseif(TARGET OpenSSL::OpenSSL)
            # OpenSSL 3.0+ 可能使用单个目标
            target_link_libraries(vox PRIVATE OpenSSL::OpenSSL)
            message(STATUS "  Using OpenSSL::OpenSSL target")
        else()
            # 回退到传统方式
            if(OPENSSL_INCLUDE_DIR)
                target_include_directories(vox PRIVATE ${OPENSSL_INCLUDE_DIR})
            endif()
            if(OPENSSL_LIBRARIES)
                target_link_libraries(vox PRIVATE ${OPENSSL_LIBRARIES})
            else()
                # 尝试手动查找库文件
                if(WIN32 AND OPENSSL_ROOT_DIR)
                    # Windows 上 vcpkg 安装的 OpenSSL 库路径
                    find_library(OPENSSL_SSL_LIB
                        NAMES ssl libssl
                        PATHS "${OPENSSL_ROOT_DIR}/lib"
                        NO_DEFAULT_PATH
                    )
                    find_library(OPENSSL_CRYPTO_LIB
                        NAMES crypto libcrypto
                        PATHS "${OPENSSL_ROOT_DIR}/lib"
                        NO_DEFAULT_PATH
                    )
                    if(OPENSSL_SSL_LIB AND OPENSSL_CRYPTO_LIB)
                        target_link_libraries(vox PRIVATE ${OPENSSL_SSL_LIB} ${OPENSSL_CRYPTO_LIB})
                        message(STATUS "  Found libraries: ${OPENSSL_SSL_LIB}, ${OPENSSL_CRYPTO_LIB}")
                    else()
                        message(FATAL_ERROR "Could not find OpenSSL libraries in ${OPENSSL_ROOT_DIR}/lib")
                    endif()
                else()
                    message(FATAL_ERROR "Could not find OpenSSL libraries")
                endif()
            endif()
            message(STATUS "  Using traditional linking method")
        endif()
        
        target_compile_definitions(vox PRIVATE VOX_USE_OPENSSL=1)
    else()
        # 提供详细的安装说明
        if(WIN32)
            message(FATAL_ERROR "\n"
                "OpenSSL not found!\n\n"
                "To install OpenSSL via vcpkg:\n"
                "  1. Install vcpkg:\n"
                "     git clone https://github.com/Microsoft/vcpkg.git\n"
                "     cd vcpkg\n"
                "     .\\bootstrap-vcpkg.bat\n\n"
                "  2. Install OpenSSL:\n"
                "     .\\vcpkg install openssl:x64-windows\n\n"
                "  3. Configure CMake with vcpkg toolchain:\n"
                "     cmake -DCMAKE_TOOLCHAIN_FILE=<vcpkg-root>/scripts/buildsystems/vcpkg.cmake -DVOX_USE_OPENSSL=ON ..\n\n"
                "Or set VCPKG_ROOT environment variable:\n"
                "     set VCPKG_ROOT=<vcpkg-root>\n"
                "     cmake -DVOX_USE_OPENSSL=ON ..\n"
            )
        else()
            message(FATAL_ERROR "OpenSSL not found. Please install OpenSSL development packages.")
        endif()
    endif()
elseif(VOX_USE_WOLFSSL)
    # find_package(wolfSSL)
    # if(wolfSSL_FOUND)
    #     target_link_libraries(vox PRIVATE wolfssl)
    #     target_compile_definitions(vox PRIVATE VOX_USE_WOLFSSL=1)
    # endif()
    message(WARNING "WolfSSL support not yet implemented")
    set(VOX_USE_WOLFSSL OFF)
elseif(VOX_USE_MBEDTLS)
    # find_package(mbedTLS)
    # if(mbedTLS_FOUND)
    #     target_link_libraries(vox PRIVATE mbedtls mbedx509 mbedcrypto)
    #     target_compile_definitions(vox PRIVATE VOX_USE_MBEDTLS=1)
    # endif()
    message(WARNING "mbedTLS support not yet implemented")
    set(VOX_USE_MBEDTLS OFF)
endif()

# ===== zlib 支持（用于 HTTP gzip，仅当启用 HTTP 模块时可选）=====
if(VOX_USE_HTTP AND VOX_USE_ZLIB)
    find_package(ZLIB QUIET)
    if(ZLIB_FOUND)
        message(STATUS "Found zlib:")
        message(STATUS "  Version: ${ZLIB_VERSION}")
        if(ZLIB_INCLUDE_DIRS)
            message(STATUS "  Include dir: ${ZLIB_INCLUDE_DIRS}")
        endif()
        if(TARGET ZLIB::ZLIB)
            target_link_libraries(vox PRIVATE ZLIB::ZLIB)
            message(STATUS "  Using modern CMake target (ZLIB::ZLIB)")
        else()
            if(ZLIB_INCLUDE_DIRS)
                target_include_directories(vox PRIVATE ${ZLIB_INCLUDE_DIRS})
            endif()
            if(ZLIB_LIBRARIES)
                target_link_libraries(vox PRIVATE ${ZLIB_LIBRARIES})
            else()
                find_library(ZLIB_LIBRARY NAMES z zlib)
                if(ZLIB_LIBRARY)
                    target_link_libraries(vox PRIVATE ${ZLIB_LIBRARY})
                else()
                    message(FATAL_ERROR "zlib library not found. Disable VOX_USE_ZLIB or install zlib.")
                endif()
            endif()
        endif()
        target_compile_definitions(vox PRIVATE VOX_USE_ZLIB=1)
    else()
        # 回退手动查找
        find_path(ZLIB_INCLUDE_DIR zlib.h)
        find_library(ZLIB_LIBRARY NAMES z zlib)
        if(ZLIB_INCLUDE_DIR AND ZLIB_LIBRARY)
            target_include_directories(vox PRIVATE ${ZLIB_INCLUDE_DIR})
            target_link_libraries(vox PRIVATE ${ZLIB_LIBRARY})
            target_compile_definitions(vox PRIVATE VOX_USE_ZLIB=1)
            message(STATUS "Found zlib: ${ZLIB_LIBRARY}")
        else()
            message(FATAL_ERROR "zlib not found. Disable VOX_USE_ZLIB or install zlib development packages.")
        endif()
    endif()
endif()

# ===== 链接库 =====
if(WIN32)
    target_link_libraries(vox PRIVATE ws2_32 mswsock)
elseif(UNIX)
    target_link_libraries(vox PRIVATE pthread m resolv)
endif()

# ===== 安装规则 =====
install(TARGETS vox
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
    RUNTIME DESTINATION bin)

install(DIRECTORY include/vox
    DESTINATION include
    FILES_MATCHING PATTERN "*.h")

# ===== 测试 =====
if(VOX_BUILD_TESTS)
    enable_testing()
    
    # 测试源文件
    set(TEST_SOURCES
        tests/test_runner.c
        tests/test_mpool.c
        tests/test_log.c
        tests/test_vector.c
        tests/test_string.c
        tests/test_queue.c
        tests/test_htable.c
        tests/test_time.c
        tests/test_atomic.c
        tests/test_rbtree.c
        tests/test_mheap.c
        tests/test_crypto.c
        tests/test_scanner.c
        tests/test_file.c
        tests/test_json.c
        tests/test_xml.c
        tests/test_toml.c
        tests/test_thread.c
        tests/test_mutex.c
        tests/test_socket.c
        tests/test_process.c
        tests/test_tpool.c
        tests/test_regex.c
    )
    if(VOX_USE_HTTP)
        list(APPEND TEST_SOURCES
            tests/test_http_router.c
            tests/test_http_middleware.c
            tests/test_http_ws.c
        )
    endif()

    # DB 测试：按启用驱动追加
    if(VOX_USE_SQLITE3)
        list(APPEND TEST_SOURCES tests/test_db_sqlite3.c)
    endif()
    if(VOX_USE_DUCKDB)
        list(APPEND TEST_SOURCES tests/test_db_duckdb.c)
    endif()
    
    # 创建测试可执行文件
    add_executable(vox_test tests/test_main.c ${TEST_SOURCES})
    target_link_libraries(vox_test PRIVATE vox)

    # 让 test_main.c 可见 DB 相关宏（因为 vox 的编译定义是 PRIVATE）
    if(VOX_USE_SQLITE3)
        target_compile_definitions(vox_test PRIVATE VOX_USE_SQLITE3=1)
    endif()
    if(VOX_USE_DUCKDB)
        target_compile_definitions(vox_test PRIVATE VOX_USE_DUCKDB=1)
    endif()
    if(VOX_USE_PGSQL)
        target_compile_definitions(vox_test PRIVATE VOX_USE_PGSQL=1)
    endif()
    if(VOX_USE_MYSQL)
        target_compile_definitions(vox_test PRIVATE VOX_USE_MYSQL=1)
    endif()
    
    # Debug 配置：为测试可执行文件添加调试信息
    if(MSVC)
        target_compile_options(vox_test PRIVATE 
            $<$<CONFIG:Debug>:/Zi /Od /RTC1>)
    else()
        if(CMAKE_BUILD_TYPE STREQUAL "Debug" OR CMAKE_BUILD_TYPE STREQUAL "")
            target_compile_options(vox_test PRIVATE -g3 -O0 -fno-omit-frame-pointer)
        endif()
    endif()
    
    # 添加测试
    add_test(NAME vox_test COMMAND vox_test)
endif()

# ===== 示例 =====
if(VOX_BUILD_EXAMPLES)
    function(add_vox_example example_name)
        add_executable(${example_name} examples/${example_name}.c)
        target_link_libraries(${example_name} PRIVATE vox)
        target_include_directories(${example_name} PRIVATE ${PROJECT_SOURCE_DIR})
        
        # 为性能测试示例启用额外优化
        if(example_name STREQUAL "json_benchmark" OR example_name STREQUAL "json_perf_test")
            if(MSVC)
                # 使用生成器表达式支持多配置生成器
                target_compile_options(${example_name} PRIVATE 
                    $<$<CONFIG:Release>:/O2 /Ob2 /Oi /Ot /GL>
                    $<$<CONFIG:RelWithDebInfo>:/O2 /Ob2 /Oi /Ot /GL>)
                set_target_properties(${example_name} PROPERTIES
                    INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE
                    INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO TRUE
                    LINK_FLAGS_RELEASE "/LTCG"
                    LINK_FLAGS_RELWITHDEBINFO "/LTCG")
            else()
                if(CMAKE_BUILD_TYPE STREQUAL "Release" OR CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
                    target_compile_options(${example_name} PRIVATE -O3 -march=native)
                    set_target_properties(${example_name} PROPERTIES
                        INTERPROCEDURAL_OPTIMIZATION TRUE)
                endif()
            endif()
        endif()
    endfunction()
    add_vox_example(mpool_example)
    add_vox_example(htable_example)
    add_vox_example(rbtree_example)
    add_vox_example(mheap_example)
    add_vox_example(vector_example)
    add_vox_example(string_example)
    add_vox_example(queue_example)
    add_vox_example(file_example)
    add_vox_example(time_example)
    add_vox_example(atomic_example)
    add_vox_example(crypto_example)
    add_vox_example(scanner_example)
    add_vox_example(scanner_stream_example)
    add_vox_example(regex_example)
    add_vox_example(json_example)
    add_vox_example(xml_example)
    add_vox_example(toml_example)
    add_vox_example(process_example)
    add_vox_example(mutex_example)
    add_vox_example(tpool_example)
    add_vox_example(socket_example)
    add_vox_example(loop_example)
    add_vox_example(loop_queue_work_example)
    add_vox_example(fs_example)
    add_vox_example(dns_example)
    if(VOX_USE_HTTP)
        add_vox_example(http_parser_example)
        add_vox_example(multipart_parser_example)
        add_vox_example(http_server_example)
        add_vox_example(http_middleware_example)
        add_vox_example(http_client_example)
        add_vox_example(ws_echo_example)
    endif()
    if(VOX_USE_WEBSOCKET)
        add_vox_example(websocket_echo_server)
        add_vox_example(websocket_echo_client)
    endif()
    if(VOX_USE_REDIS)
        add_vox_example(redis_client_example)
        add_vox_example(redis_pool_dynamic_example)
        add_vox_example(redis_pool_comparison)
    endif()
    if(VOX_USE_COROUTINE)
        add_vox_example(coroutine_pool_example)
        add_vox_example(coroutine_clients_example)
    endif()

    # DB 示例（当启用 DB 模块且至少启用一个 DB 后端时才构建）
    if(VOX_USE_DB AND (VOX_USE_SQLITE3 OR VOX_USE_DUCKDB OR VOX_USE_PGSQL OR VOX_USE_MYSQL))
        add_vox_example(db_orm_example)
        add_vox_example(db_example)
        add_vox_example(db_async_worker_example)
        add_vox_example(db_async_loop_example)
        add_vox_example(db_async_wait_example)
        add_vox_example(db_async_complex_example)
        add_vox_example(db_sync_example)
        add_vox_example(db_pool_async_example)
        add_vox_example(db_coroutine_example)
        add_vox_example(http_server_db_sync_example)
        add_vox_example(http_server_db_async_example)
        
        # MySQL 特定示例
        if(VOX_USE_MYSQL)
            add_vox_example(db_pool_async_mysql_example)
        endif()
        
        # PostgreSQL 特定示例
        if(VOX_USE_PGSQL)
            add_vox_example(db_pool_async_pgsql_example)
        endif()
    endif()
    
    # 协程示例（需要数据库支持，已在上面条件中检查）
    if(VOX_USE_SQLITE3 OR VOX_USE_DUCKDB OR VOX_USE_PGSQL OR VOX_USE_MYSQL)
        #add_vox_example(coroutine_example)
    endif()
    add_vox_example(tcp_echo_test)
    add_vox_example(udp_echo_test)
    
    # TLS echo 测试（需要 SSL 模块与 OpenSSL）
    if(VOX_USE_SSL AND VOX_USE_OPENSSL AND OpenSSL_FOUND)
        add_vox_example(https_server_example)
        add_vox_example(tls_echo_test)
        add_vox_example(dtls_echo_test)
    endif()
endif()

# ===== 打印配置信息 =====
message(STATUS "")
message(STATUS "Vox Configuration:")
message(STATUS "  Version:          ${PROJECT_VERSION}")
message(STATUS "  Build type:       ${CMAKE_BUILD_TYPE}")
message(STATUS "  C Compiler:       ${CMAKE_C_COMPILER_ID}")
message(STATUS "  Build tests:      ${VOX_BUILD_TESTS}")
message(STATUS "  Build examples:   ${VOX_BUILD_EXAMPLES}")
message(STATUS "  Build shared:     ${VOX_BUILD_SHARED}")
message(STATUS "  Modules: coroutine=${VOX_USE_COROUTINE} ssl=${VOX_USE_SSL} http=${VOX_USE_HTTP} websocket=${VOX_USE_WEBSOCKET} redis=${VOX_USE_REDIS} db=${VOX_USE_DB}")
if(UNIX AND NOT APPLE)
    message(STATUS "  Use io_uring:     ${VOX_USE_IOURING}")
endif()
message(STATUS "  Enable logging:   ${VOX_ENABLE_LOG}")
message(STATUS "  Enable assert:    ${VOX_ENABLE_ASSERT}")
if(VOX_USE_OPENSSL)
    message(STATUS "  Use OpenSSL:      ${VOX_USE_OPENSSL}")
    if(OpenSSL_FOUND)
        message(STATUS "  OpenSSL version: ${OPENSSL_VERSION}")
    endif()
elseif(VOX_USE_WOLFSSL)
    message(STATUS "  Use WolfSSL:      ${VOX_USE_WOLFSSL}")
elseif(VOX_USE_MBEDTLS)
    message(STATUS "  Use mbedTLS:      ${VOX_USE_MBEDTLS}")
endif()
if(CMAKE_BUILD_TYPE STREQUAL "Release" OR CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
    if(MSVC)
        message(STATUS "  Optimization:     /O2 /Ob2 /Oi /Ot (MSVC)")
    else()
        message(STATUS "  Optimization:     -O3 -march=native (GCC/Clang)")
    endif()
endif()
message(STATUS "")
