/* Native Xim engine adapters. All functions run on the editor's main thread. */
#include "vim.h"
#include "xim_input.h"
#include "xim_project.h"

static pos_T selection_anchor;
static int selection_buffer = 0;
static int typing_count = 0;
static int overlay_visible = FALSE;
static int overlay_top = 0;
static int overlay_rows = 0;
static int overlay_columns = 0;
static int menu_visible = FALSE;
static int menu_top = 0;
static int menu_left = 0;
static int menu_height = 0;
static int menu_width = 0;
static int menu_rows = 0;
static int menu_columns = 0;
static int hover_enabled = FALSE;
static int xim_mouse_pressed = FALSE;
static pos_T xim_press_anchor;
static int xim_press_buffer = 0;
static int xim_press_dragging = FALSE;
// Owned separator drag state: the window whose status or vertical separator
// was pressed, the press offset inside it, and which separator it is.
static win_T *xim_sep_window = NULL;
static int xim_sep_is_status = FALSE;
static int xim_sep_offset = 0;
typedef struct
{
    char_u bytes[MB_MAXBYTES + 1];
    int attr;
} xim_cell_T;
static xim_cell_T *overlay_background = NULL;

    void
xim_engine_boundary(void)
{
    u_sync(TRUE);
    typing_count = 0;
}

    int
xim_engine_unsaved(int all_buffers)
{
    return all_buffers ? anyBufIsChanged() : curbufIsChanged();
}

    int
xim_engine_named(void)
{
    return curbuf->b_ffname != NULL;
}

/*
 * Return a newline-separated list of listed buffer names for the native
 * buffer picker.  The caller frees the result with xim_engine_free().
 */
    char *
xim_engine_buffers(void)
{
    buf_T *buf;
    size_t size = 1;
    size_t used = 0;

    for (buf = firstbuf; buf != NULL; buf = buf->b_next)
    {
	if (buf->b_flags & BF_DUMMY)
	    continue;
        size += 4 * STRLEN(buf->b_ffname != NULL
                ? buf->b_ffname : (char_u *)"[No Name]") + 32;
    }

    char_u *names = alloc(size);
    if (names == NULL)
	return NULL;
    names[0] = '\0';
    for (buf = firstbuf; buf != NULL; buf = buf->b_next)
    {
	if (buf->b_flags & BF_DUMMY)
	    continue;
        char_u *name = buf->b_ffname != NULL ? buf->b_ffname : (char_u *)"[No Name]";
        char_u *display = transstr(name);
        if (display == NULL)
        {
            vim_free(names);
            return NULL;
        }
        // Identity is the stable buffer number, not a truncated filename
        // or an ambiguous [No Name] label. Keep the path tail visible.
        int prefix = vim_snprintf_safelen((char *)names + used, size - used,
                "%d:%c%c ", buf->b_fnum, buf == curbuf ? '*' : ' ',
                bufIsChanged(buf) ? '+' : ' ');
        used += prefix;
        trunc_string(display, names + used, MAX(1, (int)Columns - prefix - 2),
                (int)MIN(size - used, INT_MAX));
        used += STRLEN(names + used);
        vim_free(display);
        names[used++] = '\n';
    }
    names[used] = '\0';
    return (char *)names;
}

    void
xim_engine_cancel(void)
{
    if (!VIsual_active && selection_buffer == 0)
        return;
    VIsual_active = FALSE;
    VIsual_select = FALSE;
    selection_buffer = 0;
    redraw_curbuf_later(UPD_NOT_VALID);
}

    static int
selection_op(oparg_T *op)
{
    if (!VIsual_active || selection_buffer != curbuf->b_fnum
            || EQUAL_POS(selection_anchor, curwin->w_cursor))
        return FALSE;
    CLEAR_POINTER(op);
    op->start = selection_anchor;
    op->end = curwin->w_cursor;
    if (LT_POS(op->end, op->start))
    {
        pos_T tmp = op->start;
        op->start = op->end;
        op->end = tmp;
    }
    op->motion_type = MCHAR;
    op->inclusive = FALSE;
    op->is_VIsual = TRUE;
    op->line_count = op->end.lnum - op->start.lnum + 1;
    return TRUE;
}

    static int
replace_selection(void)
{
    oparg_T op;
    if (!selection_op(&op))
    {
        xim_engine_cancel();
        return OK;
    }
    xim_engine_boundary();
    op.op_type = OP_DELETE;
    op.regname = '_';
    curwin->w_cursor = op.start;
    xim_engine_cancel();
    return op_delete(&op);
}

    static int
editable(void)
{
    if (!curbuf->b_p_ma || !undo_allowed())
    {
        emsg(_(e_cannot_make_changes_modifiable_is_off));
        return FALSE;
    }
    return TRUE;
}

    void
xim_engine_insert(int c)
{
    if (!editable() || replace_selection() == FAIL)
        return;
    if (typing_count == 256)
        xim_engine_boundary();
    ++typing_count;
    if (u_save_cursor() == FAIL)
        return;
    if (c == '\n')
    {
        (void)open_line(FORWARD, 0, 0, NULL);
        xim_engine_boundary();
    }
    else
        ins_char(c);
    curwin->w_set_curswant = TRUE;
}

    void
xim_engine_paste(const char *text)
{
    const char_u *p = (const char_u *)text;
    int was_paste = p_paste;
    if (*p == NUL || !editable())
        return;
    if (!was_paste)
        set_option_value_give_err((char_u *)"paste", TRUE, NULL, 0);
    xim_engine_boundary();
    if (replace_selection() == FAIL)
        goto done;
    while (*p != NUL)
    {
        if (u_save_cursor() == FAIL)
            break;
        if (*p == '\n')
        {
            if (open_line(FORWARD, 0, 0, NULL) == FAIL)
                break;
            ++p;
        }
        else
        {
            const char_u *end = p;
            while (*end != NUL && *end != '\n')
                ++end;
            ins_str((char_u *)p, end - p);
            p = end;
        }
    }
done:
    xim_engine_boundary();
    if (!was_paste)
        set_option_value_give_err((char_u *)"paste", FALSE, NULL, 0);
    curwin->w_set_curswant = TRUE;
}

    void
