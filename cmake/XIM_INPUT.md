# Native input contract

`xim` and `vim` share the C engine. Only `xim` links `xim_controller.cpp`,
`xim_commands.cpp`, `xim_menu.cpp`, `xim_project.cpp`, `xim_watch.cpp`,
`xim_fsops.cpp`, `xim_recent.cpp`, and `xim_bridge.c`. `main.c` selects native dispatch at
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

The top row is a native menu bar drawn through the tabline layout; the
inherited tab labels never appear in native mode. Headings, drop-downs and
context menus share one pure hit layout in `xim_menu`. F10 opens menus;
Ctrl-N creates a new buffer and Ctrl-Y remains redo. Prompts keep an owned
UTF-8 caret with Left/Right, Home/End, Delete, Select all, clipboard and
mouse placement. Picker activation reuses the last rendered rows so clicks
and Enter agree.

Find highlights every literal match through a window-local match list, not
'hlsearch', so `xim --vim` and the compatibility dispatch are untouched. It
reports a match count and a smart/insensitive/sensitive case rule at the
right of the prompt row and never moves the caret while typing. Buffer
matching folds case through the editor's Unicode rules; a compiled pattern is
reused per query and rule. The highlight is deleted from the window that
created it, which can differ from the focused window after a menu opened
another window. Alt-C cycles the rule; a terminal that reports modifiers
delivers Alt-C with its modifier preserved at the seam (the inherited
modifyOtherKeys fold is bypassed for native dispatch), and a terminal without
it delivers Escape followed by 'c', which the seam claims as the case toggle
before treating Escape as a cancel. F3 and Shift-F3 navigate while a find
prompt is open, and F10 opens the menu bar over any prompt. Replace edits a
find field and a replace field on one line; Tab and Shift-Tab move between
them. Replace-all and replace-one run as one undo block; a replacement with
newlines splices real lines through an `u_save` range so one undo restores
the whole change. A read-only or non-modifiable buffer refuses the change and
the prompt enters its error state.

Mouse input reuses the inherited SGR decoder; no second parser and no modal
key injection exist. Click, Shift-click, drag, double/triple-click, wheel
and middle/right buttons run through owning-thread adapters using
`mouse_find_win`, `mouse_comp_pos` and `coladvance`; the wheel never moves
the caret or selection. Right-click opens Edit choices without clearing
selection; middle-click positions then pastes through the clipboard
boundary. Press-drag-release pairs press/drag/release; a terminal resize
during a capture keeps the selection and redraws the frame. A separator
press arms `win_drag_status_line` or `win_drag_vsep_line` and the document
release clears the capture. Passive motion reporting is enabled only while
a menu is open. Narrow bars clip trailing headings but keep every command
reachable by keyboard.

Native defaults precede explicit configuration. No implicit native config
path exists yet; use `-u`. Runtime lookup uses the executable's sibling build
runtime or prefix-relative installed runtime, with VIMRUNTIME overrides.
The current executable-location implementation targets Linux (`/proc/self/exe`).

Engine adapters remain synchronous and main-thread-only. The project module
has a scanner with an owned root and a path matcher with an immutable shared
path snapshot; a third content matcher reads the indexed files and starts on
the first project search. A separate explorer worker owns directory pages, the
64 MiB directory cache and one immutable flattened view. Each directory page
holds at most 200 entries; a Load more row requests the next page, and page
tokens detect a directory change between requests. The owning thread consumes
published row text, path identity, kind, depth, flags and revision; it never
walks, sorts or reads an ignore file. Invalidations are queued under the state
lock, and large retired indexes are freed by workers outside it. Requests
coalesce and completion uses one latest-result slot plus a scan-change flag.
Generations and query revisions reject obsolete results, with cancellation
checks during walking, path matching and file reads. A content request carries
its generation; a completed result is served only while that generation is
still current, so a refresh invalidates it and the prompt shows the pending
state until the new scan publishes. Content rows serialize outside the state
mutex, and each row carries a bounded display excerpt (clamped on UTF-8
boundaries with ellipsis markers) next to the true line and column used for
the jump. The content worker waits for a completed scan, skips files larger
than 1 MiB and files with a NUL byte in the first block, and caps results at
1000 matches or 200 files. It reports skipped and truncated counts as one
summary row rather than per file.

