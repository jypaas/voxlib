# 协程模块：仅当 VOX_USE_COROUTINE=ON 时由主 CMakeLists.txt 包含
# 向主目标 vox 添加协程相关源文件

set(COROUTINE_DIR ${CMAKE_CURRENT_SOURCE_DIR})
set(COROUTINE_SOURCES
    ${COROUTINE_DIR}/vox_coroutine.c
    ${COROUTINE_DIR}/vox_coroutine_promise.c
    ${COROUTINE_DIR}/vox_coroutine_pool.c
    ${COROUTINE_DIR}/vox_coroutine_scheduler.c
    ${COROUTINE_DIR}/vox_coroutine_db.c
    ${COROUTINE_DIR}/vox_coroutine_dns.c
    ${COROUTINE_DIR}/vox_coroutine_fs.c
    ${COROUTINE_DIR}/vox_coroutine_redis.c
    ${COROUTINE_DIR}/vox_coroutine_http.c
    ${COROUTINE_DIR}/vox_coroutine_ws.c
)

# 平台相关的上下文切换实现
if(WIN32)
    list(APPEND COROUTINE_SOURCES ${COROUTINE_DIR}/vox_coroutine_context_win.c)
else()
    list(APPEND COROUTINE_SOURCES ${COROUTINE_DIR}/vox_coroutine_context_asm.c)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64|AMD64")
        list(APPEND COROUTINE_SOURCES ${COROUTINE_DIR}/vox_coroutine_context_x64.S)
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64|ARM64")
        list(APPEND COROUTINE_SOURCES ${COROUTINE_DIR}/vox_coroutine_context_arm64.S)
    endif()
endif()

target_sources(${VOX_LIB_TARGET} PRIVATE ${COROUTINE_SOURCES})

target_include_directories(${VOX_LIB_TARGET} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
