protobuf_generate(
    LANGUAGE cpp
    OUT_VAR COVERAGE_PROTO_SRCS
    PROTOS coverage.proto
    PROTOC_OUT_DIR ${CMAKE_CURRENT_BINARY_DIR}
)

add_library(coverage SHARED
    src/plugin.cpp
    src/SchedulerPlugin.cpp
    src/ClientPlugin.cpp
    src/CoverageStore.cpp
    src/CoverageUtil.cpp
    ${THIRD_PARTY_ROOT}/tinyxml2/tinyxml2.cpp
    ${COVERAGE_PROTO_SRCS}
)

target_include_directories(coverage PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/inc
    ${CMAKE_CURRENT_BINARY_DIR}
    ${PG_INCLUDE_DIR}
)

target_link_libraries(coverage PRIVATE common ${PG_LIBRARY})
