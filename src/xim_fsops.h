#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Native namespace mutations. Work runs on a serialized worker; the owning
 * thread submits relative paths, shows consent and applies buffer renames.
 * Preflight reserves the operation until it is committed or cancelled. */

enum xim_fs_kind
{
    XIM_FS_CREATE_FILE = 1,
    XIM_FS_CREATE_DIR = 2,
    XIM_FS_RENAME = 3,
    XIM_FS_MOVE = 4,
    XIM_FS_DELETE = 5,
    XIM_FS_VALIDATE_ROOT = 6
};

int xim_fsops_init(void);
/* Returns an operation id, or 0 when a mutation is already active. */
unsigned long long xim_fsops_preflight(int kind, const char *root,
        const char *source, const char *destination);
unsigned long long xim_fsops_commit(unsigned long long id);
void xim_fsops_cancel(unsigned long long id);
/* 0 pending, 1 ready, -1 unknown/failed submission. */
int xim_fsops_state(unsigned long long id);
int xim_fsops_phase(unsigned long long id);  /* 0 preflight, 1 commit */
int xim_fsops_busy(void);
char *xim_fsops_summary(unsigned long long id);
char *xim_fsops_error(unsigned long long id);
char *xim_fsops_message(unsigned long long id);
int xim_fsops_success(unsigned long long id);
/* Rename/move outcome: wire-encoded absolute old and new paths. */
int xim_fsops_renamed(unsigned long long id);
char *xim_fsops_old_path(unsigned long long id);
char *xim_fsops_new_path(unsigned long long id);
int xim_fsops_created_file(unsigned long long id);
/* Validated canonical root for XIM_FS_VALIDATE_ROOT. */
char *xim_fsops_resolved_root(unsigned long long id);
void xim_fsops_ack(unsigned long long id);
void xim_fsops_shutdown(void);

#ifdef __cplusplus
}
#endif
