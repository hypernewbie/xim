# Native project portability audit

Status: partial bring-up. This audit covers the native project services and
runtime lookup, not every Xim feature. It does not claim macOS or Windows
support. The full native build and installed workflow must pass on each target.

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
Windows and macOS runtime discovery still need native installed-build tests.

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

- Linux uses an inotify watcher. A Darwin watcher and a Windows watcher are
  not implemented. They must report event loss and allow reconciliation.
  Silent polling or disabling the watcher is not an equivalent release.
- Atomic move uses Linux `renameat2(RENAME_NOREPLACE)` and Darwin
  `renameatx_np(RENAME_EXCL)`. Both use anchored directory descriptors.
  The tests create a destination after preflight and check both file contents.
  The Darwin operation and file-identity tests pass on a macOS runner.
- The watcher and project worker now share one file-version calculation.
  Linux and Darwin use their respective nanosecond timestamp fields. A fixed
  vector and nanosecond test guard the boundary. Windows needs an identity
  adapter with equivalent change detection.
- The CMake source selection still assumes Unix. Windows needs a real CMake
  target for the inherited Windows engine, clang-cl and the native adapters.
  Its input wake, file paths, operations and test harness also need work.
- The existing macOS and Windows CI workflows build inherited Vim, not the
  native Xim target. The separate `xim-platform-probe.yml` runs Xim checks.
  Its macOS and Windows jobs remain red until native builds and tests pass.

## Evidence and limits

GitHub Actions run `40780957694` passed the Ubuntu native and extracted
archive tests, including a clean Ubuntu container. On macOS it passed the
independent file-operation and file-identity tests. The full macOS build still
failed on the inotify source. The Windows configuration still failed on the
Unix terminal-library requirement. The Linux development run passed all 23
registered tests across the two temporary-path groups. A clean tracked-source
export passed 14/14. The native ASan/UBSan and TSan suites each passed 8/8.
The selected builds produced no new warnings.

Two serial 30-sample Linux comparisons measured completed quick-open and first
accepted edit on a 100000-path fixture. Baseline quick-open medians were 5.012
and 4.862 ms. Candidate medians were 4.951 and 4.939 ms. Baseline first-edit
medians were 20.727 and 20.631 ms. Candidate medians were 20.387 and 20.324 ms.
These measurements precede the final null guard. They do not establish macOS
or Windows performance, or the project's 20x target.

The macOS and Windows ports require native regression tests, clean tracked
builds, installed workflows and performance checks before release. The Ubuntu
archive in Actions is a candidate, not a published release.
