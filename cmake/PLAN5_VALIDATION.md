# Plan 5 local validation — 2026-10-14 (closeout re-run)

Source: Plan 5 working-tree changes over the Plan 4 commit, including the
closeout fixes for the review findings in `temp/XIM_PLAN5_REVIEW.md`. Linux
UTF-8, Clang/Clang++ 22.1.8, libc++, C++26, CMake 4.3.4, Ninja 1.13.2.
Acceptance build: `build/dev`, Huge, RelWithDebInfo (`-O2`). Builds, tests and
timing runs were serial.

The operator authorized native find, replace and project content search. The
work stays in native mode on the Linux terminal and exposes only shipped
commands. No regular expressions, project-wide replace, symbol/definition
search or external tools (grep, ripgrep) are included.

## Implementation

- Find highlights every literal match while the query is typed and does not
  move the caret until navigation. Enter goes to the next match, Shift-Enter
  to the previous; F3 and Shift-F3 do the same while the prompt stays open
  and keep working after it closes; Escape closes and clears. The
  window-local match list uses a dedicated `XimFindMatch` group and never
  touches `'hlsearch'`, so `xim --vim` and the compatibility dispatch are
  unaffected.
- Matching is literal (`\V` very nomagic) with an explicit `\c`/`\C` case rule
  taken from the smart-case state, so it does not depend on `'ignorecase'`.
  The compiled pattern is cached per query and rule. Counting, highlighting
  and replacement share one span scan; when the query or the line needs
  Unicode folding, the scan runs through `vim_regexec_nl`, so a long s
  matches s and folded pairs of different byte length replace correctly. An
  ASCII fast path avoids the decode on the common path.
- Case is smart by default; Alt-C cycles smart, insensitive and sensitive,
  including non-ASCII uppercase in the sensitive trigger. A terminal that
  reports modifiers delivers Alt-C distinctly: the inherited
  modifyOtherKeys Alt fold is bypassed for native dispatch. A terminal
  without it delivers Alt-C as Escape followed by `c`; the input seam claims
  that pending letter as the toggle and any other key disarms the claim so a
  stale claim can never eat a later keystroke.
- Replace shows a find field and a replace field on one line. Tab and
  Shift-Tab move between them; the prompt-editing primitives act on the active
  field so both share one caret and selection model, and the caret cell is
  computed in display cells. Enter replaces every match in one undo block;
  Ctrl-R replaces the match at or after the caret and advances. A
  replacement with newlines splices real lines through an `u_save` range, so
  one undo restores the whole change and a saved file contains no NUL bytes.
  An empty replacement deletes whole-line matches. A read-only or
  non-modifiable buffer refuses and the prompt shows the error state.
- Search project runs on a third worker that starts on the first project
  search and reads the indexed files. It reuses the smart-case rule, waits for
  a completed scan, caps results at 1000 matches or 200 files, skips files
  larger than 1 MiB and files with a NUL byte in the first block, and reports
  the skipped and truncated counts as one summary row. Enter or a click opens
  the file at the match; the wheel moves the row selection; Enter pressed
  while the search runs opens the first result as soon as it arrives.
- Content results carry their generation: a project refresh invalidates
  completed results until the new scan publishes, so the prompt can no longer
  show a stale completed state for a changed tree. Rows serialize outside the
  state mutex, and each row carries a bounded UTF-8-clamped excerpt next to
  the true line and column, so a very long matched line cannot stall the
  editor.
- F10 opens the menu bar while any prompt is open. Help > Xim help opens
  `runtime/doc/xim.txt` (`:help xim.txt`), not the inherited X-input method
  document that `:help xim` matched.

## Validation

- The `xim_project` unit test covers the content worker: literal matches,
  the skip summary for oversize and binary files, the smart-case rule, the
  1000-match cap and the truncation row, refresh invalidating completed
  results (generation check), and bounded excerpts with the true jump column
  preserved for a 9000-character line.
- PTY checks (`cmake/test_xim.py`) add closeout regressions: multiline
  replacement (real lines, one undo, no NUL bytes after save), empty
  replacement deleting whole-line matches, Unicode count/replace agreement
  (long s, folded byte-length pairs, non-ASCII smart-case trigger), F3 and
  Shift-F3 inside the find prompt, modifyOtherKeys Alt-C, highlight cleanup
  after a Help-menu window switch (no E803, no leaked match), project wheel
  selection, early Enter activating the first result with later keys typing
  into the document, replace-field click placement, and the wide-character
  caret cell. The earlier Plan 5 checks keep their coverage.
