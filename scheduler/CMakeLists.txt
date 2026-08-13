add_executable(scheduler
    src/main.cpp
    src/Server.cpp
    src/Scheduler.cpp
)

target_include_directories(scheduler PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/inc
)

target_link_libraries(scheduler PRIVATE common)

# Keep scheduler.json in a config/ directory next to the executable so the
# program finds it regardless of the working directory (e.g. IDE launches).
add_custom_command(TARGET scheduler POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:scheduler>/config
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_CURRENT_SOURCE_DIR}/scheduler.json
        $<TARGET_FILE_DIR:scheduler>/config/scheduler.json
)
