# Plan 4 local validation — 2026-10-06

Source: Plan 4 working-tree changes over the Plan 3 commit. Linux UTF-8,
Clang/Clang++ 22.1.8, libc++, C++26, CMake 4.3.4, Ninja 1.13.2. Acceptance
build: `build/dev`, Huge, RelWithDebInfo (`-O2`). Builds, tests and timing
runs were serial.

The operator authorized native top menus and ordinary mouse interaction on
2026-10-06. The work stays in native mode on the Linux terminal and exposes
only shipped commands. No content search, LSP, Windows bring-up, plugin menu
system or replacement renderer is included.

## Implementation

- The top row is a native File/Edit/View/Navigate/Help bar drawn through the
  engine tabline layout; inherited tab labels never appear in native mode.
  Headings and drop-downs come from one pure registry and hit layout in
  `xim_menu`; commands keep their existing `Action` identifiers.
- F10 opens menus. Left/Right switch headings, Up/Down move, Enter activates,
  Escape dismisses, and Alt-letter works where the decoder distinguishes it.
  `F10` was the redo fallback; `Ctrl-Y` remains redo and `Ctrl-N` creates a
  new buffer. `F3` remains Next match. These changes are documented in
  `runtime/doc/xim.txt`.
- Drop-downs show names, shortcut hints, checked View options and disabled
  Edit/Find entries. Clicking a heading, hovering while open, pressing and
  dragging onto an entry, and clicking outside all share the keyboard layout.
  Narrow bars clip trailing headings with a UTF-8 marker; every command
  remains reachable by keyboard.
- Mouse reporting reuses the inherited SGR decoder and is enabled by default
  in native mode. Click, Shift-click, drag, double-click and triple-click use
  owning-thread `mouse_find_win`, `mouse_comp_pos` and `coladvance` adapters
  with native half-open selection endpoints. Double-click uses inherited word
  classes; triple-click selects the logical line.
- The wheel scrolls the window under the pointer using the engine's
  `'mousescroll'` step and never moves the caret or selection; horizontal
  wheel applies to unwrapped text. Edge motion during a drag scrolls one
  line or column per drag event.
- Middle-click positions then pastes through the same clipboard boundary as
  `Ctrl-V`. Right-click opens the native Edit context menu without clearing
  the selection. Dragging a status line or vertical separator resizes the
  split through `win_drag_status_line`/`win_drag_vsep_line`; release clears
  the capture.
- Prompts own a UTF-8 caret with Left/Right, Home/End, Delete, Select all,
  clipboard operations and mouse placement/selection. Picker rows and
  confirmation Save/Discard/Cancel zones accept clicks and the wheel;
  activation reuses the last rendered rows so clicks and Enter agree.
- Passive motion reporting is enabled only while a menu is open. A resize
  during pointer capture cancels safely.

## Validation

- Menu unit tests cover the registry, enabled/checked states, keyboard
  skipping, heading geometry, narrow clipping, drop-down layout, scrolling,
  clamped context menus and hit testing. Existing command and project tests
  keep their coverage.
- PTY checks use real SGR reports from a 24x100 terminal: rendered text after
  clicks, drag replacement and copy, double/triple clicks, middle paste,
  wheel scroll with caret preservation, F10 menus and menu-item activation,
  save protection and confirmation clicks, quick-open/palette click and
  wheel selection, prompt mouse placement, invalid coordinates, separator
  resize, press/resize cancellation and compatibility-mode separation.
- Screen dumps cover the top bar, File/Edit/View menus (including the
  disabled entries and checked View options) and mouse selection. Earlier
  native dumps were updated for the deliberately reserved top row; inherited
  Vim dumps are unchanged. The native runner executed all 19 test functions
  without skipping.
- Idle and incremental redraw were re-measured with a 200-line fixture:
  0 output bytes in a two-second idle sample with the menu bar present,
  609 bytes for opening the File menu, and 20 and 1 bytes for two palette
  keystrokes after the result list stabilized, with zero document rows
  rewritten.

## Correctness gates

- Full `build/dev` CTest suite: 19/19 registrations pass, including the
  inherited Vim-script batches, native unit tests, project tests, PTY checks
  and 19 executed screen functions.
- ASan/UBSan and TSan native suites: 4/4 each (commands, menu, project, PTY).
  These configurations do not register the inherited screen suite.
- Fresh tracked-source export (`build/p4clean`) with the prospective new
  files included: 10/10 registrations pass; generated-header routing passes
  and no source-tree generated header is used.
- Installed candidate under `build/plan4/install`: all 16 PTY workflow checks
  pass from private directories without runtime or configuration overrides.