The Linux watcher service owns inotify descriptors and watch records; it never
accesses Vim globals or buffer pointers. Notifications return through the
project wake callback, not terminal input or key injection. Project watches
cover included directories, new subtrees and ignore-file changes; explicit
file opens watch the parent and filter unrelated names. Events coalesce for 50
ms with a 250 ms maximum delay. Raw events and the dirty set are capped at
4096 paths or 1 MiB before publication. Moved/deleted directory watches lose
old-path metadata, and the scanner installs destination watches before walking
their children. Ordinary events patch affected index directories and explorer caches
off-thread; overflow, root loss, ignore-file changes and an explicit refresh
request one reconciliation. Requested and published namespace revisions keep
path/content activation pending until that reconciliation is current. Index
patches borrow an immutable snapshot captured under the state lock. Watch failure is visible with its cause and
leaves manual Refresh project usable. The backend interface permits another
platform implementation without changing editor-state ownership.

External changes are observations, not permission to overwrite text. A clean,
current UTF-8 buffer under the 256 KiB reload cap stages owned disk bytes on a
worker. The owning-thread adapter checks buffer number, name, edit tick, disk
identity and current-buffer focus again before applying them; typing during
the read becomes a conflict. Dirty, hidden, oversize, binary or non-UTF-8
buffers receive a changed notice instead. A dirty reload requires explicit
consent, and deletion marks the retained buffer text modified with a missing
notice. Save and confirmation saves require Recreate consent bound to the
missing buffer numbers and paths, including hidden files during Quit. A changed
target or a cancelled choice cannot silently run the old continuation. An external
rename is accepted only when the old name is gone and the destination version
equals the last accepted buffer version.

Native file operations use a separate serialized worker. Preflight and commit
anchor traversal to directory descriptors, reject absolute and `..`
destinations, do not follow directory symlinks, create exclusively and rename
without replacement. Create, rename, move and permanent delete return a state
sequence; the result remains owned until the controller acknowledges the
engine buffer update and reconciliation. Recursive delete reports partial
progress instead of claiming rollback, and it never closes or discards buffer
text. There is no trash, filesystem undo or cross-filesystem copy-and-delete
move.

Recent files and folders persist as JSON under `$XDG_STATE_HOME/xim` or
`$HOME/.local/state/xim`. A dedicated worker reads and writes the file, merges
under `flock`, writes a private temporary file and renames it atomically, with
directory mode 0700 and file mode 0600. The owning thread consumes a published
serial and never blocks on recents I/O. Open folder validates a root through
the operation worker before the current project changes. Close project stops
the index, content search and project watches while retaining buffers,
explicit-file watches and recents. Old picker continuations are cleared before
a switch, so deferred input cannot replay into the new project.

Refresh never joins the old scan on the editor thread. Retired large indexes
are freed by workers outside the state mutex, not during a main-thread refresh
or a completion-slot update. Shutdown cancels first, then joins all workers; a
blocking filesystem call can still delay shutdown.

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
adapter functions, Vim globals or rendering. Explorer listing, directory-page
requests, watch reconciliation, staged reads, file operations and recents I/O
run on their own workers; only their published results reach the owning
thread.

See `runtime/doc/xim.txt` for shortcuts, startup policy and compatibility gaps.
`xim_commands`, `xim_menu`, `xim_project`, `xim_fsops`, `xim_recent`,
`xim_native_pty`, and `xim_native_screens` cover dispatch, menu
registry/geometry/hit testing, project ownership/cancellation/ranking/watch
reconciliation, filesystem operations, recents persistence, real-terminal
workflows and rendered UI respectively. Screen tests are
registered with `BUILD_FULL_TEST=ON`; they are separate from the inherited
Make_all.mak target list.
