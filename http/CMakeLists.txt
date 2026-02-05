# HTTP 模块：仅当 VOX_USE_HTTP=ON 时由主 CMakeLists.txt 包含

set(HTTP_DIR ${CMAKE_CURRENT_SOURCE_DIR})
set(HTTP_SOURCES
    ${HTTP_DIR}/vox_http_parser.c
    ${HTTP_DIR}/vox_http_multipart_parser.c
    ${HTTP_DIR}/vox_http_context.c
    ${HTTP_DIR}/vox_http_router.c
    ${HTTP_DIR}/vox_http_engine.c
    ${HTTP_DIR}/vox_http_server.c
    ${HTTP_DIR}/vox_http_ws.c
    ${HTTP_DIR}/vox_http_client.c
    ${HTTP_DIR}/vox_http_gzip.c
    ${HTTP_DIR}/vox_http_mime.c
    ${HTTP_DIR}/vox_http_middleware.c
)

target_sources(${VOX_LIB_TARGET} PRIVATE ${HTTP_SOURCES})
target_include_directories(${VOX_LIB_TARGET} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
