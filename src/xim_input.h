#pragma once

/* Decoded input only. No Vim globals or worker-thread access across this API. */
enum xim_key
{
    XIM_LEFT = 0x110000, XIM_RIGHT, XIM_UP, XIM_DOWN, XIM_HOME, XIM_END,
    XIM_PAGE_UP, XIM_PAGE_DOWN, XIM_DELETE, XIM_BACKSPACE,
    XIM_F1, XIM_F2, XIM_F3, XIM_F4, XIM_F5, XIM_F6,
    XIM_F7, XIM_F8, XIM_F9, XIM_F10, XIM_F11, XIM_F12,
    XIM_IGNORE
};
enum xim_modifier { XIM_SHIFT = 1, XIM_CTRL = 2, XIM_ALT = 4 };

#ifdef __cplusplus
extern "C" {
#endif
void xim_prepare_args(int *argc, char ***argv);
void xim_initialize(void);
void xim_step(void);
void xim_dispatch(int key, int modifiers);
void xim_render(void);
void xim_accept_paste(const char *text);

/* Engine adapters: main-thread only, borrowed strings valid for the call. */
void xim_engine_move(int key, int select);
void xim_engine_select_all(void);
void xim_engine_cancel(void);
void xim_engine_insert(int codepoint);
void xim_engine_paste(const char *text);
void xim_engine_delete(int backward);
void xim_engine_undo(int redo);
char *xim_engine_copy(int cut);
char *xim_engine_clipboard(void);
void xim_engine_free(char *text);
int xim_engine_command(const char *command, const char *argument, int preserve_position);
const char *xim_engine_error(void);
void xim_engine_find(const char *text, int backward);
void xim_engine_overlay(const char *prompt, const char *items, int error);
int xim_engine_menu_rows(void);
void xim_engine_boundary(void);
int xim_engine_unsaved(int all_buffers);
int xim_engine_named(void);
#ifdef __cplusplus
}
#endif
