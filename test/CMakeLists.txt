add_executable(test_common test_common.cpp)
target_link_libraries(test_common PRIVATE common)

add_executable(test_zmq test_zmq.cpp)
target_link_libraries(test_zmq PRIVATE common)

add_executable(test_config test_config.cpp)
target_link_libraries(test_config PRIVATE common)

add_executable(test_messages test_messages.cpp)
target_link_libraries(test_messages PRIVATE common)

add_test(NAME test_common COMMAND test_common)
add_test(NAME test_zmq COMMAND test_zmq)
add_test(NAME test_config COMMAND test_config)
add_test(NAME test_messages COMMAND test_messages)
