add_executable(test_common test_common.cpp)
target_link_libraries(test_common PRIVATE common)

add_executable(test_zmq test_zmq.cpp)
target_link_libraries(test_zmq PRIVATE common)

add_executable(test_config test_config.cpp)
target_link_libraries(test_config PRIVATE common)

add_executable(test_messages test_messages.cpp)
target_link_libraries(test_messages PRIVATE common)

# Regression test for the stpcompare scheduler's restore: in-flight tasks must
# survive a checkpoint + restore. Compiles the real plugin source so the test
# exercises the actual plan/requestTask/saveCheckpoint/restore path.
protobuf_generate(
    LANGUAGE cpp
    OUT_VAR STPCOMPARE_TEST_PROTO_SRCS
    PROTOS ${CMAKE_CURRENT_SOURCE_DIR}/../plugins/stpcompare/stpcompare.proto
    IMPORT_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/../plugins/stpcompare
    PROTOC_OUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/stpcompare_proto
)
add_executable(test_stpcompare_restore
    test_stpcompare_restore.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../plugins/stpcompare/src/SchedulerPlugin.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../plugins/stpcompare/src/StpStore.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../plugins/stpcompare/src/StpUtil.cpp
    ${STPCOMPARE_TEST_PROTO_SRCS}
)
target_include_directories(test_stpcompare_restore PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../plugins/stpcompare/inc
    ${CMAKE_CURRENT_BINARY_DIR}/stpcompare_proto
)
target_link_libraries(test_stpcompare_restore PRIVATE common ${PG_LIBRARY})

add_test(NAME test_common COMMAND test_common)
add_test(NAME test_zmq COMMAND test_zmq)
add_test(NAME test_config COMMAND test_config)
add_test(NAME test_messages COMMAND test_messages)
add_test(NAME test_stpcompare_restore COMMAND test_stpcompare_restore)
