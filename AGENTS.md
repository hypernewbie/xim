# AGENTS.md

Guidance for AI coding agents working in Xim, a fork of Vim.

## Project

This repository is **Xim**, not an upstream Vim checkout. Vim is the starting
codebase, not the product specification. Upstream is https://github.com/vim/vim.
Some components are vendored, including `src/xdiff` and `src/libvterm`.

The vision is a one-run, instant-start VS Code-like terminal editor, with a
Sublime-like feel, partial Vim plugin compatibility, and support for Vim themes.
Non-modal editing is the intended default. Essential capabilities belong in
the editor, not a required plugin stack. Treat plugin implementations as UX
prototypes, not the production architecture. The future architecture excludes
LSPs. Native code must also avoid unnecessary work.

The performance ambition is 20x faster startup and operation than comparable
plugin-heavy Vim setups and most Neovim preconfigs. This is a target, not a
measured claim. Define reference configurations and workloads before claiming
an improvement. Use multicore hardware for independent tasks. Profile throughout
development, with correctness and performance tests at every stage.

Xim will diverge from Vim's architecture, build system, and portability policy.
Do not preserve obsolete compiler support at the expense of development.
`runtime/doc/develop.txt` explains inherited code and conventions. Its upstream
product, release, and compiler policies do not override this file.

Local notes are in `temp/XIM_DESIGN.md`, `temp/XIM_PLAN0.md`,
`temp/XIM_PLAN0_REVIEW.md`, and `temp/XIM_PLAN1.md`. Read them when present.
`temp/` is gitignored, so these files can be absent in other checkouts.
The requirements in this file do not depend on those notes.

## Toolchain direction

- Use Clang for C and C++ on all supported platforms.
- Do not add GCC/G++ or MSVC compiler build paths.
- Prefer libc++ where practical. On Windows, use clang-cl with the MSVC ABI
  and MSVC library/runtime/SDK components where required. The compiler remains
  Clang/LLVM.
- Use CMake with the Ninja generator. Implement real source targets, not a
  permanent wrapper around legacy Make.
- Use current C++ for development productivity, including AI-assisted
  development. Plan 0 targets the installed compiler's C++26 mode, not complete
  support for every draft-standard feature.
- Choose explicit minimum tool versions. Do not constrain new code to ancient
  compiler capabilities.

## Current milestone: Plan 1 native editing

Plan 0 closeout fixes and the operator-authorized Plan 1 native editing layer
are implemented. Evidence is in `cmake/PLAN0_CLOSEOUT.md` and
`cmake/PLAN1_VALIDATION.md`. Keep these gates in effect for subsequent changes:

- Build from tracked sources without Make-generated source-tree headers.
- Use the selected build's generated headers, not stale reference artifacts.
- Apply the shared compiler policy to the core and its consumers.
- Measure completed operations, not echoed command text.
- Repeat full tests and corrected performance comparisons after fixes.
- Preserve enabled features, runtime behavior, and inherited C compilation.
- Record baseline failures separately. Accept no new failures, compiler warnings,
  or reproducible performance regressions.

`xim` uses native non-modal input; `vim` and `xim --vim` retain compatibility
dispatch. Read `cmake/XIM_INPUT.md` before changing the input boundary. Editor
state stays on its owning thread. Further product slices require operator
authorization; a plan file alone is not permission to change behavior.

Large scope does not justify reducing the vision. Milestone boundaries separate
build parity from later product changes.

## Build and test

CMake/Ninja is the primary build path. The `default`, `debug`, `release`,
`asan`, and `tsan` configure/build presets exist. Use the tracked-source export
check described in `cmake/README.md` to verify clean-build header routing.

    # Primary build and registered unit tests (from the repository root):
    cmake --preset default
    cmake --build --preset default --parallel
    ctest --preset default

    # Enable the full inherited Vim-script suite:
    cmake --preset default -DBUILD_FULL_TEST=ON
    cmake --build --preset default --parallel
    ctest --preset default

The inherited commands remain available for reference comparisons:

    # Reference build on Unix/Linux (from src/):
    make CC=clang

    # Run the inherited full test suite (from src/):
    make test CC=clang

    # Generate proto files (from src/, only when needed):
    make proto

    # Run a single test file (from the repository root):
    cd src/testdir && make test_name.res

Inherited test output is in `src/testdir/messages` and `src/testdir/test.log`.
During migration, run tests against the candidate binary and matching runtime,
not a system Vim or a stale reference binary.

See `cmake/README.md` for build, sanitizer, and performance commands.
Legacy platform install documents describe upstream builds, not Xim's
supported compiler policy.

