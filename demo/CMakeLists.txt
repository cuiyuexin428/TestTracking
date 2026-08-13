# Demo pair: ZmqRouter server + ZmqDealer client.
add_executable(demo_server server.cpp)
target_link_libraries(demo_server PRIVATE common)

add_executable(demo_client client.cpp)
target_link_libraries(demo_client PRIVATE common)
