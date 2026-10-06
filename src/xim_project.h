#pragma once

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
/* Nonblocking: requests a query and returns only its current completion. */
char *xim_project_files(const char *query);
char *xim_project_explorer(const char *query);
int xim_project_explorer_activate(const char *item, char **selected);
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