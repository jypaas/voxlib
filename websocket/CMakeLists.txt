# WebSocket 模块：仅当 VOX_USE_WEBSOCKET=ON 时由主 CMakeLists.txt 包含

set(WEBSOCKET_DIR ${CMAKE_CURRENT_SOURCE_DIR})
target_sources(${VOX_LIB_TARGET} PRIVATE
    ${WEBSOCKET_DIR}/vox_websocket.c
    ${WEBSOCKET_DIR}/vox_websocket_server.c
    ${WEBSOCKET_DIR}/vox_websocket_client.c
)
target_include_directories(${VOX_LIB_TARGET} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
