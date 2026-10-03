# Plan 2 local validation — 2026-10-03

Source: `c28ce9f0f39af727b72914e6022788605c860a41`, plus the Plan 2 navigation
slice. Clang/Clang++ 22.1.8, libc++, C++26, CMake 4.3.4, Ninja 1.13.2, Linux
UTF-8. The acceptance configuration is `build/dev`, matching Plan 1.

## Scope of this slice

`xim .` treats a directory argument as a project root. Ctrl-P opens quick open,
Ctrl-E the explorer, Ctrl-Shift-B the buffer picker, and Ctrl-Shift-P the
command palette. F1 and F2 remain palette fallbacks because terminals usually
cannot distinguish Ctrl-Shift-P.

## Implementation

`src/xim_project.cpp` owns the project index. Enumeration runs on one bounded
background job and writes only owned path data; the editor thread never shares
Vim globals with it. A generation counter rejects a scan from a superseded root.
The index is bounded to 100000 paths, quick open to 200 results, and the explorer
to 500 rows. `.git`, `.hg`, `.svn`, `build` and `node_modules` are pruned during
the walk. Ranking is exact basename, then basename prefix, then path substring,
compared case-insensitively.

## Indexing does not block first edit

The first implementation scanned synchronously inside `xim_initialize()`. On this
repository (429860 files) that pushed first paint past 10 s, so the process never
reached the editor. That failed the plan requirement that index work must not
block first edit.

After moving the walk to a background job, median first paint over seven runs:

| Root | Files | First paint |
| --- | ---: | ---: |
| empty directory | 0 | 16.4 ms |
| synthetic tree | 1000 | 16.8 ms |
| this repository | 429860 | 16.9 ms |

Startup is flat across project size.

## Correctness

- Full `build/dev` suite: all 17 CTest registrations passed in a serial run
  (700.70 s total, 682.15 s in the eight Vim-script batches).
- Native label: 3/3 passed, including the new screen dumps.
- ASan/UBSan and TSan native command/PTY tests passed. The TSan run covers the
  background index and the editor thread's snapshot reads.
- No new compiler warnings. `git diff --check` passes.
- New PTY coverage asserts the directory argument roots the project, quick open
  opens the chosen file without editing text, ignored trees never appear, the
  explorer expands a directory, and the buffer picker lists open files.
- New screen dumps `Test_xim_quick_open` and `Test_xim_explorer` assert the
  relative-path display, the collapsed and expanded directory markers, and the
  prompt labels.

### Known limitations

- The overlay prompt line renders a trailing `>` from the first item. This is
  pre-existing `xim_engine_overlay()` behavior; the Plan 1 palette dump shows the
  same artifact. It is recorded, not fixed here.
- `.gitignore` is not honored yet. Only the fixed component exclusions apply.
- Results refresh on the next render after a scan completes. There is no explicit
  wake of the input wait, so a result may appear on the next keypress.

## Calibrated performance

60 samples per binary, alternating order, 20000-line fixture, 24x100
xterm-256color PTY, matching runtime contents. Values in milliseconds, against
the Plan 1 acceptance round:

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

The one-second completion self-test reports 1023.6 ms for native first accepted
edit and 1026.3 ms for the reference, rejecting echoed input as completion.

Raw evidence (local, gitignored):

- `temp/XIM_PLAN2_PERF.json`
- `temp/XIM_PLAN2_SELFTEST.json`
- `temp/XIM_PLAN2_STARTUP.log`