Before submitting any patch, at minimum:
1. The build succeeds without new warnings.
2. Relevant tests pass.
3. The code matches the style of the file being edited.

## Layout

- `src/` - inherited C source and future C++ components. Subsystem names are
  usually obvious from filenames
  (`buffer.c`, `window.c`, `search.c`, `vim9compile.c`, etc.).
- `src/proto/` - function prototypes, one `.pro` file per source file.
  Regenerated; do not hand-edit unless you know what you're doing.
- `src/po` - Translations
- `src/xxd` - for the xxd subproject
- `src/xdiff` - for the xdiff library (imported from git)
- `src/libvterm` - for the libvterm library
- `src/testdir/` - tests. Vim-script files named `test_*.vim`.
  Screendump expected output lives in `src/testdir/dumps/`.
- `runtime/doc/` - user-facing documentation in Vim help format, when updating,
  also update the Last Change header
- `runtime/syntax/generator` - Syntax script for Vim Script, automatically generated
  from Vims source
- `runtime/`  - runtime files shipped with Vim, when updating, also update the
  Last Change header and a short description if this file has no maintainer
  Preserve maintainer and license information. Coordinate shared fixes with
  upstream maintainers, but make Xim-specific changes in this repository.
- `src/version.c` - contains upstream's `included_patches[]` history.
  Do not invent upstream patch numbers or add entries for Xim-only changes.

## Commit format

For Xim commits, use a concise subject and explain the change and its tests.
Include `Signed-off-by:` for the DCO and `Assisted-by:` when AI was used.
Do not label Xim commits as numbered upstream Vim patches.

For patches intended for Vim upstream, use its strict format. The subject
line is a one-sentence **problem statement**, not a description of the fix:

    patch 9.2.NNNN: short description of the problem

    Problem:  Restatement of the problem as a full sentence, possibly
              with a reporter attribution in parentheses.
    Solution: Short description of the fix, ending with the author's
              name in parentheses.

    optional longer description of the problem and solution goes here in prose.
    Do not use bullet points.

    fixes:   #NNNN
    related: #NNNN
    closes:  #NNNN

    Assisted-by: <AI tool>
    Co-authored-by: Name
    Signed-off-by: Author Name <email>

Rules for upstream submissions:

- **Subject line states the problem**, not the solution. "fix typo" is
  wrong; "typo in foo() causes OOB read" is right.
- **Problem line is a full sentence with a trailing period.** It mirrors
  the subject.
- **Solution line ends with `(Author Name)`** — parentheses, period
  after them.
- **Longer prose**, if any, goes after the Problem/Solution header
- **`fixes:` references the issue** the patch fixes.
  **`closes:` references the PR** that introduces the fix.
  **`related:` references related issues**, including issues that caused this
  one.
  All can appear. Colon, aligned, no trailing period.
- **`Signed-off-by:` is required** for the Developer Certificate of Origin (DCO).
- **`Co-Authored-by:` is allowed** and is the accepted way to
  acknowledge human assistance transparently. Human coauthors should usually
  also have their own Signed-off-by.
- **`Assisted-by:` is required** when AI was used.

## Existing C code conventions

Apply these conventions to inherited C files. They do not impose Vim's legacy
language restrictions or naming rules on new C++ components. Preserve local
style unless the task explicitly changes it.

- **Indentation is 4 spaces per level.** Existing files use tabs with
  `ts=8 sts=4 sw=4 noet` (set by the modeline in the file),
  so tabs of width 8 appear where two levels of indent collapse. `sign.c`,
  `sound.c`, and any new file must use spaces only and follow the style from
  the .editorconfig file.
- **Opening braces go on their own line (Allman style)** — for function
  definitions and for control-flow constructs (`if`/`else`/`for`/`while`/
  `do`) alike.
- **Function definitions**: return type on its own indented line, with
  the function name beginning on the next line.
- Initialize locals where a reader cannot trivially see the first
  assignment (common for pointers and return-value accumulators).
  Don't add `= 0` initializers for values that are always assigned
  before use — they can hide real uninitialized-read bugs from
  the compiler.
- `for (int i = 0; ...)` loop declarations are fine in files that
  use them; older files may declare the counter at the top of the
  block.
- **Function-scope declarations at the top of a block** is the historical
  style, but mid-block declarations are acceptable in files that have
  adopted them. Match the surrounding code.
- **Custom types end in `_T`** (e.g., `buf_T`, `linenr_T`, `pos_T`).
  Never use `_t` — it collides with POSIX typedefs.
