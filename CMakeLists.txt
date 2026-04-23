cmake_minimum_required(VERSION 3.28)

# Guard: the vcpkg preset expands $env{VCPKG_ROOT}. If the shell has not set
# it, CMake otherwise produces a cryptic "Could not find toolchain file
# /scripts/buildsystems/vcpkg.cmake" from deep in CMakeDetermineSystem.
# Catch it here with something actionable.
if(DEFINED CMAKE_TOOLCHAIN_FILE
   AND CMAKE_TOOLCHAIN_FILE MATCHES "^/?scripts/buildsystems/vcpkg\\.cmake$")
    message(FATAL_ERROR
        "\n"
        "VCPKG_ROOT is not set in this shell, but the preset expects vcpkg.\n"
        "Set VCPKG_ROOT and re-run the preset:\n"
        "  PowerShell: `$env:VCPKG_ROOT = \"C:\\path\\to\\vcpkg\"`\n"
        "  cmd:        `set VCPKG_ROOT=C:\\path\\to\\vcpkg`\n")
endif()

project(freikino
    VERSION 0.1.0
    DESCRIPTION "Freikino video player"
    LANGUAGES CXX
)

if(NOT WIN32)
    message(FATAL_ERROR "Freikino targets Windows only.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Reject in-tree builds.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
    message(FATAL_ERROR "In-source builds are forbidden. Use a build/ directory.")
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
include(compile_options)

add_subdirectory(src)

# Tests are opt-in and pull gtest in via the `tests` vcpkg manifest
# feature. The `vs2022-x64-tests` preset sets both together.
option(FREIKINO_BUILD_TESTS "Build GoogleTest-based unit tests" OFF)
if(FREIKINO_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