xim_engine_delete(int backward)
{
    pos_T start;
    if (!editable())
        return;
    xim_engine_boundary();
    if (VIsual_active)
    {
        (void)replace_selection();
        xim_engine_boundary();
        return;
    }
    start = curwin->w_cursor;
    if (backward)
    {
        if (start.col > 0)
        {
            (void)dec_cursor();
            if (u_save_cursor() == OK)
                (void)del_char(FALSE);
        }
        else if (start.lnum > 1)
        {
            --curwin->w_cursor.lnum;
            curwin->w_cursor.col = ml_get_curline_len();
            (void)do_join(2, FALSE, TRUE, FALSE, FALSE);
        }
    }
    else if (gchar_cursor() != NUL)
    {
        if (u_save_cursor() == OK)
            (void)del_char(FALSE);
    }
    else if (start.lnum < curbuf->b_ml.ml_line_count)
        (void)do_join(2, FALSE, TRUE, FALSE, FALSE);
    xim_engine_boundary();
}

    void
xim_engine_move(int key, int select)
{
    xim_engine_boundary();
    if (select && !VIsual_active)
    {
        selection_anchor = curwin->w_cursor;
        selection_buffer = curbuf->b_fnum;
        VIsual = selection_anchor;
        VIsual_active = TRUE;
        VIsual_mode = 'v';
        VIsual_select = FALSE;
    }
    else if (!select)
        xim_engine_cancel();
    switch (key)
    {
        case XIM_LEFT: (void)dec_cursor(); break;
        case XIM_RIGHT: (void)inc_cursor(); break;
        case XIM_HOME: curwin->w_cursor.col = 0; break;
        case XIM_END: curwin->w_cursor.col = ml_get_curline_len(); break;
        case XIM_UP: (void)cursor_up(1, FALSE); break;
        case XIM_DOWN: (void)cursor_down(1, FALSE); break;
        case XIM_PAGE_UP: (void)pagescroll(BACKWARD, 1, FALSE); break;
        case XIM_PAGE_DOWN: (void)pagescroll(FORWARD, 1, FALSE); break;
    }
    if (key != XIM_UP && key != XIM_DOWN)
        curwin->w_set_curswant = TRUE;
    check_cursor();
    redraw_curbuf_later(UPD_INVERTED);
}

    void
xim_engine_select_all(void)
{
    xim_engine_boundary();
    CLEAR_FIELD(selection_anchor);
    selection_anchor.lnum = 1;
    selection_buffer = curbuf->b_fnum;
    VIsual = selection_anchor;
    VIsual_active = TRUE;
    VIsual_select = FALSE;
    VIsual_mode = 'v';
    curwin->w_cursor.lnum = curbuf->b_ml.ml_line_count;
    curwin->w_cursor.col = ml_get_curline_len();
    curwin->w_cursor.coladd = 0;
    redraw_curbuf_later(UPD_INVERTED);
}

    char *
xim_engine_copy(int cut)
{
    oparg_T op;
    char_u *text;
    if (!selection_op(&op))
        return NULL;
    op.op_type = OP_YANK;
    op.regname = 'x';
    if (op_yank(&op, FALSE, FALSE) == FAIL)
        return NULL;
    text = get_reg_contents('x', GREG_NO_EXPR);
#ifdef FEAT_CLIPBOARD
    if (text != NULL && clip_plus.available)
        write_reg_contents_ex('+', text, -1, FALSE, MCHAR, 0);
#endif
    if (cut)
    {
        if (!editable())
            return (char *)text;
        (void)replace_selection();
        xim_engine_boundary();
    }
    return (char *)text;
}

    char *
xim_engine_clipboard(void)
{
#ifdef FEAT_CLIPBOARD
    if (clip_plus.available)
        return (char *)get_reg_contents('+', GREG_NO_EXPR);
#endif
    return NULL;
}

    void
xim_engine_free(char *text)
{
    vim_free(text);
}

    void
xim_engine_undo(int redo)
{
    xim_engine_boundary();
    xim_engine_cancel();
    if (redo)
        u_redo(1);
    else
        u_undo(1);
}

    int
xim_engine_columns(void)
{
    return (int)Columns;
}

    int
xim_engine_has_selection(void)
{
    return VIsual_active && selection_buffer == curbuf->b_fnum;
}

    int
xim_engine_can_undo(int redo)
{
    (void)redo;
    // Do not call undo_allowed() here: it emits E21 when 'modifiable' is
    // off, which would spam every menu render and prompt key. Undo/redo
    // availability is reported without side effects; the engine itself
    // refuses the operation when the buffer cannot be changed.
    if (!curbuf->b_p_ma)
        return FALSE;
    return TRUE;
}

    int
xim_engine_get_option(const char *name)
{
    if (STRCMP(name, "wrap") == 0)
        return curwin->w_p_wrap;
    if (STRCMP(name, "number") == 0)
        return curwin->w_p_nu || curwin->w_p_rnu;
    if (STRCMP(name, "mouse") == 0)
        return *p_mouse != NUL;
    return FALSE;
}

    void
xim_engine_toggle_option(const char *name)
{
    xim_engine_boundary();
    if (STRCMP(name, "wrap") == 0)
        do_cmdline_cmd((char_u *)"set wrap!");
    else if (STRCMP(name, "number") == 0)
    {
        if (curwin->w_p_nu || curwin->w_p_rnu)
            do_cmdline_cmd((char_u *)"set nonumber norelativenumber");
        else
            do_cmdline_cmd((char_u *)"set number");
    }
    else if (STRCMP(name, "mouse") == 0)
    {
        if (*p_mouse != NUL)
            do_cmdline_cmd((char_u *)"set mouse=");
        else
            do_cmdline_cmd((char_u *)"set mouse=a");
        setmouse();
    }
    redraw_all_later(UPD_NOT_VALID);
}

    static int
xim_word_class(char_u *line, colnr_T col)
{
    char_u ch = line[col];
    if (ch == ' ' || ch == '\t')
        return 0;
    if (vim_iswordc(ch))
        return 2;
    if (vim_strchr((char_u *)"-+*/%<>&|^!=", ch) != NULL)
        return 1;
    if (has_mbyte && MB_BYTE2LEN(ch) > 1)
        return 3;
    return ch;
}

    static int