- Keep inherited C sources compiled as C during Plan 0. Match the surrounding
  code instead of doing unrelated language conversions. Xim does not require
  Compaq C, OpenVMS, or upstream's C95 compatibility limits.
- **`bool` / `true` / `false` are acceptable.** Vim is transitioning
  from `int` with `TRUE`/`FALSE` to C99 `bool`. Do not "fix" `bool`
  back to `int`. Within a single patch, be consistent — don't mix
  `true` and `TRUE` in new code.
- **Do not mass-convert** `TRUE`/`FALSE` to `true`/`false` across files
  unless that is the patch's explicit purpose. Opportunistic
  conversions create noise in diffs.
- **`STRLEN_LITERAL("...")`** should be used when the length of a
  string literal is needed. Avoid `STRLEN()` on literals.
- **`vim_snprintf_safelen()`** returns the written length; prefer it
  over `vim_snprintf()` when the length is then needed.
- **Prefer `dict_add_string_len()`** when the string length is already
  known, over `dict_add_string()` which calls `STRLEN()`.
- **String/buffer parameters go `(char_u *buf, size_t buflen)`** —
  length alongside pointer, in bytes. Use `size_t` for byte counts,
  `int` only where required by legacy APIs.
- **Guards before divisions.** Check for divisor zero explicitly, even
  when a composite earlier guard would prevent it. Relying on
  transitive guards is fragile.
- When introducing new allocations, verify the cleanup paths handle all exit
  conditions (early return, error branches, etc).

**Use Vim wrappers instead of libc where one exists:**

| libc          | Vim                    | Why                         |
|---------------|------------------------|-----------------------------|
| `free()`      | `vim_free()`           | Tolerates NULL              |
| `malloc()`    | `alloc()` / `lalloc()` | Checks for OOM              |
| `strcpy()`    | `STRCPY()`             | Cast for `char_u *`         |
| `strchr()`    | `vim_strchr()`         | Handles special characters  |
| `strrchr()`   | `vim_strrchr()`        | Handles special characters  |
| `memcpy()`    | `mch_memmove()`        | Handles overlapping copies  |
| `bcopy()`     | `mch_memmove()`        | Handles overlapping copies  |
| `memset()`    | `vim_memset()`         | Uniform across systems      |
| `isspace()`   | `vim_isspace()`        | Handles bytes > 127         |
| `iswhite()`   | `vim_iswhite()`        | TRUE only for tab and space |

Use `runtime/doc/develop.txt` for inherited C style. Apply the references here
within that scope, not as portability requirements for new Xim code:

- `*style-names*` — reserved name patterns (`is*`, `to*`, `str*`, `mem*`,
  `wcs*`, `.*_t`, `__.*`) and inherited naming conventions. The historical
  31-character function-name limit does not apply to Xim.
- `*style-spaces*`, `*style-examples*` — spacing and one-statement-per-line.
- `*style-various*` — `FEAT_` feature prefix, uppercase `#define`,
  `#ifdef HAVE_X` rather than `#if HAVE_X`, no `'\"'`.
- `*assumptions-makefiles*` — guidance for inherited reference Makefiles only.
  New build work uses CMake/Ninja, not these Makefile restrictions.
- Vim uses `char_u` instead of `char` type
- Vim uses the macros `STRLEN`, `STRCPY`, `STRCMP`, `STRCAT` that work
  with the `char_u` type.
- `*style-clang-format*` — `sign.c` and `sound.c` are formatted with
  `clang-format`; re-run it after editing those files.

## New C++ components

- Use the selected modern C++ mode and supported Clang features.
- Use clear ownership and RAII for resources. Preserve allocator contracts at
  existing C boundaries.
- Match project formatting. Do not copy C-only wrapper and naming requirements
  into C++ without a reason.
- Keep language migration separate from behavior changes. Add correctness
  tests and performance measurements for each migration step.

## Vim9 script conventions (in tests and runtime files)

- Write modern Vim style (new files can use Vim9 script, but compatibility
  with Neovim and other forks is a concern, so in doubt please ask!)
- **Drop `l:` prefix from local variables** in Vim-script tests.
- **Don't add `CheckFeature` inside individual tests** if it's already
  at the top of the file.
- If a test file doesn't gate features at the top, add CheckFeature to
  individual tests that depend on specific build features.

## Test conventions

- Tests are in `src/testdir/test_*.vim`.
- Reproducible tests beat "it doesn't crash" tests. If a patch fixes
  a rendering bug, add a screendump test. If it fixes incorrect output,
  assert the output.
- Add comprehensive tests for newly added features and include them
  in existing tests if possible
