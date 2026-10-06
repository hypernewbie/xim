# CMake build and validation

Xim uses out-of-tree CMake/Ninja builds with Clang and Clang++ 19 or newer.
CMake 3.25 or later and Python 3.10 or later are required. Configure, build,
and run the registered unit tests with:

```sh
cmake --preset default
cmake --build --preset default --parallel
ctest --preset default
```

The default preset uses Huge features and RelWithDebInfo. CMake checks the
host's dependencies, requires Clang's C++26 mode, and uses libc++ when its
compile, link, and run probe succeeds. Inherited `.c` files remain C sources.

For clean-source acceptance, run:

```sh
python3 cmake/check_clean_build.py --source . --work build/clean-check
```

This copies tracked working-tree sources, builds without inherited generated
headers, runs unit tests, and checks Ninja's header dependencies. The core and
its consumers share `-Wall` and the reference macro policy. `:version` reports
the selected configuration; `compile_commands.json` and
`ninja -C build/default -t commands vim` contain the actual commands.
Before new files are tracked, supply their relative paths or globs with
repeated `--include` arguments. Other untracked files are excluded.

Use short build and runtime paths for inherited screen/socket tests. Unix
socket paths have a platform length limit, and some command-line screen tests
assume that the expanded runtime path fits in their terminal width.

## Vim-script suite

Enable the inherited Vim-script tests and smoke test when configuring:

```sh
cmake --preset default -DBUILD_FULL_TEST=ON
cmake --build --preset default --parallel
ctest --preset default
```

CTest runs the suite from a private build-tree copy. It includes the eleven
legacy input/output tests and all 274 `NEW_TESTS_RES` targets, including the
Vim9 tests. The new-style tests are split into alphabetic batches, each with a
six-minute timeout. Tests use the CMake-built Vim, its matching runtime, and
the CMake-built `xxd`.

When GNU libtool is installed, CTest also runs libvterm's inherited test
harness from a private build-tree copy. This matches the reference build's
conditional libvterm test target.

The `debug`, `release`, `asan`, and `tsan` configure/build presets are also
available. For example:

```sh
cmake --preset asan
cmake --build --preset asan --parallel
ctest --preset asan
```

## Native editor

The build produces `build/default/src/xim` and stages its matching runtime
under `build/default/runtime`. Run `xim file.txt` from any working directory.
Use `xim --vim` for compatibility mode. Native startup loads configuration
only when explicitly requested with `-u`. See `runtime/doc/xim.txt` and
`cmake/XIM_INPUT.md` for the shortcuts and scripting boundary.

Run native correctness checks with Python 3.10 or newer:

```sh
ctest --test-dir build/default -L xim_native --output-on-failure
python3 cmake/benchmark_xim.py --native build/default/src/xim \
    --reference src/vim --work build/default/Testing/latency \
    --runs 60 --output temp/XIM_PLAN1_PERF.json
```

The native benchmark uses real Xim shortcuts, buffer-content assertions,
editor-assembled completion markers, and redraw before acknowledgment.
Each round alternates reference/native order. Command-UI entry compares the
native palette with Vim's Ex prompt; those are different available interfaces.
The Vim reference enables the same syntax defaults, status-area height and
exclusive selection. It loads no user plugins. Earlier minimal-reference
startup samples did not enable syntax and are not equivalent configurations.
See `cmake/PLAN0_CLOSEOUT.md` for the pre-implementation native budgets.
See `cmake/PLAN1_VALIDATION.md` for the implementation's test, theme and
performance evidence, including the recorded full-suite rerun.
`cmake/PLAN3_VALIDATION.md` covers background completion and incremental UI.
The PTY harness maintains an incremental screen so unchanged prompt prefixes
need not be re-emitted for a visible-text assertion.

For completed project-query and editing-during-scan measurements:

```sh
python3 cmake/benchmark_xim_project.py --binary build/dev/src/xim \
    --work build/plan3/project-perf --runs 60 \
    --output build/plan3/project-perf.json
```

This creates exactly 100000 indexed files in a private temporary project,
warms the owned path index, verifies both query text and selected result,
and measures project startup through a content-checked edit during scanning.
It also checks activation contents and idle output/CPU. Run serially, without
compilation or other benchmark workloads. `xim_project` checks matching,
result caps, query/root revisions, refresh and shutdown with owned fixtures.
For `benchmark_xim.py`, use `--self-test --runs 1` to check delayed completion.
Its `--startup-log PATH` records a native startup trace for the first sample. For an instrumented
Clang profiling build, use `-pg -DWE_ARE_PROFILING` for C and C++ and `-pg`
at link time. Supply `--profile-dir PATH` to preserve gprof data; quit is
orderly in that mode. Instrumented timings are diagnostic, not acceptance
numbers. Inspect them with `gprof -p BINARY PATH/*.gmon`.

## Compatibility performance comparison

Build the inherited reference with `make CC=clang` from `src/`, then build the
CMake candidate with the default preset. Compare both binaries with the same
runtime and workload. The benchmark script requires Python 3 and uses only its
standard library:

```sh
python3 cmake/benchmark_vim.py \
    --binary reference=src/vim \
    --binary cmake=build/default/src/vim \
    --runtime runtime \
    --runs 30 \
    --lines 20000
```

The script alternates binary order and reports median and 90th-percentile
latency for process startup/exit, first buffer paint, the first accepted edit,
scrolling to the last line, and a completed insert/redraw. It validates edited
buffer contents and uses markers assembled by the editor, which cannot appear
in echoed input. Interaction timers start before input is sent. Run
`--self-test --runs 1` to verify a one-second delayed completion. Earlier edit
measurements with echoed markers are invalid. It generates a temporary fixture,
uses a clean Vim configuration in a 24-by-100 `xterm-256color` PTY, and prints
JSON results. Confirm both binaries report the same feature flags before you
compare them. The script does not flush the filesystem cache; compare runs made
on the same host under similar system load. Treat a median regression greater
than 10% and at least 0.25 ms in two independent runs as a signal to investigate,
not as an isolated-run pass/fail gate. Use the 90th percentile to watch tail
latency separately.