xim_mouse_to_buffer(int screen_row, int screen_col, int *is_status, int *is_sep)
{
    int row = screen_row;
    int col = screen_col;
    win_T *wp;
    if (is_status != NULL)
        *is_status = FALSE;
    if (is_sep != NULL)
        *is_sep = FALSE;
    wp = mouse_find_win(&row, &col, FIND_POPUP);
    if (wp == NULL)
        return FALSE;
    if (row >= wp->w_height || col >= wp->w_width)
    {
        if (is_status != NULL && row >= wp->w_height)
            *is_status = TRUE;
        if (is_sep != NULL && col >= wp->w_width)
            *is_sep = TRUE;
        win_enter(wp, TRUE);
        return FALSE;
    }
    win_enter(wp, TRUE);
    if (mouse_comp_pos(curwin, &row, &col, &curwin->w_cursor.lnum, NULL))
    {
        curwin->w_cursor.lnum = curbuf->b_ml.ml_line_count;
        curwin->w_cursor.col = ml_get_buf_len(curbuf, curwin->w_cursor.lnum);
    }
    else
    {
        curwin->w_curswant = col;
        curwin->w_set_curswant = FALSE;
        if (coladvance(col) == FAIL)
            curwin->w_cursor.col = ml_get_buf_len(curbuf, curwin->w_cursor.lnum);
    }
    check_cursor();
    return TRUE;
}

    int
xim_engine_click(int row, int col, int extend, int clicks)
{
    int is_status = FALSE;
    int is_sep = FALSE;
    pos_T before = curwin->w_cursor;
    (void)before;
    xim_engine_boundary();
    if (!xim_mouse_to_buffer(row, col, &is_status, &is_sep))
    {
        // A press on a status or separator line arms an owning-thread drag.
        if ((is_status || is_sep) && clicks <= 1)
        {
            int rel_row = row;
            int rel_col = col;
            win_T *wp = mouse_find_win(&rel_row, &rel_col, FIND_POPUP);
            if (wp != NULL)
            {
                xim_sep_window = wp;
                xim_sep_is_status = is_status;
                xim_sep_offset = is_status ? rel_row - wp->w_height + 1
                                           : rel_col - wp->w_width + 1;
            }
        }
        redraw_curbuf_later(UPD_VALID);
        return FALSE;
    }
    xim_sep_window = NULL;
    if (extend && VIsual_active && selection_buffer == curbuf->b_fnum)
    {
        // Shift-click extends the existing half-open selection.
        redraw_curbuf_later(UPD_INVERTED);
        return TRUE;
    }
    xim_engine_cancel();
    xim_mouse_pressed = TRUE;
    xim_press_dragging = FALSE;
    xim_press_anchor = curwin->w_cursor;
    xim_press_buffer = curbuf->b_fnum;
    if (clicks >= 3)
    {
        // Triple-click selects the logical line.
        selection_anchor.lnum = curwin->w_cursor.lnum;
        selection_anchor.col = 0;
        selection_anchor.coladd = 0;
        selection_buffer = curbuf->b_fnum;
        VIsual = selection_anchor;
        VIsual_active = TRUE;
        VIsual_mode = 'v';
        VIsual_select = FALSE;
        curwin->w_cursor.col = ml_get_buf_len(curbuf, curwin->w_cursor.lnum);
        redraw_curbuf_later(UPD_INVERTED);
        return TRUE;
    }
    if (clicks == 2)
    {
        // Double-click selects a word, mirroring the inherited classes.
        pos_T start = curwin->w_cursor;
        pos_T end = curwin->w_cursor;
        char_u *line = ml_get(start.lnum);
        if (line[start.col] != NUL)
        {
            int cclass;
            {
                char_u ch = line[start.col];
                if (ch == ' ' || ch == '\t')
                    cclass = 0;
                else if (vim_iswordc(ch))
                    cclass = 2;
                else if (vim_strchr((char_u *)"-+*/%<>&|^!=", ch) != NULL)
                    cclass = 1;
                else
                    cclass = ch;
                if (has_mbyte && MB_BYTE2LEN(line[start.col]) > 1)
                    cclass = 3;
            }
            while (start.col > 0)
            {
                colnr_T prev = start.col - 1;
                prev -= (*mb_head_off)(line, line + prev);
                if (xim_word_class(line, prev) != cclass)
                    break;
                start.col = prev;
            }
            while (line[end.col] != NUL)
            {
                colnr_T next = end.col + (*mb_ptr2len)(line + end.col);
                if (xim_word_class(line, next) != cclass)
                {
                    if (*p_sel == 'e')
                        end.col = next;
                    break;
                }
                end.col = next;
            }
            selection_anchor = start;
            selection_buffer = curbuf->b_fnum;
            VIsual = start;
            VIsual_active = TRUE;
            VIsual_mode = 'v';
            VIsual_select = FALSE;
            curwin->w_cursor = end;
            redraw_curbuf_later(UPD_INVERTED);
            return TRUE;
        }
    }
    redraw_curbuf_later(UPD_VALID);
    return TRUE;
}

    void
