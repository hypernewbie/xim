# Plan 6 local validation — 2026-10-27

Source: Plan 6 stage commits `c9176daef` (explorer pages), `aaa3731e8`
(watcher and external changes), `ee6aa2195` (file operations) and `c63242219`
(recents, project switching and closeout fixes).  This validation record and
the user documentation are added by the documentation commit.  Plan 5 baseline
commit: `d8c2f80da`.  Linux UTF-8, Clang/Clang++
22.1.8, libc++, C++26, CMake 4.3.4, Ninja 1.13.2.  Acceptance build:
`build/dev`, Huge, RelWithDebInfo (`-O2`).  Builds, tests and timing runs were
serial unless a parallel sanitizer gate is named.

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
  the buffer's last accepted disk version, which avoids treating Vim
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
  only for removal.  Open folder validates its root on the operation worker
  before the project changes; Close project stops indexing, content search and
  project watching while retaining buffers, explicit-file watches and recents.
- The menu bar is repainted as part of every overlay frame, so the active
  heading no longer depends on an unrelated tabline redraw.  Menu dumps anchor
  the terminal cursor to the last visible drop-down cell for deterministic
  screen output.

## Validation

- Full `build/dev` CTest suite: 21/21 registrations pass, including eight
  inherited Vim-script batches, inherited unit tests, the C++ link smoke test,
  native unit tests, PTY workflows and executed screen functions.
- Native suites pass in `build/default` (7/7) and `build/dev` (7/7).  Screen
  functions run in `build/dev`; `build/default` skips terminal/job-dependent
  screen coverage by design.
- ASan/UBSan native suites: 6/6 (commands, menu, recent, fsops, project, PTY).
  TSan native suites: 6/6 with `halt_on_error=1`.  TSan found two worker
  synchronization defects, both fixed: explorer invalidations were applied
  outside the state lock, and the recents worker read `stopping` and its queue
  without the lock during shutdown.  A project test helper also required a
  stable publish serial around each row copy to avoid accepting an older view
  after a newer serial.
- New screen dumps cover the explorer page after expansion, operation error,
  permanent-delete consent, changed-on-disk and missing-on-disk status and the
  recent-files picker.  Menu dumps now show the active heading and a
  deterministic cursor cell.
- Installed candidate under `build/plan6-install`: the full
  `cmake/test_xim.py` workflow passes from `/tmp/opencode/plan6-install-work`
  without a runtime or configuration override, including create, rename, move,
  permanent delete, watcher reload, dirty protection, project switching,
  recents and `xim --vim` compatibility.
- Fresh tracked-source export (`build/plan6-clean`): build, tests and
  generated-header routing pass.  See the gate result below.
- No compiler warnings in successful final builds.  `git diff --check` passes.

## Completed performance

Plan 6 gates use the proposed budgets from `temp/XIM_PLAN6.md`.  The fixture
tree and process conditions are recorded with each measurement.  No benchmark
ran during a build or another workload.  Fixture creation finished before
timing; `sync(2)` and one untimed create remove fixture writeback from the file
operation samples.

| Completed operation | Samples | Median ms | p90 ms | Budget ms |
| --- | ---: | ---: | ---: | ---: |
| Explorer movement with cached rows | 60 | 0.766 | 0.843 | 5 |
| First explorer page, 100000-child directory | 10 | 65.244 | 68.953 | 250 |
| External create visible, 10000-file tree | 30 | 56.646 | 56.929 | 500 |
| New file to disk, engine and visible completion | 30 | 5.464 | 6.016 | 100 |

The external-change figure includes the watcher's 50 ms coalescing window.  It
measures one external create, explorer refresh and rendered row; the matching
removal is drained between samples and is not timed.  The first-page timer
starts in a ready process immediately before Ctrl-E, so it excludes process
launch and the initial index scan.  The file-operation timer starts at the
final New-file Enter and ends when the opened buffer name or the Created notice
is visible; the disk entry is checked afterwards.

Startup and warm quick-open comparison, two serial rounds of 60 samples each,
same 100000-path fixture and terminal:

| Measurement | Plan 5 baseline p90 | Plan 6 p90 |
| --- | ---: | ---: |
| Warm quick-open query | 5.411 / 5.216 ms | 5.330 / 5.328 ms |
| First accepted edit during scan | 21.402 / 21.415 ms | 21.564 / 21.459 ms |

The two rounds overlap within observed run-to-run variance; no reproducible
regression appeared.  RSS after a completed quick-open run rose from 28924 kB
to 29524 kB, which is consistent with the watcher, operation and recents
workers.

A separate repeated-root-switch probe alternated two 200-file projects ten
times.  RSS stayed at 15104–15360 kB, file descriptors at 8, and inotify
watches at 2 after every switch.  The completed quick-open run also recorded 0
idle output bytes and 0 process CPU ticks in one second and reported no
periodic scan activity.

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
- The first-page timing excludes process startup.  Launch-to-first-edit remains
  covered by the separate startup comparison above.

## Gate result

Fresh tracked-source export (`build/plan6-clean`): passed.  The export built
with CMake/Ninja and Clang, its registered tests passed, and generated headers
from the export's `src/auto` were used rather than source-tree artifacts.
