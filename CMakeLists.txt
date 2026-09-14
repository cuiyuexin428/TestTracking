cmake_minimum_required(VERSION 3.16)
project(stp_test_tracking CXX)

enable_testing()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    # Force UTF-8 source encoding on MSVC to avoid misparsing non-ASCII text.
    add_compile_options(/utf-8)
endif()

# Executables and DLLs share one output directory, so common.dll sits next to
# every exe and is found at runtime without PATH setup.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>")

# Locate third-party libraries (installed by tools/build_third_party.ps1).
include(cmake/find_package.cmake)

# Core library and unit tests.
add_subdirectory(common)

add_subdirectory(demo)
add_subdirectory(scheduler)
add_subdirectory(client)
add_subdirectory(plugins)
add_subdirectory(test)

# Future programs:

# Keep shared third-party DLLs (protobuf, abseil) next to every executable.
add_custom_target(deploy_third_party ALL
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:protobuf::libprotobuf>"
        "$<TARGET_FILE:absl::abseil_dll>"
        "${CMAKE_BINARY_DIR}/bin/$<CONFIG>/"
)
add_dependencies(deploy_third_party common)

# Deploy libpq and its dependencies next to every executable so DB-backed
# storage works without PATH setup.
add_custom_target(deploy_libpq ALL
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${PG_BIN_DIR}/libpq.dll"
        "${PG_BIN_DIR}/libintl-9.dll"
        "${PG_BIN_DIR}/libiconv-2.dll"
        "${PG_BIN_DIR}/libwinpthread-1.dll"
        "${PG_BIN_DIR}/libssl-3-x64.dll"
        "${PG_BIN_DIR}/libcrypto-3-x64.dll"
        "${CMAKE_BINARY_DIR}/bin/$<CONFIG>/"
)
add_dependencies(deploy_libpq common)

# All local executable and DLL targets share the same runtime dependencies.
function(attach_runtime_dependencies directory)
    get_property(targets DIRECTORY "${directory}" PROPERTY BUILDSYSTEM_TARGETS)
    foreach(target IN LISTS targets)
        get_target_property(kind ${target} TYPE)
        if(kind STREQUAL "EXECUTABLE" OR kind STREQUAL "SHARED_LIBRARY")
            if(NOT target STREQUAL "common")
                add_dependencies(${target} deploy_third_party deploy_libpq)
            endif()
        endif()
    endforeach()
    get_property(children DIRECTORY "${directory}" PROPERTY SUBDIRECTORIES)
    foreach(child IN LISTS children)
        attach_runtime_dependencies("${child}")
    endforeach()
endfunction()
attach_runtime_dependencies("${CMAKE_CURRENT_SOURCE_DIR}")
