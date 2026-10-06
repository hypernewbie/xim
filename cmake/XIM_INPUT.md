# Native input contract

`xim` and `vim` share the C engine. Only `xim` links `xim_controller.cpp`,
`xim_commands.cpp`, `xim_project.cpp`, and `xim_bridge.c`. `main.c` selects native dispatch at
the normal top-level input seam, after the existing redraw/deferred-event
work. Command-line windows and explicitly requested Ex execution retain
their inherited nested dispatch. `xim --vim` chooses the compatibility path.
Native builds need the normal or huge feature level for Vimscript and Visual
support. An explicitly selected tiny configuration builds compatibility Vim
and reports that the native target is unavailable.

The decoder remains `safe_vgetc()`, with mappings suppressed and terminal-key
decoding enabled. The bridge translates key codes and modifier bits into the
plain C interface in `xim_input.h`. Bracketed paste uses the existing raw
payload reader and passes one UTF-8 text payload to the controller. The C++
controller resolves actions and owns prompt, palette and internal clipboard
state. It does not queue modal key sequences.

Engine adapters call text, undo, selection, search and Ex file APIs on the
main thread. Selection uses half-open character positions; rendering uses
Vim's exclusive Visual selection. Typing groups end at navigation, newline,
commands or 256 characters. Paste is one operation. File paths are escaped
before invoking Ex file operations; allocation failure aborts the command.

Native prompt overlays retain their cells in the engine screen cache. Text
is painted before trailing cells are cleared; shrinking restores only exposed
rows from an owned screen-cell snapshot, and close invalidates the underlying
editor once. The renderer's final owning-thread callback recomposes overlays
on engine redraw and resize. Inherited synchronized updates are used only when
the terminal supports them. Palette, prompt and status groups link to existing
theme groups. A prompt places the terminal cursor at its input position.
Palette selection stays within the visible list on small screens. Failed
confirmation writes retain their error text in an ErrorMsg-colored overlay;
the pending operation runs only after a successful write or explicit discard.

Native defaults precede explicit configuration. No implicit native config
path exists yet; use `-u`. Runtime lookup uses the executable's sibling build
runtime or prefix-relative installed runtime, with VIMRUNTIME overrides.
The current executable-location implementation targets Linux (`/proc/self/exe`).

Engine adapters remain synchronous and main-thread-only. The project module
has two persistent workers: a scanner with an owned root and a matcher with
an immutable shared path snapshot. Requests coalesce and completion uses one
latest-result slot plus a scan-change flag. Generations and query revisions
reject obsolete results, with cancellation checks during walking and matching.
Refresh never joins the old scan on the editor thread. Retired large indexes
are freed by workers outside the state mutex, not during a main-thread refresh
or a completion-slot update. Shutdown cancels first,
then joins both workers; a blocking filesystem call can still delay shutdown.

A nonblocking close-on-exec pipe wakes the existing Unix select/poll wait.
The native top-level seam processes completions before entering safe_vgetc();
no completion is encoded as a key. Partial key sequences stay with the decoder.
The existing wait still services timers, channels, signals and resize, and
retains its one-time idle swap flush before blocking indefinitely. Ctrl-C is
copy input in native mode, including nonblocking reads before the decoder wait.

Enter during matching retains the exact query. Subsequent decoded keys/paste
are owned by a bounded 256-entry queue and replayed after activation. At queue
capacity further terminal bytes remain unread until completion. Escape clears
the pending choice and queued input. No worker accesses borrowed strings,
adapter functions, Vim globals or rendering. Explorer listing is still bounded
synchronous filesystem work; it is not part of the asynchronous matcher.

See `runtime/doc/xim.txt` for shortcuts, startup policy and compatibility gaps.
`xim_commands`, `xim_project`, `xim_native_pty`, and `xim_native_screens`
cover dispatch, project ownership/cancellation/ranking, real-terminal workflows
and rendered UI respectively. Screen tests are
registered with `BUILD_FULL_TEST=ON`; they are separate from the inherited
Make_all.mak target list.