- Screen dumps cover the find prompt with highlights (`Test_xim_find`), the
  two-field replace prompt (`Test_xim_replace`) and the results overlay with a
  header and a selected match (`Test_xim_project_search`). The Edit menu dump
  now includes Find and Replace.
- The review's caret-latency probe was re-run against the fixed binary on the
  25 MiB long-line fixture (50 × 512 KiB lines): caret round trips dropped
  from 118–121 ms to 0.25–0.45 ms, and RSS after publishing results dropped
  from 106 MiB to 13.8 MiB (`build/plan5-review/long_results_fixed.json`).
- The PTY harness `ex()` helper was corrected during closeout: it now sends
  the palette activation and the command as one sequential stream and waits
  only on the completion file. The previous intermediate wait could match a
  stale prompt row restored from a find-prompt snapshot and fire the command
  into the buffer. The `-R` snapshot check now waits for the buffer name so
  typing cannot race the argument-file read.

## Correctness gates

- Full `build/dev` CTest suite: 19/19 registrations pass, including the
  inherited Vim-script batches, native unit tests, project tests, PTY checks
  and the screen functions.
- Native suites: `build/dev` 5/5. ASan/UBSan native 4/4 and TSan native 4/4
  (commands, menu, project, PTY), rebuilt from clean configure. The sanitizer
  configurations do not register the inherited screen suite.
- Fresh tracked-source export (`build/plan5-clean`): build, unit tests and
  generated-header routing pass.
- Installed candidate under `build/plan5-install`: all PTY workflow checks
  pass from private directories without runtime or configuration overrides;
  the project comparison re-measured on the installed binary: warm query to
  visible result 4.686/5.088 ms (Plan 4 5.067/5.452), first accepted edit
  19.771/20.014 ms (Plan 4 19.688/20.398).
- No compiler warnings in successful final builds. `git diff --check` passes.

## Completed performance

100000-line fixture (1000 `needle` matches), 24x100 xterm-256color, warm
process, 30 samples, serial runs. `find` and `replace` completion is the
rendered prompt state; `project search` completion is the first result row
over a 10000-file project. The replace measurement uses a replacement that
stops matching; the previously recorded 5.125/5.146 ms numbers are invalid
(the replacement still matched under the insensitive rule, and the old
completion predicate matched a substring of the stale "1000 matches" state).

| Measurement | Median ms | p90 ms | Local p90 budget |
| --- | ---: | ---: | ---: |
| Find, 6 keystrokes to highlight (100k lines) | 40.091 | 41.787 | 60 |
| Replace-all, 1000 matches (100k lines) | 48.105 | 49.913 | 50 |
| Project search, warm index (10k files) | 84.194 | 87.255 | 150 |

Project search, cold index (process start through first result; the timer
begins before the process exists): 109.533 ms.

- The find cost is one full-buffer scan per keystroke; the ASCII fast path
  keeps the six-keystroke query near 40 ms. The plan's risk note about
  capping highlight work applies to buffers far larger than the 100k-line
  target; those are a declared boundary.
- Idle with a completed project search: 0 output bytes and 0 process CPU
  ticks in one second (100 ticks/second). This is a short idle sample, not a
  long-running memory or CPU guarantee.

## Boundaries

- Alt-C and Ctrl-H require a terminal that reports them distinctly. A terminal
  that maps Backspace to `^H` makes the decoder deliver `K_BS`, so Backspace
  wins and Ctrl-H does not open Replace; Edit > Replace and the palette remain
  available. `Ctrl-Shift-F` likewise may need the menu or palette.
- Project search reads on-disk text. Unsaved buffer changes are not searched;
  save the buffer first.
- Project content matching folds ASCII and Latin-1 case; other scripts compare
  case-sensitively. The buffer find uses Vim's full Unicode folding, so the
  two paths can differ for non-Latin text.
- A file argument disables the project, so project search runs only for a
  directory or a bare start.
- Content results publish once per query; the plan's incremental publication
  idea remains deferred with the operator.
