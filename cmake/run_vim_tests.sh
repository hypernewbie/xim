#!/bin/sh
# run_vim_tests.sh — driver for the src/testdir/ Vim-script test suite.
#
# Usage:
#   run_vim_tests.sh <vim_binary> <runtime_dir> <testdir_dir> <test_name> <work_root> [xxd_binary]
#   run_vim_tests.sh <vim_binary> <runtime_dir> <testdir_dir> --all <work_root> [xxd_binary]
#   run_vim_tests.sh <vim_binary> <runtime_dir> <testdir_dir> --tiny <work_root> [xxd_binary]
#   run_vim_tests.sh <vim_binary> <runtime_dir> <testdir_dir> --range=a-c <work_root> [xxd_binary]
#   run_vim_tests.sh <vim_binary> <runtime_dir> <testdir_dir> --range=t <work_root> [xxd_binary]
#   run_vim_tests.sh <vim_binary> <runtime_dir> <testdir_dir> --range=u-z <work_root> [xxd_binary]
#
# The Makefile-driven test runner uses testdir as its working directory
# because test_*.vim files and their helpers (util/setup.vim) reference
# each other via relative paths. Copy the inputs into the CMake build tree
# first so tests do not modify or race with src/testdir.
#
# xvfb-run is required because several test files assume an X display.
# A test passes when runtest.vim creates its `.res` file without creating
# `test.log` or `${test}.failed`.

set -eu

VIM="$1"
RUNTIME="$2"
TESTDIR="$3"
MODE="${4:-}"
BUILD_TEST_ROOT="${5:-}"
XXD="${6:-}"

if [ -z "$VIM" ] || [ -z "$RUNTIME" ] || [ -z "$TESTDIR" ] || [ -z "$BUILD_TEST_ROOT" ]; then
    echo "usage: $0 <vim> <runtime> <testdir> <test|--all> <work_root> [xxd]" >&2
    exit 2
fi

case "$MODE" in
    --all) WORK_NAME=all ;;
    --tiny) WORK_NAME=tiny ;;
    --range=a-c) WORK_NAME=range_a_c ;;
    --range=d-h) WORK_NAME=range_d_h ;;
    --range=i-m) WORK_NAME=range_i_m ;;
    --range=n-s) WORK_NAME=range_n_s ;;
    --range=t) WORK_NAME=range_t ;;
    --range=u-z) WORK_NAME=range_u_z ;;
    ''|*[!A-Za-z0-9_]*)
        echo "invalid test name: $MODE" >&2
        exit 2
        ;;
    *) WORK_NAME="$MODE" ;;
esac

WORK_ROOT="$BUILD_TEST_ROOT/$WORK_NAME"
SOURCE_SRC=$(CDPATH= cd "$TESTDIR/.." && pwd)
mkdir -p "$WORK_ROOT/src"
for source_entry in "$SOURCE_SRC"/*; do
    source_name=${source_entry##*/}
    case "$source_name" in
        runtime|testdir|vim|xxd) continue ;;
    esac
    ln -sfn "$source_entry" "$WORK_ROOT/src/$source_name"
done
ln -sfn "$VIM" "$WORK_ROOT/src/vim"
ln -sfn "$RUNTIME" "$WORK_ROOT/runtime"
ln -sfn "$RUNTIME" "$WORK_ROOT/src/runtime"
if [ -n "$XXD" ]; then
    mkdir -p "$WORK_ROOT/src/xxd"
    ln -sfn "$XXD" "$WORK_ROOT/src/xxd/xxd"
fi

WORKDIR="$WORK_ROOT/src/testdir"
rm -rf "$WORKDIR"
mkdir -p "$WORKDIR"
cp -a "$TESTDIR"/. "$WORKDIR"/
cd "$WORKDIR"

set --
case "$MODE" in
    --all|--tiny|--range=*)
    test_vim9_list=
    tiny_tests=
    script_tests=
    list_section=
    while IFS= read -r list_line; do
        case "$list_line" in
            "SCRIPTS_TINY_OUT ="*) list_section=tiny; continue ;;
            "TEST_VIM9_RES ="*) list_section=vim9; continue ;;
            "NEW_TESTS_RES ="*) list_section=new; continue ;;
        esac
        [ -n "$list_section" ] || continue
        if [ -z "$list_line" ]; then
            list_section=
            continue
        fi
        case "$list_line" in
            *'$(TEST_VIM9_RES)'*)
                script_tests="$script_tests $test_vim9_list"
                continue
                ;;
        esac
        case "$list_section" in
            tiny)
                list_item=$(printf '%s\n' "$list_line" | sed -n 's/^[[:space:]]*\(test[[:alnum:]_]*\)\.out.*/\1/p')
                [ -n "$list_item" ] && tiny_tests="$tiny_tests $list_item"
                ;;
            vim9|new)
                list_item=$(printf '%s\n' "$list_line" | sed -n 's/^[[:space:]]*\(test_[[:alnum:]_]*\)\.res.*/\1/p')
                [ -n "$list_item" ] || continue
                if [ "$list_section" = vim9 ]; then
                    test_vim9_list="$test_vim9_list $list_item"
                else
                    script_tests="$script_tests $list_item"
                fi
                ;;
        esac
    done < "$TESTDIR/Make_all.mak"

    total=0
    if [ "$MODE" = --tiny ] || [ "$MODE" = --all ]; then
        for test in $tiny_tests; do
            total=$((total + 1))
            set -- "$@" "$test.out"
        done
    fi
    if [ "$MODE" != --tiny ]; then
        for test in $script_tests; do
            case "$MODE:$test" in
                --all:*) ;;
                --range=a-c:test_[a-c]*) ;;
                --range=d-h:test_[d-h]*) ;;
                --range=i-m:test_[i-m]*) ;;
                --range=n-s:test_[n-s]*) ;;
                --range=t:test_t*) ;;
                --range=u-z:test_[u-z]*) ;;
                *) continue ;;
            esac
            total=$((total + 1))
            set -- "$@" "$test.res"
        done
    fi
    ;;
    *)
    total=1
    set -- "$MODE.res"
    ;;
esac

if [ "$total" -eq 0 ]; then
    echo "no test targets selected for $MODE" >&2
    exit 2
fi

# Run the inherited Makefile rules in the private workspace. This preserves
# the reference test invocation, including vimcmd generation and output
# filtering, while keeping all generated files out of the source tree.
make --no-print-directory -f Makefile clean

run_status=0
timeout -k 5s 350s xvfb-run -a make --no-print-directory -f Makefile \
    "$@" \
    "VIMPROG=$VIM" \
    "XXDPROG=../xxd/xxd" \
    "SCRIPTSOURCE=$RUNTIME" \
    || run_status=$?

passed=0
for target in "$@"; do
    if [ -f "$target" ]; then
        passed=$((passed + 1))
    fi
done
failed=$((total - passed))
echo "Summary: $passed passed, $failed failed out of $total"

if [ "$run_status" -ne 0 ] || [ -f test.log ]; then
    if [ -f test.log ]; then
        echo "Inherited test failures are recorded in $WORKDIR/test.log" >&2
    fi
    exit 1
fi
