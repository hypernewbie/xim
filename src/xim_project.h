#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Native project index. The owning thread calls these; results are owned by
 * the indexer and copied into the caller-owned string returned here. */
void xim_project_init(const char *root);
void xim_project_refresh(void);
void xim_project_shutdown(void);
char *xim_project_files(const char *query);
char *xim_project_explorer(const char *query);
int xim_project_explorer_activate(const char *item, char **selected);
const char *xim_project_status(void);
void xim_project_free(char *value);

#ifdef __cplusplus
}
#endif