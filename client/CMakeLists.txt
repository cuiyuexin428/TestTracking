add_executable(client
    src/main.cpp
    src/Client.cpp
    src/TaskExecutor.cpp
)

target_include_directories(client PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/inc
)

target_link_libraries(client PRIVATE common)

# Keep client.json in a config/ directory next to the executable. This
# target runs on every build so config changes always reach the output dir.
add_custom_target(client_config ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/bin/$<CONFIG>/config
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_CURRENT_SOURCE_DIR}/client.json
        ${CMAKE_BINARY_DIR}/bin/$<CONFIG>/config/client.json
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_CURRENT_SOURCE_DIR}/client.stpcompare.json
        ${CMAKE_BINARY_DIR}/bin/$<CONFIG>/config/client.stpcompare.json
)
add_dependencies(client client_config)
