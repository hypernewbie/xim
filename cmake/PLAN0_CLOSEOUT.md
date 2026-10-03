# Plan 0 closeout — 2026-10-02

Base revision: `68465ee00`, plus the Plan 0 closeout working-tree changes.
Clang/Clang++ 22.1.8; Ninja 1.13.2; CMake 4.3.4; Linux, Huge/no-GUI,
RelWithDebInfo (`-O2 -g`), system optional features enabled, interpreters off.

R1: config, osdef, Ruby, xdiff and Wayland includes select the build headers.
The tracked-source export in `build/plan0-closeout-clean/source` builds without
inherited generated headers. Ninja dependencies confirm build-tree routing.
The xdiff change is confined to Vim's integration includes.

R3: the core and consumers inherit the interface compile policy, including
`-Wall`, `_REENTRANT`, fortify level 1 and the reference warning suppression.
No compiler warnings occurred in the clean, ASan or TSan builds.

R4: version metadata identifies the CMake configuration and selected flags,
dependencies, compilation database and Ninja link-command query. It no longer
reports a fixed reference command as the actual command.

R2: PTY completion markers are concatenated by Vim and cannot occur in command
echo. Edited buffer text is checked before signaling; timers precede writes.
First paint and first accepted edit are separate metrics. The one-second delay
self-test passes on both reference and candidate. Previous edit timings are
invalid. Corrected 60-sample rounds (reference/candidate median ms):

| Workload | Round 1 | Round 2 |
| --- | --- | --- |
| Startup/exit | 4.920/4.707 | 4.879/4.610 |
| First paint | 7.099/6.818 | 6.959/6.774 |
| First accepted edit | 7.738/7.486 | 7.593/7.369 |
| Scroll | 0.512/0.510 | 0.512/0.509 |
| Completed insert | 0.592/0.579 | 0.562/0.583 |

Results: `temp/XIM_PLAN0_CLOSEOUT_PERF{1,2}.json`. No repeatable regression.
Both rounds ran on the same host with alternating binary order.

All 274 new-style and 11 legacy script targets passed after closeout, along
with four C units, libvterm, mixed C/C++ and Vim smoke. ASan and TSan unit sets
each passed 6/6. Full-suite evidence is under `build/p0/Testing`.

Initial export tests encountered two path-dependent harness failures: a Unix
socket name exceeded its limit in the deep build directory, and the expanded
runtime path wrapped in `Test_wildmenu_pum`. The short `build/p0` path fixed
the socket case. The command-line test passed on both binaries with the short
matching checkout runtime, and the entire a-c batch then passed 46/46. No
test was skipped or weakened. The other five script batches passed on the
exported runtime. These path constraints are documented in the build guide.

Plan 1 budgets, set before UI implementation on this host: first accepted edit
p90 <= 30 ms; single-key typing/selection/scroll/palette p90 <= 5 ms;
10 KiB paste p90 <= 20 ms. Use at least 60 samples, validate resulting text,
and investigate repeated exceedances. These are local milestone budgets, not
portable wall-clock assertions or a 20x performance claim.
