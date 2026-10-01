# src/testdir/CMakeLists.txt — drive the inherited testdir Vim-script suite
# through CTest. Tests are gated behind BUILD_FULL_TEST=ON because the full
# suite takes several minutes.
#
# Layout:
#   vim_smoke:      runs test_normal.vim — fast single-file sanity check
#   vim_scripts:    runs every test_*.vim via cmake/run_vim_tests.sh --all
#
# Each test gets its own working directory under the build's Testing/
# directory so per-test files (test.out, test.ok, X*, viminfo) don't race
# across runs. xvfb-run is used because some tests assume an X server.

find_program(XVFB_RUN xvfb-run)
if(NOT XVFB_RUN)
    message(WARNING
        "xvfb-run not found — X11 client/server tests will fail. "
        "Install xvfb to enable them.")
endif()

# ----------------------------------------------------------------------------
# Smoke test — runs a representative single test file in seconds.
# ----------------------------------------------------------------------------
add_test(
    NAME vim_smoke
    COMMAND
        ${CMAKE_SOURCE_DIR}/cmake/run_vim_tests.sh
        $<TARGET_FILE:vim>
        ${CMAKE_SOURCE_DIR}/../runtime
        ${CMAKE_CURRENT_SOURCE_DIR}
        test_normal
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/Testing)
set_tests_properties(vim_smoke PROPERTIES LABELS "vim_script")

# ----------------------------------------------------------------------------
# Full script suite — slow (~5-10 min). Drives runtest.vim against every
# test_*.vim file in this directory under xvfb-run. Requires BUILD_FULL_TEST=ON.
# ----------------------------------------------------------------------------
add_test(
    NAME vim_scripts
    COMMAND
        ${CMAKE_SOURCE_DIR}/cmake/run_vim_tests.sh
        $<TARGET_FILE:vim>
        ${CMAKE_SOURCE_DIR}/../runtime
        ${CMAKE_CURRENT_SOURCE_DIR}
        --all
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/Testing)
set_tests_properties(vim_scripts PROPERTIES LABELS "vim_script" TIMEOUT 1800)