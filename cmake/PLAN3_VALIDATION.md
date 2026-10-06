# Plan 3 local validation — 2026-10-05

Source: Plan 3 working-tree changes over `465c118b9`; the implementation
commit contains this report. Linux UTF-8, Clang/Clang++ 22.1.8, libc++,
C++26, CMake 4.3.4, Ninja 1.13.2. Acceptance build: `build/dev`, Huge,
RelWithDebInfo (`-O2`). Builds, tests and timing runs were serial.

The operator authorized background completion and fixes for flashing native
UI after a hands-on run. No content search, LSP, Windows or replacement
renderer is included.

## Implementation

- Two persistent workers: one scanner and one matcher. The scanner owns its
  root; the matcher shares an immutable path index with pre-folded names.
- Scan/query requests coalesce. Completion is one latest-result slot and a
  scan-change flag. Project generations and query revisions reject old work.
- Walking and matching check cancellation. Refresh starts a new generation
  without joining the old scan on the editor thread. Large retired indexes
  are freed off-thread, outside the state mutex.
- Matching keeps a best-200 heap instead of sorting all matching paths.
  Explicit-path filesystem checks also run on the matcher.
- A nonblocking, close-on-exec pipe joins the inherited Unix select/poll wait.
  The native top-level seam handles completion outside `safe_vgetc()`; no
  synthetic input is injected. Timers, signals, channel events, partial keys,
  resize and the one-time idle swap flush remain supported.
- Enter during matching waits for the current query. Following keys and paste
  have an owned 256-entry queue; further terminal bytes stay unread at capacity.
  Escape cancels the choice and queued input.
- Refresh project is a native palette action. Tests create a new file after
  the initial scan and prove that refresh discovers it while retaining explorer
  expansion. The earlier `:edit!` test did not refresh the index.
- Native overlays retain cached cells. Text is drawn before clearing trailing
  cells; shrinking restores only exposed rows from a UTF-8/attribute snapshot.
  Closing restores the editor and clears the prompt. The renderer recomposes
  overlays at the end of engine redraw and resize, using inherited synchronized
  output only when supported. The cursor no longer moves to the document on
  each prompt key.
- Buffer rows show stable buffer numbers, active/modified markers and shortened
  paths with visible filename tails. Activation uses the number, not display
  text. Two unnamed modified buffers are distinct choices.

## Reproduced flashing and output comparison

Fixture: 40 document lines containing `UNDERLYING_MARKER alpha beta`,
24x100 xterm-256color, palette open, then `Save` typed one character at a time.
The baseline is the clean-source Plan 2 executable at `465c118b9`.

| Terminal output | Before | After |
| --- | ---: | ---: |
| Four filter characters, bytes | 3325 | 2223 |
| Underlying row writes | 30 | 17 |
| Last two characters (`ve`), bytes | 816 | 2 |
| Covered document writes during `ve` | 4 | 0 |
| Unchanged `Save as` row writes during `ve` | 2 | 0 |

The 17 remaining document writes restore rows exposed when the result list
shrinks; they do not erase the part still covered by the menu. This is an
output-work measurement, not a claim about every terminal's display timing.
Tests also check arrow selection, Unicode/combining-cell restoration, closing
and resize without another input key. Existing theme dumps remain unchanged.

## Correctness gates

- Full `build/dev` CTest suite: 18/18 registrations pass, including inherited
  Vim-script batches, native command tests, project ownership tests, PTY and
  screen checks. The native screen runner executed all 16 test functions;
  it did not skip. PTY coverage has 15 workflow functions (16 status reports).
- ASan/UBSan and TSan native suites: 3/3 each (commands, project module, PTY).
  These configurations do not register the inherited screen suite.
- Fresh tracked-source export (`build/p3clean`): 9/9 registrations pass;
  generated-header routing passes and no source-tree generated header is used.
- Installed candidate under `build/plan3/install`: all PTY workflows pass from
  unrelated private directories without runtime/configuration overrides.
- No compiler warnings in successful final builds. `git diff --check` passes.

