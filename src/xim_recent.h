#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Recent files and folders. Reads and writes run on a worker and merge
 * under a state-file lock. Rows are DISPLAY \t WIRE_PATH. */
int xim_recent_init(void);
void xim_recent_shutdown(void);
int xim_recent_ready(void);
unsigned long long xim_recent_serial(void);
/* Borrowed status for pending/failed persistence; valid until the next call. */
const char *xim_recent_status(void);
char *xim_recent_files(void);
char *xim_recent_folders(void);
void xim_recent_add_file(const char *absolute_path);
void xim_recent_add_folder(const char *absolute_path);
void xim_recent_remove(const char *absolute_path);
void xim_recent_clear(void);

#ifdef __cplusplus
}
#endif
