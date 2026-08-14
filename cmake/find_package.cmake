# Locate third-party libraries installed by tools/build_third_party.ps1
# into third_party/. The install tree travels with the project; rerun the
# build script offline to rebuild it on another machine.
#
# NOTE: the third-party static libraries (libzmq, protobuf, abseil) are built
# in both Release and Debug (/MD). The imported targets carry per-config
# locations, so the project can be built in either configuration.

set(THIRD_PARTY_ROOT "${CMAKE_SOURCE_DIR}/third_party" CACHE PATH
    "Third-party install directory")

file(GLOB _zmq_prefixes  "${THIRD_PARTY_ROOT}/libzmq-*")
file(GLOB _absl_prefixes "${THIRD_PARTY_ROOT}/abseil-cpp-*")
file(GLOB _pb_prefixes   "${THIRD_PARTY_ROOT}/protobuf-*")
list(APPEND CMAKE_PREFIX_PATH ${_zmq_prefixes} ${_absl_prefixes} ${_pb_prefixes})

find_package(ZeroMQ CONFIG REQUIRED)     # target: libzmq-static
find_package(protobuf CONFIG REQUIRED)   # target: protobuf::libprotobuf
find_package(absl CONFIG REQUIRED)       # targets: absl::*

# Header-only libraries (no cmake package; just add include paths).
set(THIRD_PARTY_INCLUDE_DIRS
    "${THIRD_PARTY_ROOT}/concurrentqueue"
    "${THIRD_PARTY_ROOT}/nlohmann"
    "${THIRD_PARTY_ROOT}/tinyxml2"
    CACHE INTERNAL "")

message(STATUS "THIRD_PARTY_ROOT=${THIRD_PARTY_ROOT}")
