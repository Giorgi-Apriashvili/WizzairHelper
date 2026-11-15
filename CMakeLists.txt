cmake_minimum_required(VERSION 3.30)

# Use the pinned vcpkg submodule unless the caller supplies a toolchain
# (git worktrees without the submodule pass -DCMAKE_TOOLCHAIN_FILE explicitly).
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/external/vcpkg/scripts/buildsystems/vcpkg.cmake")
    set(CMAKE_TOOLCHAIN_FILE "${CMAKE_CURRENT_SOURCE_DIR}/external/vcpkg/scripts/buildsystems/vcpkg.cmake"
        CACHE FILEPATH "vcpkg toolchain file")
endif()

option(WIZZ_BUILD_GUI "Build the Qt 6 desktop application (WizzairHelper.exe)" ON)
option(WIZZ_BUILD_TESTS "Build the offline conformance tests (wizzairhelper_tests)" ON)
option(WIZZ_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)

# vcpkg manifest features must be selected before project().
if(WIZZ_BUILD_GUI)
    list(APPEND VCPKG_MANIFEST_FEATURES "gui")
endif()
if(WIZZ_BUILD_TESTS)
    list(APPEND VCPKG_MANIFEST_FEATURES "tests")
endif()

project(WizzairHelper
    VERSION 1.0.0
    DESCRIPTION "Finds the cheapest Wizz Air trips from airports near a chosen starting place"
    LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "WizzairHelper targets Windows x64 only.")
endif()

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")
include(WizzProjectOptions)

add_subdirectory(src/core)
add_subdirectory(src/services)

if(WIZZ_BUILD_GUI)
    add_subdirectory(src/app)
endif()

if(WIZZ_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