- **Screendump tests** use `CheckScreendump`, `RunVimInTerminal`,
  `VerifyScreenDump`, and live dumps in `src/testdir/dumps/`.
- `v9.CheckScriptSuccess(lines)` / `v9.CheckScriptFailure(lines, error, lnum)`
  are the standard way to test Vim9 script behavior at script-load time.
- When fixing a bug reported as an issue, include a test that
  reproduces the original report, not just a minimal synthetic case.
- Tests for Syntax runtime are in `runtime/syntax/testdir`
- Tests for Indent runtime are in `runtime/indent/testdir`

## Common gotchas

- **Distinguish what code enforces from what docs claim.** If a patch
  changes documented behavior, say so in the Problem/Solution.
- **Generated files** (`src/auto/configure`, generated Wayland protocol
  C, etc.) should only be regenerated when their source changes.
  Mixing unrelated regeneration into a functional patch creates noise.

## Documentation

- User-facing option or feature changes require a `runtime/doc/*.txt`
  update in the same patch.
- When editing an existing help file, bump the `Last change:` header
  at the top.

### Help file style

See `runtime/doc/helphelp.txt` (`*help-writing*`) for the authoritative
reference. Key conventions:

- **File header**: first line is `*filename.txt*` then a tab then a
  short description. That description appears under `LOCAL ADDITIONS`
  in `help.txt`. The version and `Last change:` date go on the second
  line, right aligned.
- **Modeline**: every help file ends with a Vim modeline — typically
  `vim:tw=78:ts=8:noet:ft=help:norl:`.
- **Layout**: `'textwidth'` 78, `'tabstop'` 8, indent and align with
  tab characters. Two spaces between sentences. Run `:retab`
  (not `:retab!`, and review the diff) after editing.
- **Tags** are defined as `*tag-name*`, usually right-aligned on the
  line where the thing they name is introduced. Tag names must be
  unique across all of `runtime/doc/`; for plugin help, prefix with
  the plugin name.
- **Cross-references inside help text**:
    - `|tag-name|` — hot-link to an existing tag.
    - `` `:cmd` `` — Ex command, highlighted as a code block.
    - `'option'` — option name, in single quotes.
    - `<Key>` or `CTRL-X` — special keys.
    - `{placeholder}` — user-supplied argument.
- **Sections** are separated by a line of `=` starting in column 1.
  Column or subsection headings end with `~` to trigger heading
  highlighting.
- **Code blocks** start with `>` at the end of the introducing line
  and end with `<` as the first non-blank on a later line (any line
  starting in column 1 also implicitly closes the block). Use `>vim`
  (or another language name) to request syntax highlighting inside
  the block.
- **Notation** — `Note`, `Todo`, `Error` and a few similar words are
  auto-highlighted; do not try to fake the highlighting by other means.
- **Language**: gender-neutral language is preferred for new or updated
  text; existing wording does not need to be rewritten for this alone.

## Compatibility and release scope

Xim does not inherit Vim's stability periods or release restrictions.

- Preserve existing behavior during Plan 0.
- For later architecture changes, support Vimscript and plugins on a
  best-effort basis. Define the compatibility boundary through tests.
- Preserve Vim colorscheme support as part of the product direction.
- Document compatibility gaps and user-visible changes. Do not claim complete
  Vim compatibility.

## Security

Before reporting a suspected security issue or submitting a patch
that touches security-sensitive code, read `SECURITY.md`. Follow
the disclosure process described there.

## Before submitting

1. Commit message uses the appropriate Xim or upstream format.
2. All modified code compiles without new warnings.
3. Tests pass, and new functionality has regression tests.
   Build and migration changes also have comparable performance results.
4. Documentation is updated for user-visible changes.
5. Signed-off-by is present.
6. Diff contains only changes relevant to the stated problem —
   no stray whitespace fixes, no unrelated refactors, no unrelated
   regeneration of `auto/configure`.
7. For multi-patch series: each commit compiles and passes its own
   tests. A known-broken intermediate state that a later patch fixes
   is not acceptable — squash instead.

## When in doubt

- Make the smallest possible change to achieve the goal. Do not rewrite
  entire files or functions when a targeted edit suffices.
- Read surrounding code and match its style rather than imposing an
  "improvement."
- Err toward smaller, more focused patches. A patch that does three
  things is three patches.
- If a patch fixes a symptom of a deeper bug, say so in the Problem
  and acknowledge the scope limitation in the Solution.
- Before claiming a bug exists, reproduce it. Before claiming code does X, read
  the code. Do not rely on training-data memory of file contents.
- Before running shell commands that modify files outside the working tree,
  install packages, push branches, or invoke network operations, confirm with
  the user.