- No compiler warnings in successful final builds. `git diff --check` passes.

## Completed performance

Exactly 20000-line fixture, 24x100 xterm-256color, `-n -X -u bench.vim`,
syntax enabled, 60 alternating runs per binary, completion on an
engine-observed content or cursor event. Menu and mouse phases are
native-only: menu completion is the rendered item, click/drag completion is
the `CursorMoved` state at the pointer with the press anchor intact, and
wheel completion is the first newly visible fixture line.

| Phase | Median ms | p90 ms | Local p90 budget |
| --- | ---: | ---: | ---: |
| First paint | 21.456 | 22.053 | — |
| First accepted edit | 22.140 | 22.720 | 30 |
| Typing | 0.644 | 0.721 | 5 |
| Selection | 0.663 | 0.740 | 5 |
| Page scroll | 0.650 | 0.703 | 5 |
| 10 KiB paste | 13.569 | 13.872 | 20 |
| Palette entry | 1.644 | 1.830 | 5 |
| Menu open | 1.791 | 1.868 | 5 |
| Menu selection move | 0.241 | 0.274 | 5 |
| Mouse click | 1.815 | 1.938 | 5 |
| Mouse drag select | 1.972 | 2.085 | 5 |
| Mouse wheel | 2.142 | 2.808 | 5 |

The reserved menu row reduces the text area by one line, so a PageDown now
lands on line 20; the harness completion threshold changed from `>20` to
`>19` for both engines, leaving the measured operation the same. The
preserved Vim reference in the same run reports first paint 24.094/24.996 ms,
typing 0.758/0.842 ms, page scroll 0.607/0.700 ms and 10 KiB paste
195.809/200.789 ms; the Ex-prompt palette comparison remains a
different-interface comparison.

Warm project query and edit-during-scan numbers were re-measured on the
existing 100000-path fixture (two 49999-file families, 200 directories):

| Measurement | Median ms | p90 ms | Local p90 budget |
| --- | ---: | ---: | ---: |
| Warm query to visible result | 5.067 | 5.452 | 30 |
| Project start to first accepted edit during scan | 19.688 | 20.398 | 30 |

Idle picker sample: 0 output bytes and 0 process CPU ticks in one second
(100 ticks/second); resident memory about 29.4 MiB. This is a short idle
sample, not a long-running memory or CPU guarantee. All local budgets pass,
and the Phase 3 comparisons show no reproducible regression.

## Validation corrections

- The PTY wait helper now rechecks the retained screen model before each
  read, because incremental overlays can render a result and then emit no
  further bytes. Raw output markers still complete waits such as the Ex
  acknowledgement.
- The test-terminal model learned inherited scroll-region edits (`CSI S/T/L/M`)
  so wheel scrolling asserts the engine state and the tracked screen agree.
- The mouse wheel step follows `'mousescroll'` instead of one line per notch;
  a status/vertical separator press arms the engine's frame-drag helpers.
- The menu bar increases the reserved top row; status-line screen dumps and
  the palette scroll test were updated accordingly. The reference dumps keep
  the harness-trimmed trailing whitespace convention.

## Remaining boundaries

- A terminal or multiplexer can still intercept mouse reports, wheel
  gestures or Alt sequences before the editor sees them. Pinch, pixel and
  operating-system gestures are outside the reporting boundary.
- A terminal resize during a pointer capture cancels the drag; the capture is
  not resumed at the new geometry.
- Narrow terminals clip trailing headings and long drop-downs scroll; there
  is no horizontal menu scrolling.
- Horizontal wheel is a no-op for wrapped text and only applies to unwrapped
  windows. Middle-click paste depends on the configured clipboard or the
  internal text clipboard.
- Explorer enumeration remains synchronous and capped at 500 output rows; no
  automatic filesystem watcher, native root-change UI, content search or
  language features are added. Anchored multi-component ignore patterns
  remain unsupported.
- Full redraw on terminal resize is still necessary; synchronized updates
  depend on terminal support. This patch keeps the incremental overlay and
  menu drawing work bounded, not every possible terminal flicker removal.
- These results do not establish the product's eventual 20x target.

## Local evidence (gitignored)

Under `build/plan4/`: `full-tests.log` (19/19), `asan-tests.log` and
`tsan-tests.log` (4/4 each), `clean-build.log` (10/10 plus routing),
`installed-tests.log`, `native-comparison-final.json` (60-sample editing,
menu and mouse), `project-perf-final.json` (60 starts), `ui-probe.log`
(idle/overlay), `bench-check.json`, `asan-configure.log`, `tsan-configure.log`
and build logs. `temp/XIM_PLAN4.md` records the authorized plan.