xim_engine_drag(int row, int col)
{
    int is_status = FALSE;
    int is_sep = FALSE;
    win_T *dragwin = curwin;
    int rel_row = row;
    int rel_col = col;
    win_T *wp;
    int tmp_row = row;
    int tmp_col = col;
    // Separator drags resize the owning frame; the inherited helpers do the
    // frame math and redraw scheduling on this thread.
    if (xim_sep_window != NULL)
    {
        int count = 0;
        if (xim_sep_is_status)
            count = row - W_WINROW(xim_sep_window) - xim_sep_window->w_height + 1
                    - xim_sep_offset;
        else
            count = col - xim_sep_window->w_wincol - xim_sep_window->w_width + 1
                    - xim_sep_offset;
        if (count != 0)
        {
            if (xim_sep_is_status)
                win_drag_status_line(xim_sep_window, count);
            else
                win_drag_vsep_line(xim_sep_window, count);
        }
        return;
    }
    wp = mouse_find_win(&tmp_row, &tmp_col, FIND_POPUP);
    if (wp != NULL && wp != curwin)
    {
        // Keep the original window during a drag; edge motion scrolls it.
        rel_row = row - curwin->w_winrow;
        rel_col = col - curwin->w_wincol;
    }
    else if (wp != NULL)
    {
        rel_row = tmp_row;
        rel_col = tmp_col;
    }
    // Edge drag scrolls one line/column on drag events.
    if (rel_row < 0)
    {
        scrolldown(1, TRUE);
        rel_row = 0;
    }
    else if (rel_row >= curwin->w_height)
    {
        scrollup(1, TRUE);
        rel_row = curwin->w_height - 1;
    }
    if (rel_col < 0 && !curwin->w_p_wrap)
    {
        if (curwin->w_leftcol > 0)
            set_leftcol(curwin->w_leftcol - 1);
        rel_col = 0;
    }
    else if (rel_col >= curwin->w_width && !curwin->w_p_wrap)
    {
        set_leftcol(curwin->w_leftcol + 1);
        rel_col = curwin->w_width - 1;
    }
    (void)dragwin;
    if (!xim_mouse_pressed)
        return;
    // First motion turns the press anchor into a selection.
    if (!xim_press_dragging)
    {
        if (xim_press_buffer != curbuf->b_fnum)
            return;
        selection_anchor = xim_press_anchor;
        selection_buffer = curbuf->b_fnum;
        VIsual = selection_anchor;
        VIsual_active = TRUE;
        VIsual_mode = 'v';
        VIsual_select = FALSE;
        xim_press_dragging = TRUE;
    }
    {
        int crow = row - curwin->w_winrow;
        int ccol = col - curwin->w_wincol;
        if (crow < 0)
            crow = 0;
        if (crow >= curwin->w_height)
            crow = curwin->w_height - 1;
        if (ccol < 0)
            ccol = 0;
        if (ccol >= curwin->w_width)
            ccol = curwin->w_width - 1;
        if (mouse_comp_pos(curwin, &crow, &ccol, &curwin->w_cursor.lnum, NULL))
        {
            curwin->w_cursor.lnum = curbuf->b_ml.ml_line_count;
            curwin->w_cursor.col = ml_get_buf_len(curbuf, curwin->w_cursor.lnum);
        }
        else
        {
            curwin->w_curswant = ccol;
            curwin->w_set_curswant = FALSE;
            if (coladvance(ccol) == FAIL)
                curwin->w_cursor.col = ml_get_buf_len(curbuf, curwin->w_cursor.lnum);
        }
        check_cursor();
        redraw_curbuf_later(UPD_INVERTED);
    }
    (void)is_status;
    (void)is_sep;
}

    void
xim_engine_drag_end(void)
{
    xim_sep_window = NULL;
    xim_mouse_pressed = FALSE;
    xim_press_dragging = FALSE;
}

    void
xim_engine_scroll(int row, int col, int direction)
{
    int tmp_row = row;
    int tmp_col = col;
    win_T *wp = mouse_find_win(&tmp_row, &tmp_col, FIND_POPUP);
    win_T *old = curwin;
    pos_T saved = curwin->w_cursor;
    int saved_buf = curbuf->b_fnum;
    pos_T saved_anchor = selection_anchor;
    int saved_selbuf = selection_buffer;
    int saved_visual = VIsual_active;
    if (wp == NULL)
        return;
    curwin = wp;
    curbuf = wp->w_buffer;
    // Follow 'mousescroll' so a wheel notch scrolls like the inherited path.
    long vert_step = mouse_get_vert_scroll_step();
    long hor_step = mouse_get_hor_scroll_step();
    if (vert_step < 1)
        vert_step = 3;
    if (hor_step < 1)
        hor_step = 6;
    if (direction == 0)
        scrolldown((int)vert_step, TRUE);
    else if (direction == 1)
        scrollup((int)vert_step, TRUE);
    else if (!curwin->w_p_wrap)
    {
        if (direction == 2 && curwin->w_leftcol > 0)
        {
            long leftcol = curwin->w_leftcol - hor_step;
            if (leftcol < 0)
                leftcol = 0;
            set_leftcol((colnr_T)leftcol);
        }
        else if (direction == 3)
            set_leftcol(curwin->w_leftcol + (colnr_T)hor_step);
    }
    // Wheel motion never moves the insertion point or selection.
    if (curbuf->b_fnum == saved_buf)
        curwin->w_cursor = saved;
    selection_anchor = saved_anchor;
    selection_buffer = saved_selbuf;
    VIsual_active = saved_visual;
    curwin->w_redr_status = TRUE;
    redraw_curbuf_later(UPD_VALID);
    curwin = old;
    curbuf = curwin->w_buffer;
}

    void
xim_engine_middle_paste(int row, int col)
{
    if (xim_mouse_to_buffer(row, col, NULL, NULL))
    {
        char *text = xim_engine_clipboard();
        if (text != NULL && *text != NUL)
            xim_engine_paste(text);
        vim_free(text);
    }
}

    int
