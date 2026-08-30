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

# Threads: a thread de drain do AsyncJournal (Fase B) usa std::thread.
find_package(Threads REQUIRED)

file(GLOB ENGINE_SOURCES "engine/src/*.cpp")
add_executable(mstly_engine src/main.cpp ${ENGINE_SOURCES})
target_include_directories(mstly_engine PRIVATE engine/include)
target_link_libraries(mstly_engine PRIVATE Threads::Threads)

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

# ---------------------------------------------------------------------------
# Testes (CTest). Nao dependem de ZMQ: cada um linka so' o que precisa, entao
# `ctest` roda mesmo sem a libzmq instalada.
#   test_core     -> nucleo do matching (so' MatchingEngine)
#   test_journal  -> Fase B (AsyncJournal + ArenaAllocator + threads)
# ---------------------------------------------------------------------------
enable_testing()

add_executable(test_core tests/test_core.cpp engine/src/MatchingEngine.cpp)
target_include_directories(test_core PRIVATE engine/include)
add_test(NAME core COMMAND test_core)

add_executable(test_journal tests/test_journal.cpp
                            engine/src/AsyncJournal.cpp
                            engine/src/ArenaAllocator.cpp)
target_include_directories(test_journal PRIVATE engine/include)
target_link_libraries(test_journal PRIVATE Threads::Threads)
add_test(NAME journal COMMAND test_journal)
