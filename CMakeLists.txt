# Top-level CMake project for Xim.
#
# This file establishes the modern build foundation for Xim, replacing the
# inherited autotools/Makefile build described in temp/XIM_PLAN0.md.
#
# Plan 0 Step 1 Phase 1 (this phase):
#   - Clang-enforced out-of-tree Ninja build
#   - Builds the `xxd` subproject only
#   - Generates compile_commands.json
#
# Subsequent phases (see the plan file) will add the `vim` target, generated
# file handling, CTest integration, feature configuration, and finally replace
# the autoconf-generated `auto/config.h`.

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

# Phase 1 builds only xxd. Phase 2 replaces this stub with the full vim
# target. Phase 4 adds CTest integration.
add_subdirectory(src)