xim_engine_command(const char *command, const char *argument, int preserve_position)
{
    char_u *escaped = NULL;
    char_u *line;
    int result = FAIL;
    int errors = called_emsg;
    buf_T *editing = curbuf;
    pos_T insertion = curwin->w_cursor;
    char_u *old_ffname = curbuf->b_ffname == NULL ? NULL : vim_strsave(curbuf->b_ffname);
    char_u *old_sfname = curbuf->b_sfname == NULL ? NULL : vim_strsave(curbuf->b_sfname);
    bool distinct_sfname = curbuf->b_sfname != NULL && curbuf->b_sfname != curbuf->b_ffname;
    xim_engine_boundary();
    xim_engine_cancel();
    if (*argument != NUL && *command != NUL)
    {
        escaped = vim_strsave_fnameescape((char_u *)argument, VSE_NONE);
        if (escaped == NULL)
            return FAIL;
    }
    line = concat_str((char_u *)command, (char_u *)" ");
    if (line != NULL)
    {
        char_u *full = concat_str(line, escaped != NULL ? escaped : (char_u *)argument);
        vim_free(line);
        if (full != NULL)
        {
            State = MODE_NORMAL;
            ++no_wait_return;
            result = do_cmdline_cmd(full);
            --no_wait_return;
            need_wait_return = FALSE;
            State = MODE_INSERT;
            if (preserve_position && editing == curbuf
                    && insertion.lnum <= curbuf->b_ml.ml_line_count
                    && insertion.col <= ml_get_buf_len(curbuf, insertion.lnum))
            {
                curwin->w_cursor = insertion;
                if (insertion.col == 0 || insertion.col < ml_get_buf_len(curbuf, insertion.lnum))
                    curwin->w_cursor.coladd = 0;
            }
            vim_free(full);
        }
    }
    vim_free(escaped);
    if (STRCMP(command, "saveas") == 0 && called_emsg != errors && editing == curbuf)
    {
        vim_free(curbuf->b_ffname);
        if (distinct_sfname)
            vim_free(curbuf->b_sfname);
        curbuf->b_ffname = old_ffname;
        curbuf->b_sfname = old_sfname;
        curbuf->b_fname = (curbuf->b_sfname != NULL && curbuf->b_sfname != curbuf->b_ffname)
            ? curbuf->b_sfname : curbuf->b_ffname;
    }
    else
    {
        vim_free(old_ffname);
        vim_free(old_sfname);
    }
    curwin->w_set_curswant = TRUE;
    return called_emsg == errors ? result : FAIL;
}

    const char *
xim_engine_error(void)
{
    return (const char *)get_vim_var_str(VV_ERRMSG);
}

    void
xim_engine_find(const char *text, int backward)
{
    pos_T result = curwin->w_cursor;
    char_u *escaped;
    char_u *pattern;
    if (*text == NUL)
        return;
    xim_engine_cancel();
    escaped = vim_strsave_escaped((char_u *)text, (char_u *)"\\");
    if (escaped == NULL)
        return;
    pattern = concat_str((char_u *)"\\V", escaped);
    vim_free(escaped);
    if (pattern == NULL)
        return;
    if (searchit(curwin, curbuf, &result, NULL, backward ? BACKWARD : FORWARD,
                pattern, STRLEN(pattern), 1, SEARCH_MSG, RE_LAST, NULL) != FAIL)
    {
        curwin->w_cursor = result;
        set_last_search_pat(pattern, RE_SEARCH, TRUE, TRUE);
        curwin->w_set_curswant = TRUE;
    }
    vim_free(pattern);
    redraw_curbuf_later(UPD_NOT_VALID);
}

    int
xim_engine_menu_rows(void)
{
    return MAX(1, (int)Rows - 2);
}

    void
xim_engine_menu_bar(const char *text, int selected_col, int selected_width)
{
    int bar_attr = syn_name2attr((char_u *)"XimMenuBar");
    int sel_attr = syn_name2attr((char_u *)"XimMenuSel");
    if (bar_attr == 0)
        bar_attr = syn_name2attr((char_u *)"StatusLine");
    if (sel_attr == 0)
        sel_attr = syn_name2attr((char_u *)"PmenuSel");
    xim_drawing_overlay = TRUE;
    screen_puts_len((char_u *)text, (int)STRLEN(text), 0, 0, bar_attr);
    screen_fill(0, 1, MIN(vim_strsize((char_u *)text), (int)Columns),
            (int)Columns, ' ', ' ', bar_attr);
    if (selected_width > 0 && selected_col >= 0)
    {
        int end = MIN(selected_col + selected_width, (int)Columns);
        for (int col = MAX(0, selected_col); col < end; ++col)
        {
            char_u bytes[MB_MAXBYTES + 1];
            int attr = 0;
            screen_getbytes(0, col, bytes, &attr);
            (void)attr;
            // Re-highlight the selected heading without moving text.
            screen_puts_len(bytes, (int)STRLEN(bytes), 0, col, sel_attr);
        }
    }
    xim_drawing_overlay = FALSE;
}

    void
xim_draw_menu_bar(void)
{
    char text[1024];
    int sel_col = 0;
    int sel_width = 0;
    int bar_attr = syn_name2attr((char_u *)"XimMenuBar");
    int sel_attr = syn_name2attr((char_u *)"XimMenuSel");
    if (bar_attr == 0)
        bar_attr = syn_name2attr((char_u *)"StatusLine");
    if (sel_attr == 0)
        sel_attr = syn_name2attr((char_u *)"PmenuSel");
    xim_menu_bar_text(text, sizeof(text), (int)Columns, &sel_col, &sel_width);
    xim_drawing_overlay = TRUE;
    screen_puts_len((char_u *)text, (int)STRLEN(text), 0, 0, bar_attr);
    screen_fill(0, 1, MIN(vim_strsize((char_u *)text), (int)Columns),
            (int)Columns, ' ', ' ', bar_attr);
    if (sel_width > 0)
    {
        int start = MAX(0, sel_col);
        int end = MIN(sel_col + sel_width, (int)Columns);
        // Highlight the open heading by re-emitting its cells.
        for (int col = start; col < end; ++col)
        {
            char_u bytes[MB_MAXBYTES + 1];
            int attr = 0;
            // Read the already-painted cell so wide/combining identity
            // is preserved; only the attribute changes.
            screen_getbytes(0, col, bytes, &attr);
            (void)attr;
            if (*bytes != NUL)
                screen_puts(bytes, 0, col, sel_attr);
            else
                screen_putchar(' ', 0, col, sel_attr);
        }
    }
    xim_drawing_overlay = FALSE;
}

    void
