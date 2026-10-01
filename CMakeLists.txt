# Top-level CMake project for Xim.
#
# This file establishes the out-of-tree Clang/Ninja build foundation for Xim.
#
# Plan 0 Step 1: build the inherited editor out of tree with Clang and Ninja.
# VimFeatures.cmake and VimConfigChecks.cmake select the feature level and
# optional dependencies while retaining the default reference configuration.

cmake_minimum_required(VERSION 3.25)

# Enable CTest. Without this, add_test() calls in src/CMakeLists.txt are
# ignored and no CTestTestfile.cmake is generated.
enable_testing()

project(xim
    VERSION 9.2.0
    DESCRIPTION "Xim — terminal editor forked from Vim"
    LANGUAGES C)

# AGENTS.md mandates Clang. Refuse any other compiler at configure time so
# the policy cannot be bypassed by an IDE or environment variable.
if(NOT CMAKE_C_COMPILER_ID STREQUAL "Clang")
    message(FATAL_ERROR
        "Xim requires Clang. Detected: ${CMAKE_C_COMPILER_ID} (${CMAKE_C_COMPILER}).")
endif()

message(STATUS "Xim ${PROJECT_VERSION} — Clang ${CMAKE_C_COMPILER_VERSION}")

# Helper modules. Each one owns a narrow concern so src/CMakeLists.txt stays
# readable. Adding new modules here must not affect vendor directories.
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(VimCompilerFlags)
include(VimGenerated)
include(VimConfigChecks)
include(VimFeatures)

add_subdirectory(src)
