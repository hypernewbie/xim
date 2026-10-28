# Plan 6 local validation — 2026-10-27

Source: implementation through `d86f0673d`, followed by eleven review commits
from `731420737` through `e23f49740`. Plan 5 comparison baseline: `d8c2f80da`.
The final gates below ran again after the last code fix. Review details are in
`temp/XIM_PLAN6_REVIEW.md` when that local file is present.

Linux UTF-8, Clang/Clang++ 22.1.8, libc++, C++26, CMake 4.3.4, Ninja 1.13.2.
Acceptance build: `build/dev`, Huge, RelWithDebInfo (`-O2`). Builds, test runs
and timing workloads did not overlap. Each build used up to eight compiler
jobs.

The operator authorized the live-project slice on 2026-10-27.  The work stays
in native mode on the Linux terminal.  It does not add an integrated terminal,
language features, Windows support, trash, filesystem undo, recursive copy,
cross-filesystem move emulation or multi-root workspaces.

## Implementation

- The explorer is worker-owned.  Each directory page holds at most 200 entries;
  a Load more row requests the next page, and page tokens detect a directory
  change between requests.  The worker owns the 64 MiB directory cache and the
  immutable flattened view.  Rows carry display text, path identity, kind,
  depth, flags and revision; activation never rebuilds a path from indentation
  or a clipped label.  Empty directories, control-character escaping, file
  symlink aliases and unexpanded directory symlinks are covered.  Directory
  listing uses `d_type` when the filesystem supplies it and falls back to an
  anchored `fstatat` for `DT_UNKNOWN`, special files and symlink targets.
- A Linux inotify service in `xim_watch.cpp` owns descriptors and watch
  records.  It watches project directories, new subtrees, ignore files and the
  parent directories of explicitly opened files.  Events coalesce for 50 ms
  with a 250 ms maximum delay; the dirty set is capped at 4096 paths or 1 MiB.
  Ordinary events patch the affected index directories and explorer caches
  off-thread.  Overflow, root loss, ignore changes and explicit refresh request
  one reconciliation.  Watch failure is reported with its cause and leaves
  manual Refresh project usable.  The service never accesses Vim globals or
  buffer pointers and notifies through the project wake pipe.
- External changes are observations.  A clean, current UTF-8 file at or below
  256 KiB stages owned disk bytes on the read worker.  The owning-thread
  adapter rechecks buffer number, name, edit tick, disk identity and focus
  before applying them; typing during the read becomes a conflict.  Dirty,
  hidden, oversize, binary and non-UTF-8 buffers receive a changed notice.
  Reload from disk on a dirty buffer requires explicit `[r]` consent.  External
  deletion keeps the text with a missing notice.  An external rename is
  accepted only when the old name is gone and the destination version equals
  the buffer's last accepted disk version. This avoids treating Vim
  `writebackup` renames as external moves.
- Native create, rename and move are buffered as a preflight/commit pair with a
  serialized worker in `xim_fsops.cpp`.  Traversal is anchored to directory
  descriptors, rejects absolute and `..` destinations, does not follow
  directory symlinks, creates with `O_EXCL` and renames with
  `renameat2(RENAME_NOREPLACE)`.  Permanent delete uses an unlink walk that
  never follows symlinks, reports partial progress and retains affected buffer
  text.  Rename and move update every affected buffer path, including
  descendants of a moved directory, without rewriting buffer contents.
- Recent files and folders persist as JSON in `xim_recent.cpp` under
  `$XDG_STATE_HOME/xim` or `$HOME/.local/state/xim`.  The worker merges under
  `flock`, writes a private temporary file and renames it atomically, with
  directory mode 0700 and file mode 0600.  Missing entries remain selectable
  only for removal. Open folder checks its root on the operation worker
  before the project changes. Close project stops indexing, content search and
  project watching while retaining buffers, explicit-file watches and recents.
- The menu bar is repainted as part of every overlay frame, so the active
  heading no longer depends on an unrelated tabline redraw.  Menu dumps anchor
  the terminal cursor to the last visible drop-down cell for deterministic
  screen output.

## Review closeout

The review reproduced and fixed idle preflight stalls, cancelled-plan ownership,
reload format/stamp/undo errors, and loss of deleted buffer copies. Save now
requires Recreate consent for missing paths, including hidden buffers during
Quit. Directory watches follow moved identities, and namespace revisions prevent
activation before reconciliation. Project directories no longer masquerade as
writable documents or recent files.