xim_engine_menu_popup(const char *items, int row, int col, int width, int rows,
        int selected_row, int error)
{
    int attr = syn_name2attr((char_u *)"XimPalette");
    int sel_attr = syn_name2attr((char_u *)"XimPaletteSel");
    int err_attr = syn_name2attr((char_u *)"ErrorMsg");
    const char *p = items;
    const char *end;
    int r;
    xim_drawing_overlay = TRUE;
    term_set_sync_output(TERM_SYNC_OUTPUT_ENABLE);
    if (menu_visible && (menu_rows != Rows || menu_columns != Columns))
    {
        redraw_all_later(UPD_NOT_VALID);
        update_screen(0);
        menu_visible = FALSE;
    }
    if (!menu_visible)
    {
        vim_free(overlay_background);
        // Share one full-screen snapshot for prompts and menus so a
        // top drop-down and a bottom prompt do not overwrite each other.
        // The snapshot is refreshed only when neither overlay is visible.
        if (!overlay_visible)
        {
            overlay_background = ALLOC_CLEAR_MULT(xim_cell_T, (size_t)Rows * Columns);
            if (overlay_background != NULL)
                for (int rr = 0; rr < Rows; ++rr)
                    for (int cc = 0; cc < Columns; ++cc)
                    {
                        xim_cell_T *cell = &overlay_background[(size_t)rr * Columns + cc];
                        screen_getbytes(rr, cc, cell->bytes, &cell->attr);
                    }
        }
    }
    else if (row != menu_top || col != menu_left || rows != menu_height
            || width != menu_width)
    {
        // Restore the previous drop-down rectangle before painting the
        // moved one, preserving the document underneath.
        if (overlay_background != NULL)
            for (r = menu_top; r < menu_top + menu_height && r < Rows; ++r)
                for (int cc = menu_left; cc < menu_left + menu_width && cc < Columns; ++cc)
                {
                    xim_cell_T *cell = &overlay_background[(size_t)r * Columns + cc];
                    if (*cell->bytes != NUL)
                        screen_puts(cell->bytes, r, cc, cell->attr);
                }
        else
        {
            redraw_all_later(UPD_NOT_VALID);
            update_screen(0);
        }
    }
    menu_visible = TRUE;
    menu_top = row;
    menu_left = col;
    menu_height = rows;
    menu_width = width;
    menu_rows = Rows;
    menu_columns = Columns;
    overlay_rows = Rows;
    overlay_columns = Columns;
    r = row;
    for (p = items; *p != NUL && r < row + rows; p = end + 1, ++r)
    {
        int item_attr = error ? err_attr
            : (r - row == selected_row ? sel_attr : attr);
        end = (const char *)vim_strchr((char_u *)p, '\n');
        if (end == NULL)
            break;
        screen_puts_len((char_u *)p, (int)(end - p), r, col, item_attr);
        int cells = vim_strnsize((char_u *)p, (int)(end - p));
        // Clear only trailing cells inside the drop-down; cells beyond
        // the popup keep the underlying document from the snapshot.
        screen_fill(r, r + 1, MIN(col + cells, (int)Columns), MIN(col + width, (int)Columns),
                ' ', ' ', item_attr);
    }
    term_set_sync_output(TERM_SYNC_OUTPUT_DISABLE);
    out_flush();
    xim_drawing_overlay = FALSE;
}

    static void
xim_clear_menu_popup(void)
{
    if (!menu_visible)
        return;
    if (overlay_background != NULL)
        for (int r = menu_top; r < menu_top + menu_height && r < Rows; ++r)
            for (int cc = menu_left; cc < menu_left + menu_width && cc < Columns; ++cc)
            {
                xim_cell_T *cell = &overlay_background[(size_t)r * Columns + cc];
                if (*cell->bytes != NUL)
                    screen_puts(cell->bytes, r, cc, cell->attr);
            }
    else
    {
        redraw_all_later(UPD_NOT_VALID);
        update_screen(0);
    }
    menu_visible = FALSE;
    if (!overlay_visible)
        VIM_CLEAR(overlay_background);
}

    void
xim_engine_overlay(const char *prompt, const char *items, int error)
{
    int row = (int)Rows - 2;
    const char *end;
    int attr = syn_name2attr((char_u *)"XimPalette");
    int count = 0;
    const char *p;
    for (p = items; *p != NUL; ++p)
        if (*p == '\n')
            ++count;
    row -= count < row ? count : row;
    xim_drawing_overlay = TRUE;
    term_set_sync_output(TERM_SYNC_OUTPUT_ENABLE);
    if (overlay_visible && (overlay_rows != Rows || overlay_columns != Columns))
    {
        redraw_all_later(UPD_NOT_VALID);
        update_screen(0);
        overlay_visible = FALSE;
    }
    if (!overlay_visible)
    {
        vim_free(overlay_background);
        overlay_background = ALLOC_CLEAR_MULT(xim_cell_T, (size_t)Rows * Columns);
        if (overlay_background != NULL)
            for (int r = 0; r < Rows; ++r)
                for (int col = 0; col < Columns; ++col)
                {
                    xim_cell_T *cell = &overlay_background[(size_t)r * Columns + col];
                    screen_getbytes(r, col, cell->bytes, &cell->attr);
                }
    }
    else if (row > overlay_top)
    {
        // Restore only newly exposed rows, never redraw the editor under
        // the part of the menu that remains visible. Preserve UTF-8,
        // combining characters, wide-cell identity and theme attributes.
        if (overlay_background != NULL)
            for (int r = overlay_top; r < row; ++r)
                for (int col = 0; col < Columns; ++col)
                {
                    xim_cell_T *cell = &overlay_background[(size_t)r * Columns + col];
                    if (*cell->bytes != NUL)
                        screen_puts(cell->bytes, r, col, cell->attr);
                }
        else
        {
            redraw_all_later(UPD_NOT_VALID);
            update_screen(0);
        }
    }
    overlay_visible = TRUE;
    overlay_top = row;
    overlay_rows = Rows;
    overlay_columns = Columns;
    for (p = items; *p != NUL && row < Rows - 2; p = end + 1, ++row)
    {
        int item_attr = error ? syn_name2attr((char_u *)"ErrorMsg")
            : *p == '>' ? syn_name2attr((char_u *)"XimPaletteSel") : attr;
        end = (const char *)vim_strchr((char_u *)p, '\n');
        if (end == NULL)
            break;
        screen_puts_len((char_u *)p, (int)(end - p), row, 0, item_attr);
        int cells = vim_strnsize((char_u *)p, (int)(end - p));
        screen_fill(row, row + 1, MIN(cells, (int)Columns), (int)Columns,
                ' ', ' ', item_attr);
    }
    attr = syn_name2attr((char_u *)"XimPrompt");
    screen_puts_len((char_u *)prompt, (int)STRLEN(prompt), (int)Rows - 1, 0, attr);
    screen_fill((int)Rows - 1, (int)Rows,
            MIN(vim_strsize((char_u *)prompt), (int)Columns), (int)Columns,
            ' ', ' ', attr);
    {
        int cursor_cells = 0;
        int length_cells = 0;
        extern void xim_prompt_get(int *, int *);
        xim_prompt_get(&cursor_cells, &length_cells);
        int label_cells = vim_strsize((char_u *)prompt) - length_cells;
        if (label_cells < 0)
            label_cells = (int)STRLEN(prompt) - length_cells;
        if (label_cells < 0)
            label_cells = 0;
        int cursor_col = label_cells + cursor_cells;
        if (cursor_col >= Columns)
            cursor_col = Columns - 1;
        if (cursor_col < 0)
            cursor_col = 0;
        windgoto((int)Rows - 1, cursor_col);
    }
    term_set_sync_output(TERM_SYNC_OUTPUT_DISABLE);
    out_flush();
    xim_drawing_overlay = FALSE;
}

    static void
