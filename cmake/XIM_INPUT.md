# Native input contract

`xim` and `vim` share the C engine. Only `xim` links `xim_controller.cpp`,
`xim_commands.cpp`, and `xim_bridge.c`. `main.c` selects native dispatch at
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

Every prompt overlay invalidates the covered editor screen before the next
redraw. Palette, prompt and status groups have default links to existing
theme groups. A prompt places the terminal cursor at its input position.
Palette selection stays within the visible list on small screens. Failed
confirmation writes retain their error text in an ErrorMsg-colored overlay;
the pending operation runs only after a successful write or explicit discard.

Native defaults precede explicit configuration. No implicit native config
path exists yet; use `-u`. Runtime lookup uses the executable's sibling build
runtime or prefix-relative installed runtime, with VIMRUNTIME overrides.
The current executable-location implementation targets Linux (`/proc/self/exe`).

The boundary is synchronous and main-thread-only. Independent background
work must return owned results to the controller; workers must not access
borrowed strings, adapter functions, or Vim globals. No worker framework is
introduced for this milestone.

See `runtime/doc/xim.txt` for shortcuts, startup policy and compatibility gaps.
`xim_commands`, `xim_native_pty`, and `xim_native_screens` cover dispatch,
real-terminal workflows and rendered UI respectively. Screen tests are
registered with `BUILD_FULL_TEST=ON`; they are separate from the inherited
Make_all.mak target list.
