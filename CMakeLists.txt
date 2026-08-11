# =============================================================================
# TIANSHU CMake build system (top-level)
# =============================================================================
#
# Per ADR-0003, TIANSHU supports both CMake and Bazel. This is the CMake side.
# Per ADR-0004, build entry must use native `cmake` commands; scenario overrides
# go through CMakePresets.json.
# Per ADR-0005, lightweight multi-platform via 5 profiles.
# Per ADR-0008, message format feature flags.
# Per ADR-0009, code comments in English.
# Per ADR-0017, Apache-2.0 license headers required on all source files.

cmake_minimum_required(VERSION 3.25)

# Read version from VERSION file (single source of truth).
# VERSION file contains a single line with semantic version, e.g. "0.1.0".
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" _version_raw)
string(STRIP "${_version_raw}" TIANSHU_VERSION)
if(NOT TIANSHU_VERSION)
    message(WARNING "VERSION file empty or missing; falling back to 0.0.0")
    set(TIANSHU_VERSION "0.0.0")
endif()

project(tianshu
    VERSION ${TIANSHU_VERSION}
    DESCRIPTION "TIANSHU - SLA-constrained declarative real-time dataflow compilation framework"
    HOMEPAGE_URL "https://github.com/lykling/tianshu"
    LANGUAGES CXX
)

# -----------------------------------------------------------------------------
# C++ standard (per ADR-0003)
# -----------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Cache FetchContent downloads outside build/ so rm -rf build/ doesn't re-clone.
set(FETCHCONTENT_BASE_DIR "${CMAKE_SOURCE_DIR}/.cache/fetchcontent")
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)  # Disable C++ modules (phase 0 simplicity)

# -----------------------------------------------------------------------------
# Default build type
# -----------------------------------------------------------------------------
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE "Debug" CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
        Debug Release RelWithDebInfo MinSizeRel)
endif()

# -----------------------------------------------------------------------------
# Position-independent code (required for shared libraries)
# -----------------------------------------------------------------------------
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# -----------------------------------------------------------------------------
# Output directories (cleaner tree)
# -----------------------------------------------------------------------------
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# -----------------------------------------------------------------------------
# Compile commands for clangd/IDE
# -----------------------------------------------------------------------------
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Convenience symlink at repo root so clangd finds it without per-preset config.
# Writes to compile_commands.json in build dir; symlink at root points to
# the most recently configured preset.
if(CMAKE_EXPORT_COMPILE_COMMANDS)
    set(_root_cc_json "${CMAKE_SOURCE_DIR}/compile_commands.json")
    set(_build_cc_json "${CMAKE_BINARY_DIR}/compile_commands.json")
    if(EXISTS "${_build_cc_json}")
        if(NOT EXISTS "${_root_cc_json}" OR
           NOT IS_SYMLINK "${_root_cc_json}")
            file(CREATE_LINK "${_build_cc_json}" "${_root_cc_json}" SYMBOLIC)
            message(STATUS "Symlinked ${_root_cc_json} -> ${_build_cc_json}")
        endif()
    endif()
endif()

# -----------------------------------------------------------------------------
# Profile selection (per ADR-0005)
# -----------------------------------------------------------------------------
set(TIANSHU_PROFILE "desktop" CACHE STRING "Target platform profile")
set_property(CACHE TIANSHU_PROFILE PROPERTY STRINGS
    desktop server vehicle embedded mcu)

add_compile_definitions(TIANSHU_PROFILE_${TIANSHU_PROFILE})
message(STATUS "TIANSHU profile: ${TIANSHU_PROFILE}")

# -----------------------------------------------------------------------------
# Feature flags (per ADR-0008 message format, ADR-0016 config format, etc.)
# -----------------------------------------------------------------------------
option(TIANSHU_WITH_PROTOBUF "Enable Protobuf message format" ON)
option(TIANSHU_WITH_FLATBUFFERS "Enable FlatBuffers message format" ON)
option(TIANSHU_WITH_POD "Enable POD message format (always recommended)" ON)
option(TIANSHU_WITH_ZENOH "Enable Zenoh cross-machine transport" ON)
option(TIANSHU_WITH_TOML "Enable TOML config parser" ON)
option(TIANSHU_WITH_YAML "Enable YAML config parser" ON)
option(TIANSHU_WITH_JSON "Enable JSON config parser" ON)

# MCu profile forces some flags OFF
if(TIANSHU_PROFILE STREQUAL "mcu")
    set(TIANSHU_WITH_PROTOBUF OFF CACHE BOOL "MCU: Protobuf disabled" FORCE)
    set(TIANSHU_WITH_FLATBUFFERS OFF CACHE BOOL "MCU: FlatBuffers disabled" FORCE)
    set(TIANSHU_WITH_ZENOH OFF CACHE BOOL "MCU: full Zenoh disabled (use pico)" FORCE)
    set(TIANSHU_WITH_TOML OFF CACHE BOOL "MCU: no filesystem" FORCE)
    set(TIANSHU_WITH_YAML OFF CACHE BOOL "MCU: no filesystem" FORCE)
    set(TIANSHU_WITH_JSON OFF CACHE BOOL "MCU: no filesystem" FORCE)
