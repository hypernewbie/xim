#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Linux inotify watcher. Project directories and the parent directories of
 * explicitly opened files are watched. All notifications arrive on one
 * watcher thread; the editor is woken through the project wake path. */

enum xim_watch_kind
{
    XIM_WATCH_CREATE = 1,
    XIM_WATCH_MODIFY = 2,
    XIM_WATCH_DELETE = 4,
    XIM_WATCH_MOVE = 8,
    XIM_WATCH_ATTRIB = 16
};

enum xim_watch_flags
{
    XIM_WATCH_OVERFLOW = 1,   /* kernel queue overflow: rescan required */
    XIM_WATCH_ROOT_LOST = 2   /* project root deleted, replaced or unmounted */
};

typedef struct
{
    int kind;                    /* bit set from xim_watch_kind */
    const char *path;            /* absolute path, valid during the call */
    const char *other;           /* move destination, or NULL */
    int is_directory;
    unsigned long long version;  /* stat identity after the batch, 0 unknown */
} xim_watch_event;

typedef void (*xim_watch_callback)(void *context,
        const xim_watch_event *events, size_t count, int flags);

/* Install or replace the project root. The root watch is installed
 * immediately; the scan worker adds one watch per visited directory. */
int xim_watch_start_project(const char *root, xim_watch_callback callback,
        void *context);
/* Remove project watches and keep explicit-file watches. */
void xim_watch_stop_project(void);
/* Add one project directory watch (called by the scan worker). */
int xim_watch_watch_dir(const char *absolute_path);
/* Watch the parent directory of an explicitly opened file. */
int xim_watch_watch_file(const char *absolute_path);
void xim_watch_unwatch_file(const char *absolute_path);
/* Borrowed status for the current project: live, partial or empty. */
const char *xim_watch_status(void);
void xim_watch_shutdown(void);

#ifdef __cplusplus
}
#endif
