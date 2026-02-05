# SSL/TLS 模块：仅当 VOX_USE_SSL=ON 时由主 CMakeLists.txt 包含
# 依赖在主 CMakeLists 中已配置的 VOX_USE_OPENSSL / VOX_USE_WOLFSSL / VOX_USE_MBEDTLS
# 主 CMakeLists 负责 find_package(OpenSSL) 等，本文件只添加源文件

set(SSL_DIR ${CMAKE_CURRENT_SOURCE_DIR})
set(SSL_SOURCES
    ${SSL_DIR}/vox_ssl.c
    ${CMAKE_SOURCE_DIR}/vox_tls.c
    ${CMAKE_SOURCE_DIR}/vox_dtls.c
)

if(VOX_USE_OPENSSL)
    list(APPEND SSL_SOURCES ${SSL_DIR}/vox_ssl_openssl.c)
elseif(VOX_USE_WOLFSSL)
    # list(APPEND SSL_SOURCES ${SSL_DIR}/vox_ssl_wolfssl.c)
elseif(VOX_USE_MBEDTLS)
    # list(APPEND SSL_SOURCES ${SSL_DIR}/vox_ssl_mbedtls.c)
endif()

target_sources(${VOX_LIB_TARGET} PRIVATE ${SSL_SOURCES})
target_include_directories(${VOX_LIB_TARGET} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
