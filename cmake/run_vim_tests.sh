#!/bin/sh
# run_vim_tests.sh — driver for the src/testdir/ Vim-script test suite.
#
# Usage:
#   run_vim_tests.sh <vim_binary> <runtime_dir> <testdir_dir> <test_name>
#   run_vim_tests.sh <vim_binary> <runtime_dir> <testdir_dir> --all
#
# The Makefile-driven test runner uses testdir as its working directory
# because test_*.vim files and their helpers (util/setup.vim) reference
# each other via relative paths. We do the same and run tests serially.
#
# Per-test isolation would require duplicating the entire testdir tree per
# test; not worth the complexity while we keep the inherited sequential
# runner.
#
# xvfb-run is required because several test files assume an X display.
# Each test passes when it produces a `.res` file containing "ALL DONE"
# or when its `messages` log shows the right Executed: / Failed: counts.

set -eu

VIM="$1"
RUNTIME="$2"
TESTDIR="$3"
MODE="${4:-}"

if [ -z "$VIM" ] || [ -z "$RUNTIME" ] || [ -z "$TESTDIR" ]; then
    echo "usage: $0 <vim> <runtime> <testdir> <test|--all>" >&2
    exit 2
fi

cd "$TESTDIR"

# Inherited test runner convention: write the path of the vim binary to
# `vimcmd` so tests that spawn child vim instances (e.g. RunVimInTerminal)
# can find the right binary. Without this, they fall back to ../vim which
# is not present in the CMake build.
#
# The Makefile writes two lines: line 1 is VIMPROG, line 2 is the full
# command including any required environment variables. Tests that spawn a
# child vim use line 2 so the child inherits VIMRUNTIME.
{
    echo "$VIM"
    echo "VIMRUNTIME=$RUNTIME $VIM"
} > vimcmd

run_one() {
    name="$1"
    # Clean per-test artifacts so the previous run doesn't pollute the next.
    # The Makefile-driven runner uses the same pattern (.NOTPARALLEL).
    rm -f messages test.log test.out test.ok X* viminfo starttime
    rm -f "${name}.res" "${name}.failed"

    # The Makefile-driven runner uses --not-a-term + a synthetic terminal.
    # xvfb-run provides the X11 display that clientserver tests expect.
    VIMRUNTIME="$RUNTIME" xvfb-run -a "$VIM" \
        -f -u util/unix.vim \
        --gui-dialog-file guidialog \
        -U NONE --noplugin --not-a-term \
        -S runtest.vim "${name}.vim" \
        --cmd 'au SwapExists * let v:swapchoice = "e"' \
        >"${name}.out" 2>&1 \
        || true

    if [ -f "${name}.failed" ]; then
        echo "FAIL $name"
        if [ -f messages ]; then
            tail -50 messages >&2
        fi
        return 1
    fi

    # Inherited behavior: the test runner writes ${name}.res when all
    # assertions pass. Its absence plus no Executed: marker in messages
    # indicates the test runner itself failed (e.g. could not start the
    # test or syntax error in the script).
    if [ -f "${name}.res" ]; then
        if grep -q '^ALL DONE' messages 2>/dev/null; then
            echo "PASS $name"
            return 0
        fi
        # .res without ALL DONE is suspicious; treat as fail.
        echo "FAIL $name (.res without ALL DONE)"
        tail -30 messages >&2
        return 1
    fi

    echo "FAIL $name (no .res)"
    tail -30 "${name}.out" >&2
    return 1
}

if [ "$MODE" = "--all" ]; then
    fail=0
    total=0
    for test in $(ls test_*.vim | sed 's/\.vim$//' | sort); do
        total=$((total + 1))
        if ! run_one "$test"; then
            fail=$((fail + 1))
        fi
    done
    echo "Summary: $((total - fail)) passed, $fail failed out of $total"
    exit $fail
else
    run_one "$MODE"
fi