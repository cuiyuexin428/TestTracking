protobuf_generate(
    LANGUAGE cpp
    OUT_VAR DEMO_PROTO_SRCS
    PROTOS demo.proto
    PROTOC_OUT_DIR ${CMAKE_CURRENT_BINARY_DIR}
)

add_library(demo SHARED
    demo.cpp
    ${DEMO_PROTO_SRCS}
)

target_include_directories(demo PRIVATE
    ${CMAKE_CURRENT_BINARY_DIR}
)

target_link_libraries(demo PRIVATE common)
