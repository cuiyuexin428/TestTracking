foreach(name common zmq config messages)
    add_executable(test_${name} test_${name}.cpp)
    target_link_libraries(test_${name} PRIVATE common)
    add_test(NAME test_${name} COMMAND test_${name})
endforeach()
add_executable(test_stpcompare_restore test_stpcompare_restore.cpp)
target_link_libraries(test_stpcompare_restore PRIVATE stpcompare_core)
add_test(NAME test_stpcompare_restore COMMAND test_stpcompare_restore)

add_executable(test_coverage_client test_coverage_client.cpp)
target_link_libraries(test_coverage_client PRIVATE coverage_core)
add_test(NAME test_coverage_client COMMAND test_coverage_client)

# Prefer the portable analysis runtime when present; it includes the database
# driver required by the integration tests and also travels into the intranet.
if(NOT Python3_EXECUTABLE AND EXISTS "${CMAKE_SOURCE_DIR}/runtime/python/python.exe")
    set(Python3_EXECUTABLE "${CMAKE_SOURCE_DIR}/runtime/python/python.exe")
endif()
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    add_test(NAME test_scheduler_logging COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/test_scheduler_logging.py --scheduler $<TARGET_FILE:scheduler> -v)
    set_tests_properties(test_scheduler_logging PROPERTIES TIMEOUT 40)
    add_test(NAME test_scheduler_management COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/test_scheduler_management.py
        --scheduler $<TARGET_FILE:scheduler> --probe $<TARGET_FILE:scheduler_probe> -v)
    set_tests_properties(test_scheduler_management PROPERTIES TIMEOUT 120)
    add_test(NAME test_result_service COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/test_result_service.py -v)
    set_tests_properties(test_result_service PROPERTIES TIMEOUT 30)
    add_test(NAME test_plugin_views COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/test_plugin_views.py -v)
    set_tests_properties(test_plugin_views PROPERTIES TIMEOUT 30)
    foreach(name dsh_proxy reports analysis_catalog python_analysis_dependencies ai_providers management management_http artifacts deployments program_bindings server_plugins)
        add_test(NAME test_${name} COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/test_${name}.py -v)
        set_tests_properties(test_${name} PROPERTIES TIMEOUT 90)
    endforeach()
    if(WIN32)
        add_test(NAME test_client_deployment_integration COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/test_client_deployment_integration.py
            --bin-dir $<TARGET_FILE_DIR:client> -v)
        set_tests_properties(test_client_deployment_integration PROPERTIES TIMEOUT 120)
        add_test(NAME test_client_update COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/test_client_update.py -v)
        set_tests_properties(test_client_update PROPERTIES TIMEOUT 150)
        add_test(NAME test_package_portability COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/test_package_portability.py -v)
        set_tests_properties(test_package_portability PROPERTIES TIMEOUT 90)
    endif()
endif()

find_program(NodeJS_EXECUTABLE NAMES node nodejs)
if(NodeJS_EXECUTABLE AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test_result_web.js")
    add_test(NAME test_result_web COMMAND ${NodeJS_EXECUTABLE}
        --test ${CMAKE_CURRENT_SOURCE_DIR}/test_result_web.js)
    set_tests_properties(test_result_web PROPERTIES TIMEOUT 30)
endif()
add_executable(test_client_runtime test_client_runtime.cpp ../client/src/TaskExecutor.cpp)
target_include_directories(test_client_runtime PRIVATE ../client/inc)
target_link_libraries(test_client_runtime PRIVATE common)
add_test(NAME test_client_runtime COMMAND test_client_runtime)
set_tests_properties(test_client_runtime PROPERTIES TIMEOUT 30)
add_executable(test_client_protocol test_client_protocol.cpp
    ../client/src/Client.cpp ../client/src/TaskExecutor.cpp)
target_include_directories(test_client_protocol PRIVATE ../client/inc)
target_link_libraries(test_client_protocol PRIVATE common)
add_library(client_retry_probe SHARED client_retry_probe_plugin.cpp)
target_link_libraries(client_retry_probe PRIVATE common)
add_dependencies(test_client_protocol demo client_retry_probe client)
add_test(NAME test_client_protocol COMMAND test_client_protocol)
set_tests_properties(test_client_protocol PROPERTIES TIMEOUT 90)
add_library(scheduler_probe SHARED scheduler_probe_plugin.cpp)
target_link_libraries(scheduler_probe PRIVATE common)
add_executable(test_scheduler_protocol test_scheduler_protocol.cpp)
target_link_libraries(test_scheduler_protocol PRIVATE scheduler_core)
add_dependencies(test_scheduler_protocol scheduler_probe)
add_test(NAME test_scheduler_protocol COMMAND test_scheduler_protocol)
set_tests_properties(test_scheduler_protocol PROPERTIES TIMEOUT 20)
add_executable(test_plugin_runtime test_plugin_runtime.cpp)
target_include_directories(test_plugin_runtime PRIVATE ../plugins/coverage/inc ../plugins/stpcompare/inc)
target_link_libraries(test_plugin_runtime PRIVATE common coverage_core stpcompare_core ${PG_LIBRARY})
add_test(NAME test_plugin_runtime COMMAND test_plugin_runtime)
set_tests_properties(test_plugin_runtime PROPERTIES TIMEOUT 60)
if(WIN32 AND Python3_Interpreter_FOUND AND EXISTS "${PG_BIN_DIR}/initdb.exe")
    add_test(NAME test_schema_startup COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/test_schema_startup.py
        --bin-dir $<TARGET_FILE_DIR:scheduler> --pg-bin ${PG_BIN_DIR} -v)
    set_tests_properties(test_schema_startup PROPERTIES TIMEOUT 90)
    add_test(NAME test_management_integration COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/test_management_integration.py
        --bin-dir $<TARGET_FILE_DIR:scheduler> --pg-bin ${PG_BIN_DIR} -v)
    set_tests_properties(test_management_integration PROPERTIES TIMEOUT 150)
    add_test(NAME test_coverage_settle COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/test_coverage_settle.py
        --exe $<TARGET_FILE:coverage_settle> --pg-bin ${PG_BIN_DIR} -v)
    set_tests_properties(test_coverage_settle PROPERTIES TIMEOUT 180)
    add_test(NAME test_runtime_integration COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/test_runtime_integration.py
        --bin-dir $<TARGET_FILE_DIR:scheduler> --pg-bin ${PG_BIN_DIR} -v)
    set_tests_properties(test_runtime_integration PROPERTIES TIMEOUT 300)
    add_test(NAME test_tools_integration COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/test_tools_integration.py --pg-bin ${PG_BIN_DIR} -v)
    set_tests_properties(test_tools_integration PROPERTIES TIMEOUT 120)
    add_test(NAME test_client_startup COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/test_client_startup.py
        --client $<TARGET_FILE:client> --demo $<TARGET_FILE:demo>)
    set_tests_properties(test_client_startup PROPERTIES TIMEOUT 30)
    # test_plugin_runtime keeps its PostgreSQL contracts behind `--db`; this
    # wrapper supplies a private instance so they run in CTest.
    add_test(NAME test_plugin_runtime_db COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/test_plugin_runtime_db.py
        --exe $<TARGET_FILE:test_plugin_runtime> --pg-bin ${PG_BIN_DIR})
    set_tests_properties(test_plugin_runtime_db PROPERTIES TIMEOUT 300)
endif()
