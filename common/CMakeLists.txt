# protobuf 25.3 parses DEPENDENCIES as a single argument. Keep imported
# schemas in one CMake list so changes to command.proto regenerate
# communication.pb.* as well.
set(PROTO_IMPORTS
    ${CMAKE_CURRENT_SOURCE_DIR}/proto/common.proto
    ${CMAKE_CURRENT_SOURCE_DIR}/proto/heartbeat.proto
    ${CMAKE_CURRENT_SOURCE_DIR}/proto/command.proto
)
# The protobuf 25.3 helper expands ARGN unquoted before parsing it. Escape
# list separators so DEPENDENCIES remains one argument until the helper adds
# it to add_custom_command(DEPENDS ...).
string(REPLACE ";" "\\\\;" PROTO_IMPORTS_ARGUMENT "${PROTO_IMPORTS}")

# Shared base library: ZMQ wrapper, blocking queue, event dispatch, logging,
# config access, protocol messages.
protobuf_generate(
    LANGUAGE cpp
    OUT_VAR PROTO_SRCS
    PROTOS
        proto/common.proto
        proto/heartbeat.proto
        proto/command.proto
        proto/communication.proto
    DEPENDENCIES "${PROTO_IMPORTS_ARGUMENT}"
    IMPORT_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/proto
    PROTOC_OUT_DIR ${CMAKE_CURRENT_BINARY_DIR}
    EXPORT_MACRO COMMON_API
)

# Shared library: one copy of the framework classes, generated protobuf
# messages, and the third-party static libraries (zmq/protobuf/absl) per
# process. WINDOWS_EXPORT_ALL_SYMBOLS exports everything (including the
# protobuf runtime) so plugin DLLs and executables resolve symbols through
# the common import library.
add_library(common SHARED
    src/version.cpp
    src/zmq_helper.cpp
    src/EventManager.cpp
    src/Logger.cpp
    src/Config.cpp
    src/DynamicLibrary.cpp
    ${PROTO_SRCS}
)

set_target_properties(common PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)

target_include_directories(common PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/inc
    ${CMAKE_CURRENT_BINARY_DIR}
    ${THIRD_PARTY_INCLUDE_DIRS}
)

# libzmq is linked PRIVATE into this DLL; nobody needs its symbols across the
# boundary. protobuf is a shared DLL and its import library propagates to
# consumers (together with absl::abseil_dll).
get_target_property(_zmq_include_dirs libzmq-static INTERFACE_INCLUDE_DIRECTORIES)
get_target_property(_absl_include_dirs absl::abseil_dll INTERFACE_INCLUDE_DIRECTORIES)
target_include_directories(common PUBLIC
    ${_zmq_include_dirs}
    ${_absl_include_dirs}
)

target_link_libraries(common PRIVATE libzmq-static)
target_link_libraries(common PUBLIC protobuf::libprotobuf)

# Generated messages are part of common.dll; export.h resolves COMMON_API to
# dllexport while building this DLL and to dllimport for every consumer.
target_compile_definitions(common PRIVATE COMMON_BUILD)
if(MSVC)
    target_compile_options(common PRIVATE /FIcommon/export.h)
    target_compile_options(common INTERFACE /FIcommon/export.h)
else()
    target_compile_options(common PRIVATE -include common/export.h)
    target_compile_options(common INTERFACE -include common/export.h)
endif()

if(UNIX)
    # dlopen/dlsym/dlclose
    target_link_libraries(common PRIVATE ${CMAKE_DL_LIBS})
endif()