Other fixes preserve insertion columns, apply recent-state deltas without
resurrecting removals, and retain failed forms with inline errors. Superseded
confirmations cannot retain an invisible Busy reservation or an old Quit
continuation. A content scan now completes during unrelated overwrites and
create/delete activity. Its coalesced follow-up reads the latest index, and
activation rejects a vanished target.

Two serial runs exposed replace-all above its 50 ms budget. Profiling located
repeated byte-comparison work, which `c8a435e56` removes without changing Unicode
fallback. Benchmark waits now fail on timeout. First-page setup checks index
publication, and the cold clock starts before process creation.

## Validation

- Full `build/dev` test set: all 22 registrations pass, including all eight
  inherited Vim-script batches. The final run uses two groups: 21/21 with a
  canonical checkout-local `TMPDIR`, then `vim_scripts_i_m` 1/1 with a short
  descriptor alias to the checkout. Native coverage is 8/8, including the new
  watcher unit test and executed screen functions.
- The temporary-path split addresses two environment-only failures. A long
  `TMPDIR` wraps the command in `Test_echo_verbose_system`. An all-alias run
  instead fails `Test_terminal_sync_shell_dir`, which expects the physical
  shell directory. The final split satisfies both assumptions. It keeps files
  inside the checkout and changes no inherited assertions or expected dumps.
- ASan/UBSan native tests: 7/7 with leak detection and
  `UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1`. TSan native tests: 7/7
  with `TSAN_OPTIONS=halt_on_error=1`. No sanitizer reports. These builds omit
  the terminal/job-dependent screen registration; `build/dev` runs it.
- Screen dumps cover explorer expansion, retained operation errors, Delete and
  Recreate consent, external-change notices, recents and active menu headings.
  PTY regressions also cover file formats, read hooks, missing hidden buffers,
  failed-form retry, cancelled continuations and multi-instance recent state.
- A 60 MB PTY fixture checks content search, quick open and explorer progress
  during sustained overwrites and create/delete activity in another subtree.
  A separate regression checks a result deleted before watcher notification.
- Installed candidate: `build/plan6-review/install`. The full
  `cmake/test_xim.py` workflow passes from
  `build/plan6-review/closeout-install-work`, without a runtime override. It
  checks installed themes, the new regressions and `xim --vim` compatibility.
- Tracked-source export: `build/plan6-review/closeout-clean`. Build, all 13
  registered tests and generated-header routing pass.
- No compiler warnings in final builds. `git diff --check` passes.

## Completed performance

Plan 6 gates use the proposed budgets from `temp/XIM_PLAN6.md`.  The fixture
tree and process conditions are recorded with each measurement.  No benchmark
ran during a build or another workload.  Fixture creation finished before
timing; `sync(2)` and one untimed create remove fixture writeback from the file
operation samples.

| Completed operation | Samples | Median ms | p90 ms | Budget ms |
| --- | ---: | ---: | ---: | ---: |
| Explorer movement with cached rows | 60 | 0.672 | 0.750 | 5 |
| First explorer page, 100000-child directory | 10 | 62.213 | 63.239 | 250 |
| External create visible, 10000-file tree | 30 | 56.367 | 56.600 | 500 |
| New file to disk, engine and visible completion | 30 | 2.904 | 3.172 | 100 |

The external-change figure includes the watcher's 50 ms coalescing window.  It
measures one external create, explorer refresh and rendered row; the matching
removal is drained between samples and is not timed.  The first-page timer
starts in a ready process immediately before Ctrl-E, so it excludes process
launch and the initial index scan. The file-operation timer starts before
sending the filename plus Enter. It ends at the visible opened-buffer name or
Created notice. The disk entry is checked afterwards.

Startup and warm quick-open comparison, two serial rounds of 60 samples each,
same 100000-path fixture and terminal:

| Measurement | Plan 5 baseline p90 | Plan 6 p90 |
| --- | ---: | ---: |
| Warm quick-open query | 5.155 / 5.152 ms | 5.329 / 5.287 ms |
| First accepted edit during scan | 20.466 / 20.337 ms | 20.621 / 21.019 ms |

Find/replace/content comparison, two serial rounds of 30 samples each:

