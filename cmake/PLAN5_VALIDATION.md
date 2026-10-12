# Plan 5 local validation — 2026-10-11

Source: Plan 5 working-tree changes over the Plan 4 commit. Linux UTF-8,
Clang/Clang++ 22.1.8, libc++, C++26, CMake 4.3.4, Ninja 1.13.2. Acceptance
build: `build/dev`, Huge, RelWithDebInfo (`-O2`). Builds, tests and timing
runs were serial.

The operator authorized native find, replace and project content search. The
work stays in native mode on the Linux terminal and exposes only shipped
commands. No regular expressions, project-wide replace, symbol/definition
search or external tools (grep, ripgrep) are included.

## Implementation

- Find highlights every literal match while the query is typed and does not
  move the caret until navigation. Enter and F3 go to the next match,
  Shift-Enter and Shift-F3 to the previous; Escape closes and clears. The
  window-local match list uses a dedicated `XimFindMatch` group and never
  touches `'hlsearch'`, so `xim --vim` and the compatibility dispatch are
  unaffected.
- Matching is literal (`\V` very nomagic) with an explicit `\c`/`\C` case rule
  taken from the smart-case state, so it does not depend on `'ignorecase'`.
  A native scan counts matches for the prompt line and an ASCII fast path
  avoids the UTF-8 decode on the common path.
- Case is smart by default; Alt-C cycles smart, insensitive and sensitive.
  Terminals without modifyOtherKeys deliver Alt-C as Escape followed by `c`;
  the input seam claims that pending letter as the toggle and drops the
  swallowed key so it never reaches the prompt.
- Replace shows a find field and a replace field on one line. Tab and
  Shift-Tab move between them; the prompt-editing primitives act on the active
  field so both share one caret and selection model. Enter replaces every
  match in one undo block; Ctrl-R replaces the match at or after the caret and
  advances. A read-only or non-modifiable buffer refuses and the prompt shows
  the error state.
- Search project runs on a third worker that starts on the first project
  search and reads the indexed files. It reuses the smart-case rule, waits for
  a completed scan, caps results at 1000 matches or 200 files, skips files
  larger than 1 MiB and files with a NUL byte in the first block, and reports
  the skipped and truncated counts as one summary row. Enter or a click opens
  the file at the match.
- The overlay adapter grew an optional right-aligned info field and takes the
  caret cell from the controller, so a two-field prompt places the caret in
  either field and the terminal cursor stays where the design requires.

## Validation

- The `xim_project` unit test covers the content worker: literal matches,
  the skip summary for oversize and binary files, the smart-case rule, the
  1000-match cap and the truncation row.
- PTY checks (`cmake/test_xim.py`): find highlights without caret moves, smart
  case, the Alt-C cycle, Enter navigation, Escape cleanup and F3; replace-all
  over multibyte-capable matching with a single undo, Ctrl-R replace-one and
  the read-only refusal; project search over a fixture tree with smart case,
  the skip summary, Enter and click navigation. The existing native workflows
  keep their coverage.
- Screen dumps cover the find prompt with highlights (`Test_xim_find`), the
  two-field replace prompt (`Test_xim_replace`) and the results overlay with a
  header and a selected match (`Test_xim_project_search`). The Edit menu dump
  now includes Find and Replace.
- The Alt-C delivery was verified against the real decoder: an all-ASCII
  terminal reports Alt-C as `Escape` followed by `c`, which the seam claims.

## Correctness gates

- Full `build/dev` CTest suite: 19/19 registrations pass, including the
  inherited Vim-script batches (700.0 s), native unit tests, project tests,
  PTY checks and the screen functions.
- Native suites: `build/dev` 5/5 (commands, menu, project, PTY and the screen
  functions). ASan/UBSan native 4/4 and TSan native 4/4 (commands, menu,
  project, PTY). The sanitizer configurations do not register the inherited
  screen suite.
- ASan caught and corrected an off-by-one in the literal-pattern allocation
  before the final build; the fix is squashed into the find commit.
- Fresh tracked-source export (`build/plan5-clean`): 10/10 registrations pass;
  generated-header routing passes and no source-tree generated header is used.
- Installed candidate under `build/plan5-install`: all PTY workflow checks
  pass from private directories without runtime or configuration overrides.
- No compiler warnings in successful final builds. `git diff --check` passes.

## Completed performance

100000-line fixture (1000 `needle` matches), 24x100 xterm-256color, warm
process, 30 samples, serial runs. `find` and `replace` completion is the
rendered prompt state; `project search` completion is the first result row
over a 10000-file project.

| Measurement | Median ms | p90 ms | Local p90 budget |
| --- | ---: | ---: | ---: |
| Find, 6 keystrokes to highlight (100k lines) | 38.183 | 39.527 | 60 |
| Replace-all, 1000 matches (100k lines) | 5.125 | 5.146 | 50 |
| Project search, warm index (10k files) | 85.467 | 87.873 | 150 |

Project search, cold index (startup through first result): 110.457 ms.

The Plan 4 project comparison was re-measured on the same 100000-path fixture:
warm query to visible result 5.054/5.397 ms (Plan 4 5.067/5.452) and project
start to first accepted edit during scan 20.608/22.104 ms (Plan 4
19.688/20.398). The content worker starts lazily on the first project search,
so ordinary startup starts the same two workers as Plan 4; the remaining
difference is within run-to-run noise and below the 10% signal threshold.

- The find cost is one full-buffer scan per keystroke; the ASCII fast path
  cut it from 200 ms to 38 ms for the six-keystroke query. The plan's risk
  note about capping highlight work applies to buffers far larger than the
  100k-line target; those are a declared boundary.
- Idle with a completed project search: 0 output bytes and 0 process CPU ticks
  in one second (100 ticks/second). This is a short idle sample, not a
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
