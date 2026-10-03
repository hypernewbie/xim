# src/testdir/CMakeLists.txt — drive the inherited testdir Vim-script suite
# through CTest. Tests are gated behind BUILD_FULL_TEST=ON because the full
# suite takes several minutes.
#
# Layout:
#   vim_smoke:      runs test_assert.vim — fast single-file sanity check
#   vim_scripts_tiny: run legacy SCRIPTS_TINY_OUT targets
#   vim_scripts_*:    run NEW_TESTS_RES from Make_all.mak in bounded batches
#
# The driver copies src/testdir into a private build-tree working directory
# before running. This keeps generated test artifacts out of the source tree
# and prevents concurrent CTest/Makefile runs from racing. xvfb-run is used
# because some tests assume an X server.

find_program(XVFB_RUN xvfb-run)
if(NOT XVFB_RUN)
    message(WARNING
        "xvfb-run not found — X11 client/server tests will fail. "
        "Install xvfb to enable them.")
endif()

if(TARGET xim)
    add_test(NAME xim_native_screens
        COMMAND ${CMAKE_COMMAND} -E env XIM_BINARY=$<TARGET_FILE:xim>
            ${CMAKE_SOURCE_DIR}/cmake/run_vim_tests.sh
            $<TARGET_FILE:vim> ${CMAKE_BINARY_DIR}/runtime
            ${CMAKE_CURRENT_SOURCE_DIR} test_xim_native
            ${CMAKE_BINARY_DIR}/Testing $<TARGET_FILE:xxd>
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/Testing)
    set_tests_properties(xim_native_screens PROPERTIES LABELS "xim_native" TIMEOUT 120)
endif()

# ----------------------------------------------------------------------------
# Smoke test — runs a representative single test file in seconds.
# ----------------------------------------------------------------------------
add_test(
    NAME vim_smoke
    COMMAND
        ${CMAKE_SOURCE_DIR}/cmake/run_vim_tests.sh
        $<TARGET_FILE:vim>
        ${CMAKE_SOURCE_DIR}/runtime
        ${CMAKE_CURRENT_SOURCE_DIR}
        test_assert
        ${CMAKE_BINARY_DIR}/Testing
        $<TARGET_FILE:xxd>
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/Testing)
set_tests_properties(vim_smoke PROPERTIES LABELS "vim_script" TIMEOUT 120)

# Legacy input/output test scripts are part of the inherited nongui target.
add_test(
    NAME vim_scripts_tiny
    COMMAND
        ${CMAKE_SOURCE_DIR}/cmake/run_vim_tests.sh
        $<TARGET_FILE:vim>
        ${CMAKE_SOURCE_DIR}/runtime
        ${CMAKE_CURRENT_SOURCE_DIR}
        --tiny
        ${CMAKE_BINARY_DIR}/Testing
        $<TARGET_FILE:xxd>
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/Testing)
set_tests_properties(vim_scripts_tiny PROPERTIES
    LABELS "vim_script"
    TIMEOUT 120)

# ----------------------------------------------------------------------------
# Full script suite — run the inherited NEW_TESTS_RES list in bounded
# alphabetic batches. Each batch has a 6-minute timeout. Requires
# BUILD_FULL_TEST=ON.
# ----------------------------------------------------------------------------
foreach(test_range IN ITEMS a-c d-h i-m n-s t u-z)
    string(REPLACE "-" "_" range_id "${test_range}")
    add_test(
        NAME vim_scripts_${range_id}
        COMMAND
            ${CMAKE_SOURCE_DIR}/cmake/run_vim_tests.sh
            $<TARGET_FILE:vim>
            ${CMAKE_SOURCE_DIR}/runtime
            ${CMAKE_CURRENT_SOURCE_DIR}
            --range=${test_range}
            ${CMAKE_BINARY_DIR}/Testing
            $<TARGET_FILE:xxd>
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/Testing)
    set_tests_properties(vim_scripts_${range_id} PROPERTIES
        LABELS "vim_script"
        TIMEOUT 360)
endforeach()
