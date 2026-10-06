# Plan 2 local validation — 2026-10-04

Source: `4eb65cefc` plus the review-closeout fixes (the working tree at the
time of this section; see `git log` for the closeout commits).
Clang/Clang++ 22.1.8, libc++, C++26, CMake 4.3.4,
Ninja 1.13.2, Linux UTF-8. The acceptance configuration is `build/dev`,
matching Plan 1.

## Review closeout — 2026-10-04

An independent review round (`temp/XIM_PLAN2_REVIEW.md`) reproduced six
defects beyond the two declared deferrals. All six are fixed in this
round:

1. **Empty-candidate crash.** Typing a path-like query (for example a
   bare `/`) sorted from `candidates.begin() + 1` even when no explicit
   candidate existed, reading past an empty vector's end and killing the
   editor. The explicit candidate is now tracked by a flag and only a
   valid range is sorted. Ranking is computed once per candidate into a
   rank array instead of re-ranking inside the sort comparator.
2. **Explorer followed directory symlinks.** `collect_listing` used
   status-based `is_directory()` and `filesystem::relative()` (which
   resolves symlinks), so a self-link rendered as `+ ./` and expanding it
   recursed until the stack gave out, and a link to a sibling directory
   listed files outside the root. Both listing passes now use
   `lexically_relative()` and directory symlinks are never listed or
   expanded. A file symlink is indexed under its in-tree name and opens
   its target.
3. **Ignore rules matched the wrong scope.** Rules were applied to the
   full project-relative path, so root `/cache/` also hid `src/cache/`,
   and a rule in `a/.gitignore` named `a` hid `a/keep.txt` (git keeps
   both files). Rules now apply to the path relative to the directory
   that owns the `.gitignore`, and an anchored single-component rule
   matches exactly one component deep. Differential fixtures against
   `git check-ignore` agree with the walker on both cases.
4. **File-only start still scanned.** `xim_project_init(NULL)` selected
   the working directory and started a walk, so `xim hello.txt` indexed
   every sibling. A file-only invocation now calls
   `xim_project_disable()`: no root, no walk, empty status; explicit
   paths and the buffer picker still work. A bare start keeps the
   working directory as the project root.
5. **Arrows only moved the palette selection.** Up/Down now move the
   selection in every list prompt (palette, quick open, explorer,
   buffers).
6. **Picker opens prompted on unsaved work.** Opening a file from quick
   open, the explorer, or the buffer picker now switches with `:hide
   edit` / `:hide buffer`: the modified buffer stays in the buffer list
   instead of raising the Save/Discard prompt. Close, Quit, and the
   explicit Open prompt still protect modified buffers; Quit checks all
   buffers including hidden ones.

Scan completion is now an explicit flag instead of "snapshot non-empty",
so a completed empty project no longer reports "Indexing project..."
forever.

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

`xim_initialize()` calls `xim_project_init(path)` for a directory argument
or a bare start, and `xim_project_disable()` for a file-only invocation.
In disabled mode the picker serves explicit paths only and no walk ever
starts. Explicit paths work in that mode because the picker falls back to
the direct filesystem check.

### Refresh preserves expansion

`start_scan()` leaves `expanded_directories` untouched. The Plan 2 test only
proved that expansion survived reopening the explorer: `:edit!` did not
refresh the index. Plan 3 wires the native Refresh project action and tests
that a newly created file appears. See `cmake/PLAN3_VALIDATION.md`.

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
  ASan build: 2/2 in 15.3 s. TSan build: 2/2 in 16.6 s.
