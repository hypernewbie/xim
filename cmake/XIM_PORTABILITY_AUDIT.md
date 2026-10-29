# Native project portability audit

Status: Linux and macOS release candidates pass native builds, tests and
installed workflows. This audit covers the native project services and runtime
lookup, not every Xim feature. It does not claim Windows support. The tested
targets are Ubuntu 24.04 x86-64 and Intel macOS 15.

## Reuse of Vim facilities

Xim uses the inherited Vim engine for buffers, edits, undo, rendering,
syntax and script execution. The native controller calls the editor on its
owning thread. The workers publish owned data instead of accessing editor
buffers or Vim globals.

Runtime discovery now uses Vim's `set_progpath()` and `mch_can_exe()` before
initial runtimepath setup. The controller resolves symlinks and selects the
build or installed runtime. Vim's `vim_setenv()` sets the environment. The
native controller no longer reads `/proc/self/exe` directly. The Linux PTY
suite tests a PATH symlink, custom `argv[0]` and an explicit override.
Linux and macOS runtime discovery passed native installed-build tests. Windows
still needs an installed-build test.

Vim provides file-change checks through `check_timestamps()` and
`FileChangedShell`. These run as editor operations. Xim's watcher adds
background project notifications and clean-buffer staged reads. The watcher
also updates project indexes and explorer pages. Reuse of Vim's change checks
requires tests for background progress, hooks, reload ownership and undo.
Do not assume that replacing the watcher with `:checktime` preserves them.

Vim also provides `vim_rename()` and platform `mch_rename()` functions.
The `vim_rename()` implementation can remove the destination before renaming
and can fall back to copy and delete. It also reads editor options. It is not
a drop-in worker API for Xim's no-replace, no-cross-filesystem-copy contract.
A prior destination check alone cannot prevent a concurrent destination create.
Reuse is appropriate only after an API preserves these properties and the
worker ownership boundary.

## Changes and remaining work

- Linux uses inotify. macOS uses FSEvents. Both backends report event loss and
  allow project reconciliation. Darwin path updates complete before the watch
  API returns, so edits made just after watch startup are not missed. Windows
  still needs a watcher.
- Atomic move uses Linux `renameat2(RENAME_NOREPLACE)` and Darwin
  `renameatx_np(RENAME_EXCL)`. Both use anchored directory descriptors.
  The tests create a destination after preflight and check both file contents.
  The Darwin operation and file-identity tests pass on a macOS runner.
- The watcher and project worker share one Unix file-version calculation.
  Linux and Darwin use their respective nanosecond timestamp fields. A fixed
  vector and nanosecond test guard the boundary. A Windows handle-identity
  helper and its isolated tests now exist, but the project and watcher workers
  do not yet use it.
- CMake now selects Vim's inherited Windows console engine with clang-cl and
  the Windows ABI. The native Xim target still needs Windows implementations
  for its watcher, filesystem operations, project scanner, recents and input
  wake. Its PTY test harness also needs a Windows equivalent.
- The inherited macOS and Windows CI workflows build Vim, not native Xim. The
  separate `xim-platform-probe.yml` tests Ubuntu and macOS Xim builds and
  packages. Windows remains deferred.

## Evidence and limits

GitHub Actions run `40825896789` passed for Ubuntu and macOS. Ubuntu 24.04
x86-64 passed all 8 native tests. The extracted package passed its PTY checks
on the runner and in a clean Ubuntu 24.04 container. The container installed
the listed runtime libraries and Python. The test confirmed `xim --version`,
runtime lookup, editing, project operations and Vim compatibility. The
SHA-256 check passed.

Intel macOS 15 passed all 8 native tests and both isolated file-stat and
filesystem-operation tests. The extracted package passed the same PTY workflow
and checksum check. `otool -L` found only macOS system libraries and the
bundled Pixman library; neither binary links to Homebrew. Apple has not
notarized this package.

The candidate artifacts are available from this Actions run:

- [Ubuntu 24.04 x86-64](https://github.com/hypernewbie/xim/actions/runs/40825896789/artifacts/12877036603)
- [Intel macOS 15](https://github.com/hypernewbie/xim/actions/runs/40825896789/artifacts/12876878225)

These are Actions artifacts, not a published GitHub Release. They do not
include a Windows build. Windows file identity, native commands and menus have
isolated passing tests, but its full native build still fails on POSIX-only
project, filesystem, recents, watcher and input-wake code.

Two serial 30-sample Linux comparisons measured completed quick-open and first
accepted edit on a 100000-path fixture. Baseline quick-open medians were 5.012
and 4.862 ms. Candidate medians were 4.951 and 4.939 ms. Baseline first-edit
medians were 20.727 and 20.631 ms. Candidate medians were 20.387 and 20.324 ms.
These measurements predate the final null guard. They do not establish macOS
performance or the project's 20x target.