| Completed operation | Baseline p90 ms | Candidate p90 ms | Budget ms |
| --- | ---: | ---: | ---: |
| Find, six keystrokes over 100000 lines | 41.725 / 41.588 | 33.193 / 33.394 | 60 |
| Replace-all, 1000 matches | 48.365 / 48.988 | 38.929 / 39.033 | 50 |
| Warm content search, 10000 files | 84.445 / 85.284 | 87.675 / 90.408 | 150 |

Single first-fixture content-search samples were 120.618/121.966 ms for the
baseline and 134.382/133.272 ms for the candidate. These are one launch per
round, not p90 figures. They motivated two further comparisons with 30 fresh
processes per binary and alternating launch order. Each round reuses one
10000-file fixture with warm filesystem caches:

| Process-cold content search | Baseline ms | Candidate ms |
| --- | ---: | ---: |
| Median, two rounds | 99.718 / 98.536 | 103.487 / 102.901 |
| p90, two rounds | 103.225 / 103.790 | 107.884 / 105.045 |

The process-cold figures include launch but do not claim cold filesystem
caches. Old cold figures with a post-launch timer are not comparable. Raw
samples include initial outliers as high as 557 ms and remain in the JSON.

Warm content-search medians rose about 4.9%. Process-cold medians rose
3.8–4.4%. These comparisons remain below the documented regression threshold:
more than 10% and at least 0.25 ms in two independent median comparisons.
All defined budgets pass. This is not a claim that the new services cost zero.
RSS after quick open was 28924–29180 kB for the baseline and 29788 kB for the
candidate.

A final root-switch probe alternated two 200-file projects ten times. RSS
stayed at 15104 kB, file descriptors at 8 and inotify watches at 1. Completed
query and content-search runs recorded 0 idle output bytes and 0 process CPU
ticks in one second. No periodic idle scan appeared.

## Boundaries

- The watcher backend is Linux inotify.  A network filesystem can omit events
  even when watch setup succeeds; quick open shows the watcher state and
  Refresh project retries setup and reconciles.  Watch-limit exhaustion is a
  partial state, not a false live claim.  Windows uses a later backend without
  changing buffer ownership.
- Auto-reload is limited to the current, clean UTF-8 buffer at or below
  256 KiB.  Larger, hidden, binary, non-UTF-8 or dirty buffers keep their text
  and receive a notice.  Reload from disk is the explicit dirty-buffer path;
  Save as remains the separate save-copy action.
- Permanent delete has no trash or filesystem undo, and buffer Ctrl-Z cannot
  recover a deleted disk entry.  A recursive delete can report partial
  progress.  Cross-filesystem moves fail and leave the source; recursive copy
  and drag-and-drop are not implemented.
- Explorer pages stop at the per-directory cap and the flattened row cap, and
  report truncation.  The 200-entry page, 64 MiB cache and 20000-row view cap
  are deliberate bounds, not a claim that arbitrarily large trees render in
  one frame.
- Recent state is data only.  It is not Vimscript and does not restore a
  session, window layout or unsaved buffer.  A corrupt or unwritable state
  file leaves the editor usable and the last valid lists intact.
- External rename detection is deliberately narrow.  A move that cannot prove
  the destination version marks the old path missing instead of guessing.
- File-operation timing covers the create path.  Rename and move share the same
  preflight, commit and engine-update path but have no separate timing gate.
- The first-page timing excludes process startup. Launch-to-first-edit remains
  covered by the separate startup comparison above.
- Live content results are an as-read view, not an atomic filesystem snapshot.
  Ordinary activity queues a follow-up while the owned scan finishes. Root or
  ignore-scope uncertainty can still cancel work. A blocking filesystem call
  can delay worker shutdown.

## Gate result

Final tracked-source export (`build/plan6-review/closeout-clean`): passed.
CMake/Ninja and Clang built the tracked sources. All 13 registered tests passed.
Dependency checks routed generated headers through the selected candidate
build, not stale source-tree artifacts.

Local evidence is under `build/plan6-review/`: `closeout-dev-{canonical,short}.log`,
`closeout-{asan,tsan}-native.log`, `closeout-clean.log`,
`closeout-install-pty.log`, `closeout-live-perf.json`,
`closeout-{project,search}-{baseline,current}-{1,2}.json`,
`cold-comparison{,-2}.json` and `closeout-root-switch.json`.
`closeout-perf-binaries.sha256` identifies the compared installed binaries.
These artifacts are local evidence, not tracked product files.