- No new compiler warnings. `git diff --check` passes.
- Python PTY script (`cmake/test_xim.py`) extended with the Plan 2
  closeout scenarios plus the review-closeout scenarios: 12 workflow
  functions (13 status reports at that round),
  all passing on `build/dev/src/xim` (and on the clean tracked-source
  build's candidate binary) in ~15 s. The absolute-path scenario types
  every character of the absolute path, including the bare `/` prefix
  that used to crash the editor.
- Vim-native screen suite: 15/15 test functions pass, including the
  three review-closeout tests (anchored gitignore, arrow selection,
  dirty-buffer switch).
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
  - `project_gitignore_anchored` / `Test_xim_project_gitignore_anchored` —
    root `/cache/` hides `cache/` but keeps `src/cache/`.
  - `project_gitignore_scoped` — a rule in `a/.gitignore` does not hide
    `a/keep.txt` merely because the owner is named `a`.
  - `project_picker_arrows` / `Test_xim_project_arrow_select` — Down then
    Enter opens the second entry, not the first.
  - `project_dirty_switch` / `Test_xim_project_dirty_switch` — a picker
    switch keeps a modified buffer without prompting, the buffer list
    still shows it, and Quit still protects it.
- Existing screen dumps `Test_xim_status`, `Test_xim_selection`,
  `Test_xim_open`, `Test_xim_find`, `Test_xim_palette`,
  `Test_xim_unsaved`, `Test_xim_theme_*`, `Test_xim_palette_scroll`,
  `Test_xim_failed_save`, and `Test_xim_failed_unnamed_save` all
  reproduce byte-for-byte.

### Known limitations

- The overlay prompt line renders a trailing `>` from the first item.
  This is the cursor visualisation that vim places on the prompt row;
  the same artifact appears in the palette dumps. Recorded, not fixed.
- Anchored multi-component `.gitignore` patterns (a leading `/` followed
  by another `/`) are skipped. Single-component anchors match one
  component deep, matching git.
- At this round, results required the next keypress after scan completion.
  Plan 3 corrects this with a wake descriptor, not an idle polling loop.
- At this round, matching still ran synchronously on the editor thread.
  The earlier diagnostic measured roughly 45 ms on 100000 paths; rank
  caching removed repeated comparator work but was not a new acceptance
  measurement. Plan 3 moves matching off-thread, adds cancellation and
  an idle wake, and measures completed visible results. See
  `cmake/PLAN3_VALIDATION.md`.
- Explorer activation always resolves through the project root; opening
  a file symlink changes the buffer name to the resolved target.
- `xim_native_screens` runs only where the runner `vim` has `+terminal`.
  The existing local `build/default` binary lacked it, so the suite silently
  skipped there (`build/default/src/vim --version` showed `-terminal`);
  this was an observed build state, not a default-preset requirement.
  `build/dev` is the acceptance configuration for this test.  A skipped
  suite still exits 0 — check the `messages` file for "NO tests
  executed" before trusting a green run in a new build directory.

## Calibrated performance

60 samples per binary, alternating order, 20000-line fixture, 24x100
xterm-256color PTY, matching runtime contents. Values in milliseconds,
against the Plan 1 acceptance round:

| Phase | Xim median | Xim p90 | Reference median | Xim p90 budget |
| --- | ---: | ---: | ---: | ---: |
| First paint | 21.31 | 21.85 | 24.13 | — |
| Launch to first accepted edit | 21.62 | 22.14 | 24.43 | 30 |
| Typing | 0.36 | 0.37 | 0.41 | 5 |
| Selection | 0.25 | 0.30 | 0.29 | 5 |
| Page scroll | 0.28 | 0.34 | 0.33 | 5 |
| 10 KiB paste | 11.78 | 12.02 | 191.39 | 20 |
| Command UI | 0.27 | 0.31 | 0.21 | 5 |

Every phase stays inside the budget. The Plan 2 closeout slice is
unchanged from Plan 1 on the input side; the small shifts versus the
Plan 1 round come from a different 60-sample run on the same machine,
not from the navigation code.

The one-second completion self-test reports 1023.6 ms for native first
accepted edit and 1026.3 ms for the reference, rejecting echoed input as
completion.

Raw evidence (local, gitignored):

- `temp/XIM_PLAN2_PERF.json`
- `temp/XIM_PLAN2_SELFTEST.json`
- `temp/XIM_PLAN2_STARTUP.log`