Project tests cover rank order, case matching, bare slash queries, the 200-result
cap, 1000 superseded queries, root replacement during matching, repeated refresh,
empty completion, disable, descriptor flags and repeated immediate shutdown.
The stop predicate is changed under the condition-variable mutex to prevent a
lost wake. PTY checks cover idle scan/query completion, no idle output loop,
immediate Enter plus subsequent typing/paste, modified-buffer retention,
refresh discovering new files, stable unnamed buffers, overlay restoration
and idle resize.

Two validation failures were corrected, not waived: the initial custom wait
needed inherited signal/flush handling and had to leave partial keys to the
decoder; the clean export exposed clipped buffer names in longer build paths.
The latter prompted the stable-ID/truncated-display fix. Screen tests exposed
an uncleared prompt after close, which is also fixed.

## Completed performance

Exactly 100000 files: two root files and two 49999-file families across 200
directories. New local fixture, warmed published path index; no disk-cache
flush. Sixty queries alternate `alpha`/`beta`. Completion requires the current
query label and exact selected file on the rendered screen; activation contents
are checked. Sixty separate project starts verify a content-checked edit during
active scanning and orderly cancellation on exit.

| Measurement | Median ms | p90 ms | Local p90 budget |
| --- | ---: | ---: | ---: |
| Warm query to visible result | 5.104 | 5.600 | 30 |
| Project start to first accepted edit during scan | 19.866 | 20.517 | 30 |

Idle picker: 0 output bytes and 0 process CPU ticks in the one-second sample
(100 ticks/second); resident memory about 28.3 MiB. This is a short idle sample,
not a long-running memory or CPU guarantee.

The isolated 100000-path module fixture completes 60 queries in roughly
1.4 ms p90. This is backend diagnostics, not visible-result latency.

Existing editing comparison: 60 samples per native executable, alternating
Plan 2 baseline and candidate, identical harness/runtime configuration and
20000-line fixture. The incremental PTY decoder is used for both. Historical
raw-output timings are not directly comparable because harness decoding adds
cost; the like-for-like native comparison is:

| Phase | Before median ms | After median ms | After p90 ms | Budget ms |
| --- | ---: | ---: | ---: | ---: |
| First accepted edit | 22.673 | 22.890 | 23.813 | 30 |
| Typing | 0.607 | 0.621 | 0.711 | 5 |
| Selection | 0.624 | 0.633 | 0.707 | 5 |
| Page scroll | 0.616 | 0.626 | 0.724 | 5 |
| 10 KiB paste | 13.720 | 13.573 | 14.009 | 20 |
| Palette entry | 1.528 | 1.506 | 1.650 | 5 |

All local budgets pass, with no median increase reaching the existing 0.25 ms
regression threshold. A separate 60-sample comparison against the preserved
Vim reference also stays within budget; palette versus Ex prompt remains a
different-interface comparison. The delayed completion self-test takes about
1024 ms for native first edit and rejects echoed input as completion.
These results do not establish the product's eventual 20x target.

## Remaining boundaries

- Explorer output is capped at 500 rows, but directory enumeration remains
  synchronous and may examine more entries before limiting the output.
- Cancellation cannot interrupt a filesystem call already blocked in the OS.
  Such a call can delay shutdown or the next worker request, though not typing.
- No automatic filesystem watcher, native root-change UI, content search or
  language features are added here. Broader Plan 2 requirements such as full
  multi-buffer save-choice UX still need separate closeout.
- Anchored multi-component ignore patterns remain unsupported. The supported
  single-component scoping fixes from Plan 2 remain covered.
- Full redraw on terminal resize is still necessary; synchronized updates depend
  on terminal support. This patch removes the reproduced per-key erase/repaint
  loop, not every possible terminal flicker.

## Local evidence (gitignored)

Under `build/plan3/`: `full-tests-final.log`, `asan-tests-final.log`,
`tsan-tests-final.log`, `clean-build-final2.log`, `installed-tests-final.log`,
`native-comparison-final.json`, `edit-perf.json`, `project-perf-final.json`,
`self-test.json`, `repaint-comparison.log`, `project-unit-final.log` and raw
terminal captures. `temp/XIM_PLAN3.md` records the authorized plan.