xim_redraw_overlay(void)
{
    if (!xim_prompt_active())
    {
        if (menu_visible)
            xim_clear_menu_popup();
        return;
    }
    // The engine has produced a fresh underlying frame. Capture that
    // frame rather than retaining a snapshot from before the resize.
    overlay_visible = FALSE;
    menu_visible = FALSE;
    xim_render();
}

    static void
xim_set_hover(int enable)
{
    if (enable == hover_enabled)
        return;
    hover_enabled = enable;
    // Passive motion only while a menu is open; idle editing keeps
    // button-motion reporting so the terminal stays quiet.
    out_str_nf((char_u *)(enable ? "\033[?1003h" : "\033[?1003l"));
    out_flush();
}

    void
xim_initialize(void)
{
    // A directory argument starts a project at that directory.  A file
    // argument opens that file alone; it does not start a directory walk.
    // The picker falls back to a direct filesystem check for any path
    // the user types, so an out-of-tree file remains reachable without
    // a project root.
    xim_redraw_ui = xim_redraw_overlay;
    xim_draw_menu_bar_ptr = xim_draw_menu_bar;
    char_u *root = NULL;
    int file_arguments = 0;
    int i;

    for (i = 0; i < GARGCOUNT; ++i)
    {
	char_u *name = GARGLIST[i].ae_fname;
	if (name == NULL || *name == NUL)
	    continue;
	++file_arguments;
	if (i == 0 && mch_isdir(name))
	    root = vim_strsave(name);
    }
    if (root != NULL)
	xim_project_init((char *)root);
    else if (file_arguments == 0)
	// Bare start: the current directory is the project root.
	xim_project_init(NULL);
    else
	// File-only start: no project, so no directory walk runs.
	xim_project_disable();
    vim_free(root);
    // Disable netrw so a directory argument opens an empty buffer rather
    // than the directory listing plugin.  Without this, filetype detection
    // launches netrw on the directory buffer and intercepts Ctrl-P / P-E
    // before xim's picker can render the overlay.
    do_cmdline_cmd((char_u *)"let g:loaded_netrw = 1");
    do_cmdline_cmd((char_u *)"set nocompatible laststatus=2 noshowmode noshowcmd noruler noinsertmode selection=exclusive backspace=indent,eol,start ttimeout ttimeoutlen=20 mouse=a mousemodel=extend showtabline=2");
    do_highlight((char_u *)"default link XimStatus StatusLine", FALSE, FALSE);
    do_highlight((char_u *)"default link XimPrompt Pmenu", FALSE, FALSE);
    do_highlight((char_u *)"default link XimPalette Pmenu", FALSE, FALSE);
    do_highlight((char_u *)"default link XimPaletteSel PmenuSel", FALSE, FALSE);
    do_highlight((char_u *)"default link XimMenuBar StatusLine", FALSE, FALSE);
    do_highlight((char_u *)"default link XimMenuSel PmenuSel", FALSE, FALSE);
    do_cmdline_cmd((char_u *)"set statusline=%#XimStatus#\\ Xim\\ %f\\ %m%=%l:%c\\ ");
    do_cmdline_cmd((char_u *)"syntax enable");
    setmouse();
    p_lpl = FALSE;
    State = MODE_NORMAL;
}

    void
