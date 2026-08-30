cmake_minimum_required(VERSION 3.16)

project(mstly_matching_engine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# ZeroMQ (libzmq) + cppzmq (header-only C++ binding)
#
# Two supported provisioning paths:
#   1. vcpkg (preferred): configure with
#        -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake
#      vcpkg exports proper CONFIG packages for both ZeroMQ and cppzmq.
#   2. System / MSYS2-MinGW: the cppzmq package ships cppzmqConfig.cmake and
#      pulls in libzmq itself. Point CMake at it with
#        -DCMAKE_PREFIX_PATH=C:/msys64/ucrt64
#
# The `cppzmq` imported target carries the libzmq link + include dirs, so
# linking against it is all that is needed. A manual fallback is provided for
# environments that expose only the headers/lib without the CONFIG package.
# ---------------------------------------------------------------------------
find_package(cppzmq CONFIG QUIET)

file(GLOB ENGINE_SOURCES "engine/src/*.cpp")
add_executable(mstly_engine src/main.cpp ${ENGINE_SOURCES})
target_include_directories(mstly_engine PRIVATE engine/include)

if(TARGET cppzmq)
    message(STATUS "ZMQ: using cppzmq CONFIG package")
    target_link_libraries(mstly_engine PRIVATE cppzmq)
else()
    message(STATUS "ZMQ: cppzmq CONFIG package not found, falling back to manual discovery")
    find_path(ZMQ_INCLUDE_DIR NAMES zmq.hpp)
    find_library(ZMQ_LIBRARY NAMES zmq libzmq)
    if(NOT ZMQ_INCLUDE_DIR OR NOT ZMQ_LIBRARY)
        message(FATAL_ERROR "Could not locate zmq.hpp / libzmq. "
                            "Provide vcpkg toolchain or set CMAKE_PREFIX_PATH.")
    endif()
    target_include_directories(mstly_engine PRIVATE ${ZMQ_INCLUDE_DIR})
    target_link_libraries(mstly_engine PRIVATE ${ZMQ_LIBRARY})
endif()
