# Plan 1 local validation — 2026-10-03

Source: `68465ee00f5d8048014e1f8def292606c08d444f`, plus the Plan 0 closeout,
Plan 1, and subsequent save-workflow fixes. Clang/Clang++ 22.1.8, libc++, C++26,
CMake 4.3.4, Ninja 1.13.2, Linux UTF-8. The primary acceptance configuration
is `build/dev`: Huge/no-GUI, RelWithDebInfo, X11, Wayland, pixman, channels
and terminal enabled; optional interpreters off, matching the reference.

## Implementation and correctness

`xim` links a native C++ controller and command resolver through the C engine
adapters in `src/xim_input.h`. It uses the inherited terminal decoder and
text engine. `vim`, `xim --vim`, and Ex/silent execution keep compatibility
dispatch. Startup stages the runtime and generates help tags. The input
contract is in `cmake/XIM_INPUT.md`; user instructions are in
`runtime/doc/xim.txt`.

Validated behavior includes typing and replacement, movement, wrapped lines,
exclusive multi-line selection, internal clipboard, undo/redo, literal UTF-8
and tab paste, find/next/previous, open/save/save-as/close/quit, unsaved-change
choices, readonly and failed writes, explicit configuration, and ignored modal
insert mappings. Ctrl-S flow control is disabled in the actual PTY attributes.
Pasted text stays literal even with autoindent enabled; the option is restored.
Typing undo groups are bounded to 256 characters. Palette selection scrolls
into view on a ten-row terminal. Failed confirmation writes retain the error and the pending Save/Discard/Cancel
choices.

Plan 1 review defects R1 and R2 are closed. Successful Save and Save-as calls
preserve the insertion position, including a bracketed paste ending at a
buffer or CRLF boundary. An empty or failed unnamed Save-as continuation keeps
visible Save/Discard/Cancel state, an invalid filename is restored to the
original buffer state in xim, and clearing the prompt does not revive hidden
pending actions.

Results:

- Tracked-source export `build/p1clean`: fresh configure/build, 8/8 registered
  tests, and no source-tree generated headers in Ninja dependencies. Explicit
  `--include` arguments supply prospective source additions before tracking.
- Full `build/dev` suite: all 17 CTest registrations passed in a serial run
  (702.35 s total, 684.16 s in the eight Vim-script batches after the save-flow
  fixes). This covers 274 inherited script targets, 11 legacy targets, four C
  units, libvterm, mixed C/C++ linkage, Vim smoke, native command/PTY tests,
  and native screen tests.
- The initial concurrent run failed in `test_plugin_termdebug` while other
  builds were active. Isolated candidate and reference runs each passed all
  13 tests. The entire `vim_scripts_n_s` batch subsequently passed in 174.5 s.
  The cause of the initial failure is not established. Its evidence is in
  `temp/XIM_PLAN1_FULL_INITIAL_FAILURE.md`; no tests were changed or skipped.
- ASan/UBSan and TSan native command/PTY tests passed after the save fixes,
  including failed confirmation writes and unnamed Save-as continuations.
  Plan 0 sanitizer engine-unit evidence is recorded in the closeout document.
- The existing `default` cache, which has Python3 enabled and channels/terminal
  disabled, also builds and passes native PTY tests. Screen verification uses
  the terminal-enabled `build/dev` configuration.
- Explicit tiny configuration builds compatibility Vim; its 6/6 tests pass.
  Native input needs normal or huge Vimscript/Visual features.
- Installed `build/dev/install/bin/xim` passes the workflow and opens
  `:help xim-start` from a separate private directory, with runtime/config
  environment overrides removed. No system Vim supplies the runtime.
- No new compiler warnings. `git diff --check` passes.

Eleven screen dumps cover status, selection, open/find prompts, palette,
unsaved changes, palette scrolling, failed named and unnamed save feedback,
desert and catppuccin. Shipped default, desert, slate, habamax and catppuccin themes
were exercised. Local third-party `phi_canary` and `phi_copper` were exercised
with `termguicolors`; native prompt links follow Pmenu after theme changes.
These local themes are named samples, not dependencies of the registered tests.

## Calibrated performance