xim_step(void)
{
    int c;
    int key;
    int modifiers = 0;
    int blocking = FALSE;
    state_no_longer_safe("Xim input");
    if (selection_buffer != 0 && selection_buffer != curbuf->b_fnum)
        xim_engine_cancel();
    State = MODE_INSERT;
    ++no_mapping;
    ++allow_keys;
    if (xim_background())
    {
        c = K_IGNORE;
        goto received;
    }
    xim_render();
    // Wait at the top-level seam: completions are not decoder input and
    // engine operations never run inside safe_vgetc(). The existing Unix
    // wait still services timers, channels and resize notifications.
    while (xim_input_blocked()
            || (vpeekc() == NUL && typebuf.tb_len == 0 && !input_available()))
    {
        xim_wake_fd = xim_project_wake_fd();
        xim_waiting = TRUE;
        out_flush_cursor(FALSE, FALSE);
        blocking = mch_wait_for_xim_event(xim_input_blocked(), blocking);
        xim_waiting = FALSE;
        if (xim_background())
        {
            c = K_IGNORE;
            goto received;
        }
        xim_render();
    }
    c = safe_vgetc();
received:
    --allow_keys;
    --no_mapping;
    // Native mouse uses the inherited SGR decoder; no second parser and no
    // modal key injection. All window math stays on the owning thread.
    if (is_mouse_key(c))
    {
        int m_modifiers = 0;
        int clicks = ((mod_mask & MOD_MASK_MULTI_CLICK) >> 5) + 1;
        if (clicks < 1)
            clicks = 1;
        if (clicks > 4)
            clicks = 4;
        if (mod_mask & MOD_MASK_SHIFT)
            m_modifiers |= XIM_SHIFT;
        if (mod_mask & MOD_MASK_CTRL)
            m_modifiers |= XIM_CTRL;
        if (mod_mask & MOD_MASK_ALT)
            m_modifiers |= XIM_ALT;
        if (c == K_LEFTMOUSE || c == K_LEFTMOUSE_NM)
            xim_mouse_press(mouse_row, mouse_col, 0, m_modifiers, clicks);
        else if (c == K_LEFTDRAG)
            xim_mouse_drag(mouse_row, mouse_col, m_modifiers);
        else if (c == K_LEFTRELEASE || c == K_LEFTRELEASE_NM)
        {
            xim_mouse_release(mouse_row, mouse_col, 0, m_modifiers);
            xim_mouse_pressed = FALSE;
            xim_press_dragging = FALSE;
        }
        else if (c == K_MIDDLEMOUSE)
            xim_mouse_press(mouse_row, mouse_col, 1, m_modifiers, clicks);
        else if (c == K_MIDDLEDRAG)
            xim_mouse_drag(mouse_row, mouse_col, m_modifiers);
        else if (c == K_MIDDLERELEASE)
            xim_mouse_release(mouse_row, mouse_col, 1, m_modifiers);
        else if (c == K_RIGHTMOUSE)
            xim_mouse_press(mouse_row, mouse_col, 2, m_modifiers, clicks);
        else if (c == K_RIGHTDRAG)
            xim_mouse_drag(mouse_row, mouse_col, m_modifiers);
        else if (c == K_RIGHTRELEASE)
            xim_mouse_release(mouse_row, mouse_col, 2, m_modifiers);
        // SGR wheel codes arrive inverted (64->DOWN, 65->UP); map to
        // visual directions so wheel-up shows earlier lines.
        else if (c == K_MOUSEDOWN)
            xim_mouse_wheel(mouse_row, mouse_col, 0, m_modifiers);
        else if (c == K_MOUSEUP)
            xim_mouse_wheel(mouse_row, mouse_col, 1, m_modifiers);
        else if (c == K_MOUSELEFT)
            xim_mouse_wheel(mouse_row, mouse_col, 2, m_modifiers);
        else if (c == K_MOUSERIGHT)
            xim_mouse_wheel(mouse_row, mouse_col, 3, m_modifiers);
        else if (c == K_MOUSEMOVE)
            xim_mouse_move(mouse_row, mouse_col, m_modifiers);
        got_int = FALSE;
        State = MODE_INSERT;
        check_cursor();
        curwin->w_redr_status = TRUE;
        redraw_curbuf_later(UPD_VALID);
        xim_set_hover(xim_menu_active());
        if (!xim_prompt_active())
        {
            if (overlay_visible && !menu_visible)
            {
                redraw_all_later(UPD_NOT_VALID);
                clear_cmdline = TRUE;
                redraw_cmdline = TRUE;
                overlay_visible = FALSE;
                VIM_CLEAR(overlay_background);
            }
            if (!xim_menu_active() && menu_visible)
                xim_clear_menu_popup();
        }
        return;
    }
    if (c == K_PS)
    {
        garray_T text;
        ga_init2(&text, 1, 1024);
        bracketed_paste(PASTE_EX, FALSE, &text);
        ga_append(&text, NUL);
        if (text.ga_data != NULL)
            xim_accept_paste(text.ga_data);
        ga_clear(&text);
        c = K_IGNORE;
    }
    key = c;
    if (mod_mask & MOD_MASK_SHIFT) modifiers |= XIM_SHIFT;
    if (mod_mask & MOD_MASK_CTRL) modifiers |= XIM_CTRL;
    if (mod_mask & MOD_MASK_ALT) modifiers |= XIM_ALT;
    switch (c)
    {
        case K_S_LEFT: modifiers |= XIM_SHIFT; /* FALLTHROUGH */
        case K_LEFT: key = XIM_LEFT; break;
        case K_S_RIGHT: modifiers |= XIM_SHIFT; /* FALLTHROUGH */
        case K_RIGHT: key = XIM_RIGHT; break;
        case K_S_UP: modifiers |= XIM_SHIFT; /* FALLTHROUGH */
        case K_UP: key = XIM_UP; break;
        case K_S_DOWN: modifiers |= XIM_SHIFT; /* FALLTHROUGH */
        case K_DOWN: key = XIM_DOWN; break;
        case K_S_HOME: modifiers |= XIM_SHIFT; /* FALLTHROUGH */
        case K_HOME: case K_KHOME: key = XIM_HOME; break;
        case K_S_END: modifiers |= XIM_SHIFT; /* FALLTHROUGH */
        case K_END: case K_KEND: key = XIM_END; break;
        case K_PAGEUP: key = XIM_PAGE_UP; break;
        case K_PAGEDOWN: key = XIM_PAGE_DOWN; break;
        case K_DEL: case K_KDEL: key = XIM_DELETE; break;
        case K_BS: key = XIM_BACKSPACE; break;
        case K_F1: key = XIM_F1; break;
        case K_F2: key = XIM_F2; break;
        case K_F3: key = XIM_F3; break;
        case K_F4: key = XIM_F4; break;
        case K_F5: key = XIM_F5; break;
        case K_F6: key = XIM_F6; break;
        case K_F7: key = XIM_F7; break;
        case K_F8: key = XIM_F8; break;
        case K_F9: key = XIM_F9; break;
        case K_F10: key = XIM_F10; break;
        case K_F11: key = XIM_F11; break;
        case K_F12: key = XIM_F12; break;
        default: if (IS_SPECIAL(c)) key = XIM_IGNORE; break;
    }
    xim_dispatch(key, modifiers);
    got_int = FALSE;
    State = MODE_INSERT;
    check_cursor();
    if (key != XIM_UP && key != XIM_DOWN)
        curwin->w_set_curswant = TRUE;
    xim_set_hover(xim_menu_active());
    if (!xim_prompt_active())
    {
        curwin->w_redr_status = TRUE;
        redraw_curbuf_later(UPD_VALID);
        if (overlay_visible && !menu_visible)
        {
            redraw_all_later(UPD_NOT_VALID);
            clear_cmdline = TRUE;
            redraw_cmdline = TRUE;
            overlay_visible = FALSE;
            VIM_CLEAR(overlay_background);
        }
        if (!xim_menu_active() && menu_visible)
            xim_clear_menu_popup();
        if (!xim_menu_active() && overlay_visible && menu_visible)
        {
            // Both overlays shared one snapshot; the menu is gone but the
            // prompt snapshot remains valid for its own rows.
        }
    }
}
