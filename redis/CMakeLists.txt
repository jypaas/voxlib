# Redis 模块：仅当 VOX_USE_REDIS=ON 时由主 CMakeLists.txt 包含

set(REDIS_DIR ${CMAKE_CURRENT_SOURCE_DIR})
target_sources(${VOX_LIB_TARGET} PRIVATE
    ${REDIS_DIR}/vox_redis_parser.c
    ${REDIS_DIR}/vox_redis_client.c
    ${REDIS_DIR}/vox_redis_pool.c
)
target_include_directories(${VOX_LIB_TARGET} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
