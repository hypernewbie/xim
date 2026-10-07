#pragma once

/* Decoded input only. No Vim globals or worker-thread access across this API. */
#include <stddef.h>
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
int xim_prompt_active(void);
int xim_background(void);
int xim_input_blocked(void);
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
int xim_engine_columns(void);
void xim_engine_boundary(void);
int xim_engine_unsaved(int all_buffers);
int xim_engine_named(void);
char *xim_engine_buffers(void);
/* Native menu and mouse boundary. Main thread only. */
int xim_menu_active(void);
unsigned xim_menu_capabilities(void);
void xim_engine_menu_bar(const char *text, int selected_col, int selected_width);
void xim_draw_menu_bar(void);
void xim_menu_bar_text(char *buf, size_t len, int columns, int *sel_col, int *sel_width);
void xim_engine_menu_popup(const char *items, int row, int col, int width, int rows,
        int selected_row, int error);
int xim_engine_has_selection(void);
int xim_engine_can_undo(int redo);
int xim_engine_get_option(const char *name);
void xim_engine_toggle_option(const char *name);
int xim_engine_click(int row, int col, int extend, int clicks);
void xim_engine_drag(int row, int col);
void xim_engine_drag_end(void);
void xim_engine_scroll(int row, int col, int direction);
void xim_engine_middle_paste(int row, int col);
void xim_mouse_press(int row, int col, int button, int modifiers, int clicks);
void xim_mouse_drag(int row, int col, int modifiers);
void xim_mouse_release(int row, int col, int button, int modifiers);
void xim_mouse_wheel(int row, int col, int direction, int modifiers);
void xim_mouse_move(int row, int col, int modifiers);
int xim_prompt_place(int screen_col);
void xim_prompt_get(int *cursor_cells, int *length_cells);
void xim_prompt_selection(int *start_cells, int *end_cells);
#ifdef __cplusplus
}
#endif
