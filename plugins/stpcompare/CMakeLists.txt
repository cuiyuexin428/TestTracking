protobuf_generate(
    LANGUAGE cpp
    OUT_VAR STPCOMPARE_PROTO_SRCS
    PROTOS stpcompare.proto
    PROTOC_OUT_DIR ${CMAKE_CURRENT_BINARY_DIR}
)

add_library(stpcompare SHARED
    src/plugin.cpp
    src/SchedulerPlugin.cpp
    src/ClientPlugin.cpp
    src/StpStore.cpp
    src/StpUtil.cpp
    ${STPCOMPARE_PROTO_SRCS}
)

target_include_directories(stpcompare PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/inc
    ${CMAKE_CURRENT_BINARY_DIR}
    ${PG_INCLUDE_DIR}
)

target_link_libraries(stpcompare PRIVATE common ${PG_LIBRARY})
