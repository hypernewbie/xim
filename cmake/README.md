# CMake build and validation

Xim uses out-of-tree CMake/Ninja builds with Clang and Clang++. CMake 3.25 or
later is required. Configure, build, and run the registered unit tests with:

```sh
cmake --preset default
cmake --build --preset default --parallel
ctest --preset default
```

The default preset uses Huge features and RelWithDebInfo. CMake checks the
host's dependencies, requires Clang's C++26 mode, and uses libc++ when its
compile, link, and run probe succeeds. Inherited `.c` files remain C sources.

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

## Performance comparison

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
latency for process startup/exit, first buffer paint, scrolling to the last
line, and a visible insert. It generates its fixture in a temporary directory,
uses a clean Vim configuration in a 24-by-100 `xterm-256color` PTY, and prints
JSON results. Confirm both binaries report the same feature flags before you
compare them. The script does not flush the filesystem cache; compare runs made
on the same host under similar system load. Treat a median regression greater
than 10% and at least 0.25 ms in two independent runs as a signal to investigate,
not as an isolated-run pass/fail gate. Use the 90th percentile to watch tail
latency separately.
