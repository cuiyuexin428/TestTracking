add_library(scheduler_core STATIC
    src/Server.cpp
    src/Scheduler.cpp
    src/SchedulerMessages.cpp
    src/SchedulerManagement.cpp
    src/SchedulerClientControl.cpp
    src/SchedulerDeployment.cpp
    src/SchedulerMaintenance.cpp
    src/SchedulerRollout.cpp
    src/ManagementServer.cpp
    src/Storage.cpp
)

target_include_directories(scheduler_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/inc
    ${PG_INCLUDE_DIR}
)

target_link_libraries(scheduler_core PUBLIC common ${PG_LIBRARY})
if(WIN32)
    target_link_libraries(scheduler_core PUBLIC ws2_32)
endif()
add_executable(scheduler src/main.cpp)
target_link_libraries(scheduler PRIVATE scheduler_core)

# Keep scheduler.json in a config/ directory next to the executable. This
# target runs on every build so config changes always reach the output dir.
add_custom_target(scheduler_config ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/bin/$<CONFIG>/config
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_CURRENT_SOURCE_DIR}/scheduler.json
        ${CMAKE_BINARY_DIR}/bin/$<CONFIG>/config/scheduler.json
)
add_dependencies(scheduler scheduler_config)
