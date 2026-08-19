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

# PostgreSQL client library (libpq) for DB-backed storage. Points at a local
# PostgreSQL install (e.g. the unpacked binary zip used for development).
set(PG_ROOT "" CACHE PATH
    "PostgreSQL install root containing include/ lib/ bin/")
if(NOT PG_ROOT AND DEFINED ENV{PG_ROOT})
    set(PG_ROOT "$ENV{PG_ROOT}")
endif()
if(NOT PG_ROOT)
    file(GLOB _pg_candidates
        "C:/Program Files/PostgreSQL/*"
        "C:/pgsql"
        "${THIRD_PARTY_ROOT}/pgsql"
        "C:/WorkSpace/ThirdParty/pgsql")
    foreach(_pg_candidate IN LISTS _pg_candidates)
        if(EXISTS "${_pg_candidate}/include/libpq-fe.h" AND
           (EXISTS "${_pg_candidate}/lib/libpq.lib" OR EXISTS "${_pg_candidate}/lib/libpq.so"))
            set(PG_ROOT "${_pg_candidate}")
            break()
        endif()
    endforeach()
endif()
if(NOT PG_ROOT)
    message(FATAL_ERROR
        "PostgreSQL/libpq was not found. Install PostgreSQL or pass -DPG_ROOT=<postgres-root> "
        "(a directory containing include/libpq-fe.h and lib/libpq.lib).")
endif()
set(PG_INCLUDE_DIR "${PG_ROOT}/include" CACHE PATH "libpq headers directory")
set(PG_LIBRARY "${PG_ROOT}/lib/libpq.lib" CACHE FILEPATH "libpq import library")
set(PG_BIN_DIR "${PG_ROOT}/bin" CACHE PATH "PostgreSQL bin directory (libpq.dll and deps)")

if(NOT EXISTS "${PG_INCLUDE_DIR}/libpq-fe.h" OR NOT EXISTS "${PG_LIBRARY}")
    message(FATAL_ERROR
        "libpq not found under ${PG_ROOT}; set PG_ROOT to a PostgreSQL install "
        "containing include/libpq-fe.h and lib/libpq.lib")
endif()

message(STATUS "PG_ROOT=${PG_ROOT}")