endif()

# Propagate as compile definitions
foreach(_flag
        PROTOBUF FLATBUFFERS POD ZENOH TOML YAML JSON)
    if(TIANSHU_WITH_${_flag})
        add_compile_definitions(TIANSHU_WITH_${_flag}=1)
    else()
        add_compile_definitions(TIANSHU_WITH_${_flag}=0)
    endif()
endforeach()

# -----------------------------------------------------------------------------
# Warning level (per ADR-0005 lightweight + strict)
# -----------------------------------------------------------------------------
add_compile_options(
    -Wall
    -Wextra
    -Wpedantic
    -Werror
    -Wno-unused-parameter
    -Wno-deprecated-declarations
    -Wshadow
    -Wnon-virtual-dtor
    -Woverloaded-virtual
    -Wconversion
    -Wsign-conversion
    -Wold-style-cast
    -Wnull-dereference
)

# -----------------------------------------------------------------------------
# Sanitizer options (per ADR-0005 / INFRA-CI-3)
# -----------------------------------------------------------------------------
option(TIANSHU_ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(TIANSHU_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)
option(TIANSHU_ENABLE_TSAN "Enable ThreadSanitizer" OFF)

if(TIANSHU_ENABLE_ASAN)
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address)
endif()
if(TIANSHU_ENABLE_UBSAN)
    add_compile_options(-fsanitize=undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=undefined)
endif()
if(TIANSHU_ENABLE_TSAN)
    add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
    add_link_options(-fsanitize=thread)
endif()

# MCU profile: -ffreestanding -fno-exceptions -fno-rtti
if(TIANSHU_PROFILE STREQUAL "mcu")
    add_compile_options(
        -ffreestanding
        -fno-exceptions
        -fno-rtti
        -fno-threadsafe-statics
        -Os
    )
endif()

# -----------------------------------------------------------------------------
# Testing
# -----------------------------------------------------------------------------
enable_testing()

option(TIANSHU_BUILD_TESTS "Build unit tests (requires GoogleTest)" ON)
if(TIANSHU_BUILD_TESTS)
    include(FetchContent)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG v1.17.0
    )
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
    include(GoogleTest)
endif()

option(TIANSHU_BUILD_BENCHMARKS "Build benchmarks (requires GoogleBenchmark)" ON)
if(TIANSHU_BUILD_BENCHMARKS)
    if(NOT googletest_SOURCE_DIR)
        include(FetchContent)
    endif()
    FetchContent_Declare(
        googlebenchmark
        GIT_REPOSITORY https://github.com/google/benchmark.git
        GIT_TAG v1.9.0
    )
    set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googlebenchmark)
endif()

# Third-party libraries should not be held to our -Werror standard.
foreach(_target benchmark gtest gtest_main gmock gmock_main)
    if(TARGET ${_target})
        target_compile_options(${_target} PRIVATE -Wno-error)
    endif()
endforeach()

# -----------------------------------------------------------------------------
# Subdirectories
# -----------------------------------------------------------------------------
add_subdirectory(tianshu)
add_subdirectory(examples)
add_subdirectory(tests)
if(TIANSHU_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()

# -----------------------------------------------------------------------------
# Install rules (Phase 2+: pkg-config and find_package)
# -----------------------------------------------------------------------------
# install(EXPORT tianshuTargets ...)

# -----------------------------------------------------------------------------
# Configuration summary
# -----------------------------------------------------------------------------
message(STATUS "")
message(STATUS "==== TIANSHU Configuration Summary ====")
message(STATUS "  Version:        ${TIANSHU_VERSION}")
message(STATUS "  Profile:        ${TIANSHU_PROFILE}")
message(STATUS "  Build type:     ${CMAKE_BUILD_TYPE}")
message(STATUS "  C++ standard:   ${CMAKE_CXX_STANDARD}")
message(STATUS "")
message(STATUS "  Feature flags:")
message(STATUS "    Protobuf:     ${TIANSHU_WITH_PROTOBUF}")
message(STATUS "    FlatBuffers:  ${TIANSHU_WITH_FLATBUFFERS}")
message(STATUS "    POD:          ${TIANSHU_WITH_POD}")
message(STATUS "    Zenoh:        ${TIANSHU_WITH_ZENOH}")
message(STATUS "    TOML:         ${TIANSHU_WITH_TOML}")
message(STATUS "    YAML:         ${TIANSHU_WITH_YAML}")
message(STATUS "    JSON:         ${TIANSHU_WITH_JSON}")
message(STATUS "")
message(STATUS "  Sanitizers:")
message(STATUS "    ASan:         ${TIANSHU_ENABLE_ASAN}")
message(STATUS "    UBSan:        ${TIANSHU_ENABLE_UBSAN}")
message(STATUS "    TSan:         ${TIANSHU_ENABLE_TSAN}")
message(STATUS "========================================")
message(STATUS "")
