protobuf_generate(LANGUAGE cpp OUT_VAR COVERAGE_PROTO_SRCS PROTOS coverage.proto
    PROTOC_OUT_DIR ${CMAKE_CURRENT_BINARY_DIR})

add_library(coverage_data STATIC
    src/CoverageStore.cpp src/CoverageUtil.cpp
    ${THIRD_PARTY_ROOT}/tinyxml2/tinyxml2.cpp ${COVERAGE_PROTO_SRCS})
target_include_directories(coverage_data PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/inc ${CMAKE_CURRENT_BINARY_DIR} ${PG_INCLUDE_DIR})
target_link_libraries(coverage_data PUBLIC common ${PG_LIBRARY})

add_library(coverage_core STATIC src/SchedulerPlugin.cpp src/ClientPlugin.cpp)
target_link_libraries(coverage_core PUBLIC coverage_data)
add_library(coverage SHARED src/plugin.cpp)
target_link_libraries(coverage PRIVATE coverage_core)

# Selection and merging exist only in the independent database tool.
add_executable(coverage_settle src/settle_coverage.cpp src/CoveragePostProcess.cpp)
target_link_libraries(coverage_settle PRIVATE coverage_data)
