#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Native project index. The owning thread calls these; results are owned by
 * the indexer and copied into the caller-owned string returned here. */
void xim_project_init(const char *root);
void xim_project_disable(void);
void xim_project_refresh(void);
void xim_project_shutdown(void);
int xim_project_wake_fd(void);
int xim_project_poll(void);
int xim_project_pending(void);
void xim_project_cancel_query(void);
/* Wake the editor from any worker without borrowing editor state. */
void xim_project_notify(void);
/* Nonblocking: requests a query and returns only its current completion. */
char *xim_project_files(const char *query);

/* Explorer view. Rows are one per line:
 *   DISPLAY \t PATH \t KIND \t DEPTH \t FLAGS \t REVISION
 * KIND is 'f' (file), 'd' (directory), 'm' (load more) or 's' (status).
 * FLAGS bit 0 is the expanded state of a directory. PATH is the stable row
 * identity: a project-relative path encoded for the wire (percent escapes for
 * '%', tab, CR and LF; "." is the root). DISPLAY is already sanitized for the
 * terminal and must never be parsed back into a path. The result is NULL
 * while the worker is building the current view; xim_project_explorer_pending
 * reports that state. */
char *xim_project_explorer_rows(void);
int xim_project_explorer_pending(void);
unsigned long long xim_project_explorer_serial(void);
/* Toggle directory expansion; path is a wire path from a row. */
int xim_project_explorer_toggle(const char *wire_path);
/* Request the next page for a directory (or the root with "."). */
void xim_project_explorer_more(const char *wire_path);
/* Current root and explorer/watcher status; borrowed and valid until the
 * next call on the same thread. */
const char *xim_project_root(void);
const char *xim_project_explorer_status(void);
/* Project-relative wire path to an absolute path the engine can open. */
char *xim_project_resolve_wire(const char *wire_path);
/* Decode a wire path without resolving it; caller frees with
 * xim_project_free. */
char *xim_project_wire_decode(const char *wire_path);

/* External change service. Watcher events are queued and drained by the
 * owning thread as rows:
 *   KIND \t VERSION \t ISDIR \t PATH \t OTHER
 * KIND is a xim_watch_kind bit set, paths are absolute wire paths and OTHER
 * is the move source (empty for other kinds). The result must be freed with
 * xim_project_free. */
char *xim_project_take_changes(void);
const char *xim_project_watch_status(void);
int xim_project_watch_file(const char *absolute_path);
void xim_project_unwatch_file(const char *absolute_path);
unsigned long long xim_project_file_version(const char *absolute_path);

/* Staged disk reads. The worker reads the file and verifies its identity
 * before and after; the owning thread applies the bytes. Returns 0 when the
 * read could not be queued. */
unsigned long long xim_project_stage_read(const char *absolute_path,
        size_t limit);
int xim_project_read_state(unsigned long long id);
char *xim_project_read_take(unsigned long long id, size_t *length,
        unsigned long long *version, int *binary);
void xim_project_read_discard(unsigned long long id);
/* Project content search. Nonblocking like xim_project_files; rows are
 * DISPLAY \t PATH \t LNUM \t COL with an empty PATH on a file header row.
 * The result must be freed with xim_project_free. */
char *xim_project_content(const char *query, int case_sensitive);
int xim_project_content_pending(void);
/* Monotonic count of published results; lets callers cache rows across
 * caret moves without missing a new completion. */
unsigned long long xim_project_content_serial(void);
const char *xim_project_content_status(void);
const char *xim_project_status(void);
void xim_project_free(char *value);

/* Resolve a project-relative path (or out-of-root marker + absolute) into
 * an absolute path that the editor's `:edit` command can open.  Returns
 * NULL when the path is already absolute.  The result must be freed with
 * xim_project_free. */
char *xim_project_resolve(const char *item);

#ifdef __cplusplus
}
#endif