Budgets were set before UI implementation in `cmake/PLAN0_CLOSEOUT.md`.
Final acceptance uses 60 samples per binary, alternating order, a 20,000-line
fixture, a 24x100 xterm-256color PTY, and no concurrent builds. Both binaries
use matching runtime contents, enabled syntax, exclusive selection and
two-line status height. Arguments include `-n -X -u bench.vim -i NONE`;
system clipboard connections are disabled for this workload. Native input
uses Xim shortcuts. Completion is editor-assembled, follows redraw, and is
checked against buffer text and selection/scroll state.

Final results in milliseconds:

| Phase | Xim median | Xim p90 | Reference median | Reference p90 | Xim p90 budget |
| --- | ---: | ---: | ---: | ---: | ---: |
| First paint | 22.790 | 23.567 | 25.693 | 26.789 | — |
| Launch to first accepted edit | 23.107 | 23.872 | 26.013 | 27.127 | 30 |
| Typing | 0.408 | 0.448 | 0.433 | 0.504 | 5 |
| Selection | 0.306 | 0.330 | 0.307 | 0.383 | 5 |
| Page scroll | 0.271 | 0.311 | 0.337 | 0.384 | 5 |
| 10 KiB paste | 12.430 | 12.678 | 198.614 | 203.390 | 20 |
| Command UI | 0.303 | 0.363 | 0.223 | 0.292 | 5 |

Command UI compares the native palette with the preserved Ex prompt; their
available interfaces differ. The two earlier equivalent-config 60-sample
rounds also meet the budgets. Earlier minimal-reference startup samples did
not enable syntax and are not equivalent startup configurations.

The one-second completion self-test reports 1023.5 ms for native first edit
and 1027.1 ms for the reference, rejecting echoed input as completion.
A separate 60-sample compatibility comparison reports candidate/reference
median first accepted edit 7.451/7.419 ms and p90 7.748/7.846 ms, with no
repeatable regression against the corrected Plan 0 rounds.

Raw evidence (local, gitignored):

- `temp/XIM_PLAN1_FINAL_PERF.json`
- `temp/XIM_PLAN1_EQUIVALENT_PERF{1,2}.json`
- `temp/XIM_PLAN1_FINAL_SELFTEST.json`
- `temp/XIM_PLAN1_COMPAT_PERF.json`
- `temp/XIM_PLAN1_STARTUP_NORMAL.log`
- `temp/XIM_PLAN1_SAVE_FIX_PERF.json`
- `temp/XIM_PLAN1_SAVE_FIX_SELFTEST.json`
- `temp/XIM_PLAN1_SAVE_FIX_STARTUP.log`

## Profiling and limits

The non-instrumented startup trace spends 14.77 ms sourcing syntax/filetype
support and 2.46 ms opening buffers; its internal first redraw is 19.43 ms.
The end-to-end PTY metric also includes process launch and runtime setup.
The startup difference from earlier minimal Vim samples comes primarily
from enabled runtime support; the equivalent baseline includes it too.

A separate 20-sample `-pg -DWE_ARE_PROFILING` build records the same phases
and whole-workload CPU data in `build/prof/profiles-final/*.gmon` and
`temp/XIM_PLAN1_FINAL_PROFILE.json`. Keeping Vim's SIGPROF handler intact
requires `WE_ARE_PROFILING`. Aggregate gprof self samples identify `win_line`
(19.77%) and `screen_line` (13.95%) as the largest individual consumers.
This profile includes benchmark acknowledgment and snapshot machinery;
its 10 ms sampling resolution does not establish sub-millisecond per-phase
CPU attribution or zero controller cost. The local gprof/BFD reader emits
a DWARF FORM 0x23 diagnostic; symbol-level flat profiling remains available.
Instrumented timing is diagnostic and is excluded from acceptance numbers.

The tested boundary is Linux/xterm UTF-8 text input. Mouse/GUI input,
binary NUL payloads, OSC 52 transport and unchanged modal plugin hooks are
outside this milestone. System clipboard adapters reuse the inherited
X11/Wayland paths; the internal fallback is the exercised acceptance path.
The supported scripting and terminal boundaries are documented in `xim.txt`.
These results make no 20x plugin-stack performance claim.
