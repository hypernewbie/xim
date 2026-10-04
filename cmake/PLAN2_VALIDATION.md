# Plan 2 local validation — 2026-10-04

Source: `4eb65cefc`, the Plan 1 review closeout plus the Plan 2 navigation
slice and its follow-up. Clang/Clang++ 22.1.8, libc++, C++26, CMake 4.3.4,
Ninja 1.13.2, Linux UTF-8. The acceptance configuration is `build/dev`,
matching Plan 1.

## Scope of this slice

`xim .` treats a directory argument as a project root. Ctrl-P opens quick
open, Ctrl-E the explorer, Ctrl-Shift-B the buffer picker, and Ctrl-Shift-P
the command palette. F1 and F2 remain palette fallbacks because terminals
usually cannot distinguish Ctrl-Shift-P.

## Implementation

`src/xim_project.cpp` owns the project index. Enumeration runs on one
bounded background job and writes only owned path data; the editor thread
never shares Vim globals with it. A generation counter rejects a scan from
a superseded root. The index is bounded to 100000 paths, quick open to 200
results, and the explorer to 500 rows. `.git`, `.hg`, `.svn`, `build` and
`node_modules` are pruned during the walk. Ranking is exact basename, then
basename prefix, then path substring, compared case-insensitive.

### `.gitignore` honour

The walker reads `<dir>/.gitignore` on entry, parses lines, and stores them
per directory in `rules_by_dir`. Rules support `!` negation, trailing `/`
for directory-only matching, and a leading `/` for path-anchored patterns
(path-anchored slash patterns are skipped, recorded as a limitation).
Wildcards `*` and `?` match a single path component. The rule walk visits
the map in path order (root first, then descendants) so an inner
`!keep.log` can un-ignore a path that an outer `*.log` excluded — last
matching rule wins.

### Explicit path support

Quick open's filesystem fallback recognises any query that contains `/` or
starts with `/` or `~`, runs `weakly_canonical` on the input, and returns the
result as either the project-relative form (when the file is inside the
project root) or an out-of-root `> /abs/path` marker that the controller
hands back to `:edit` verbatim. The picker no longer waits on the index for
an explicit path; the index and the direct filesystem check run side by
side and the explicit path wins because it sorts first in the result list.

### Single-file invocation skips the walk

`xim_initialize()` only calls `xim_project_init(path)` when the argument is
a directory; a file argument leaves the project root undefined. Explicit
paths still work in that mode because the picker falls back to the direct
filesystem check.

### Refresh preserves expansion

`start_scan()` leaves `expanded_directories` untouched. A `:edit!` on the
same buffer re-enters the walker; the explorer's expanded state from the
previous render survives because the registry is only reset when the root
changes.

### Symlink cycle and root escape

`std::filesystem::recursive_directory_iterator` does not follow directory
symlinks by default. Regression coverage in `Test_xim_project_symlinks`
exercises a self-link, a cycle, and an out-of-tree escape; none appear in
the index or the explorer.

### Duplicate basenames

The picker already lists the full project-relative path, so `a/foo.cpp`
and `b/foo.cpp` appear as distinct rows even when their basenames match.

## netrw disabling

Filetype detection launches `netrw` on a directory buffer, which then
intercepts Ctrl-P / Ctrl-E before xim's picker can render the overlay. The
picker tests were silently passing on netrw because the dumps compared
against the directory listing rather than the overlay. `xim_initialize`
sets `g:loaded_netrw = 1` before vim's filetype machinery runs, so a
directory argument opens an empty buffer and xim's keystrokes reach the
picker.

## Indexing does not block first edit

The first implementation scanned synchronously inside `xim_initialize()`.
On this repository (429860 files) that pushed first paint past 10 s, so the
process never reached the editor. That failed the plan requirement that
index work must not block first edit.

After moving the walk to a background job, median first paint over seven
runs:

| Root | Files | First paint |
| --- | ---: | ---: |
| empty directory | 0 | 16.4 ms |
| synthetic tree | 1000 | 16.8 ms |
| this repository | 429860 | 16.9 ms |

Startup is flat across project size.

## Correctness

- Full `build/dev` suite: all 9 CTest registrations passed in a serial run
  (the eight Vim-script batches plus the native PTY suite). The native
  PTY suite ran 12/12 in 4.4 s; every Plan 1 dump plus the two Plan 2
  dumps still reproduce byte-for-byte.
- ASan/UBSan and TSan native command/PTY tests passed. The TSan run
  covers the background index and the editor thread's snapshot reads.
- No new compiler warnings. `git diff --check` passes.
- New PTY coverage asserts:
  - `Test_xim_project_gitignore` — `*.log` at the root, `!keep.log` in
    the inner directory, directory-only rules, kept text shown, ignored
    logs hidden.
  - `Test_xim_project_symlinks` — self-link, cycle, escape, and a regular
    file symlink all behave correctly.
  - `Test_xim_project_explicit_path` — an absolute path opens the file
    directly without waiting for the index.
  - `Test_xim_project_single_file_no_scan` — file-only invocation does
    not start a directory walk; out-of-root absolute paths still open.
  - `Test_xim_project_refresh_preserves_expansion` — expansion state
    survives `:edit!` refresh.
  - `Test_xim_project_duplicate_basenames` — two files with the same
    basename appear as distinct rows.
- Existing screen dumps `Test_xim_status`, `Test_xim_selection`,
  `Test_xim_open`, `Test_xim_find`, `Test_xim_palette`,
  `Test_xim_unsaved`, `Test_xim_theme_*`, `Test_xim_palette_scroll`,
  `Test_xim_failed_save`, and `Test_xim_failed_unnamed_save` all
  reproduce byte-for-byte.

### Known limitations

- The overlay prompt line renders a trailing `>` from the first item.
  This is the cursor visualisation that vim places on the prompt row;
  the same artifact appears in the palette dumps. Recorded, not fixed.
- Anchored path rules in `.gitignore` (a leading `/` followed by a `/`)
  are skipped. Single-component anchored rules still match.
- Results refresh on the next render after a scan completes. There is no
  explicit wake of the input wait, so a result may appear on the next
  keypress. Waking the input wait is the next slice.

## Calibrated performance

60 samples per binary, alternating order, 20000-line fixture, 24x100
xterm-256color PTY, matching runtime contents. Values in milliseconds,
against the Plan 1 acceptance round:

| Phase | Xim median | Xim p90 | Reference median | Xim p90 budget |
| --- | ---: | ---: | ---: | ---: |
| First paint | 23.454 | 24.786 | 26.627 | — |
| Launch to first accepted edit | 23.809 | 25.173 | 26.951 | 30 |
| Typing | 0.427 | 0.470 | 0.443 | 5 |
| Selection | 0.312 | 0.350 | 0.311 | 5 |
| Page scroll | 0.276 | 0.326 | 0.334 | 5 |
| 10 KiB paste | 12.436 | 12.811 | 199.725 | 20 |
| Command UI | 0.335 | 0.401 | 0.221 | 5 |

Every phase stays inside the budget. Startup moved from 22.790 to 23.454 ms
median, consistent with spawning one background job. Paste is unchanged.

The one-second completion self-test reports 1023.6 ms for native first
accepted edit and 1026.3 ms for the reference, rejecting echoed input as
completion.

Raw evidence (local, gitignored):

- `temp/XIM_PLAN2_PERF.json`
- `temp/XIM_PLAN2_SELFTEST.json`
- `temp/XIM_PLAN2_STARTUP.log`