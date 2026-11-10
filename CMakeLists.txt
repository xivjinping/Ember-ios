# Builds the portable C++ core as a static library.
#
#   Host tests (Linux/macOS):  cmake -S . -B build && cmake --build build && ctest --test-dir build
#   iOS device slice:          cmake -S . -B build-ios -G Xcode -DCMAKE_SYSTEM_NAME=iOS \
#                                    -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=15.0
#
# The app itself is built from project.yml (XcodeGen), which compiles these
# same sources directly, so CMake is only needed for tests and for backends
# that bring their own build system.
cmake_minimum_required(VERSION 3.20)
project(ember_core LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(EMEMA_BUILD_TESTS "Build host-side core tests" ON)

add_library(ember_core STATIC
    Core/src/backend/stub_backend.cpp
    Core/src/cpu/x86_interpreter.cpp
    Core/src/cpu/jit/jit_memory.cpp
    Core/src/fs/fatx.cpp
    Core/src/fs/mapped_file.cpp
    Core/src/fs/title_probe.cpp
    Core/src/fs/xbe.cpp
    Core/src/fs/xiso.cpp
    Core/src/kernel/xbe_loader.cpp
    Core/src/mem/xbox_memory.cpp
    Core/src/nv2a/pushbuffer.cpp
    Core/src/nv2a/texture_convert.cpp)
target_include_directories(ember_core PUBLIC Core/include)
target_compile_options(ember_core PRIVATE -Wall -Wextra)
if(NOT APPLE)
    target_compile_definitions(ember_core PRIVATE _GNU_SOURCE)
endif()

find_package(Threads REQUIRED)
target_link_libraries(ember_core PUBLIC Threads::Threads)

if(EMEMA_BUILD_TESTS AND NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
    enable_testing()
    add_executable(ember_core_tests Core/tests/core_tests.cpp)
    target_link_libraries(ember_core_tests PRIVATE ember_core)
    add_test(NAME core COMMAND ember_core_tests)
endif()
