# MQTT 模块：仅当 VOX_USE_MQTT=ON 时由主 CMakeLists.txt 包含

set(MQTT_DIR ${CMAKE_CURRENT_SOURCE_DIR})
target_sources(${VOX_LIB_TARGET} PRIVATE
    ${MQTT_DIR}/vox_mqtt_parser.c
    ${MQTT_DIR}/vox_mqtt_client.c
    ${MQTT_DIR}/vox_mqtt_server.c
)
target_include_directories(${VOX_LIB_TARGET} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
