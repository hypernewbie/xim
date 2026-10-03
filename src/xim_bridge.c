/* Native Xim engine adapters. All functions run on the editor's main thread. */
#include "vim.h"
#include "xim_input.h"
#include "xim_project.h"

static pos_T selection_anchor;
static int selection_buffer = 0;
static int typing_count = 0;
static int overlay_visible = FALSE;

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
	size += STRLEN(buf->b_ffname != NULL
					       ? buf->b_ffname : (char_u *)"[No Name]") + 1;
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
	size_t len = STRLEN(name);
	mch_memmove(names + used, name, len);
	used += len;
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
xim_engine_overlay(const char *prompt, const char *items, int error)
{
    int row = (int)Rows - 2;
    const char *end;
    int attr = syn_name2attr((char_u *)"XimPalette");
    int count = 0;
    const char *p;
    overlay_visible = TRUE;
    for (p = items; *p != NUL; ++p)
        if (*p == '\n')
            ++count;
    row -= count < row ? count : row;
    for (p = items; *p != NUL && row < Rows - 2; p = end + 1, ++row)
    {
        int item_attr = error ? syn_name2attr((char_u *)"ErrorMsg")
            : *p == '>' ? syn_name2attr((char_u *)"XimPaletteSel") : attr;
        end = (const char *)vim_strchr((char_u *)p, '\n');
        if (end == NULL)
            break;
        screen_fill(row, row + 1, 0, (int)Columns, ' ', ' ', item_attr);
        screen_puts_len((char_u *)p, (int)(end - p), row, 0, item_attr);
    }
    attr = syn_name2attr((char_u *)"XimPrompt");
    screen_fill((int)Rows - 1, (int)Rows, 0, (int)Columns, ' ', ' ', attr);
    screen_puts_len((char_u *)prompt, (int)STRLEN(prompt), (int)Rows - 1, 0, attr);
    windgoto((int)Rows - 1, MIN(vim_strsize((char_u *)prompt), (int)Columns - 1));
    out_flush();
}

    void
xim_initialize(void)
{
    // A directory argument starts a project at that directory.  A file
    // argument roots the project at its parent so indexing stays local.
    char_u *root = NULL;
    if (GARGCOUNT > 0)
    {
	char_u *name = GARGLIST[0].ae_fname;
	if (name != NULL && *name != NUL)
	{
	    if (mch_isdir(name))
		root = vim_strsave(name);
	    else
	    {
		char_u *parent = vim_strsave(name);
		if (parent != NULL)
		{
		    if (mch_isdir(parent))
			root = parent;
		    else
		    {
			char_u *slash = gettail(parent);
			if (slash != NULL)
			    *slash = NUL;
			root = parent;
		    }
		}
	    }
	}
    }
    xim_project_init(root == NULL ? NULL : (char *)root);
    if (root != NULL)
	vim_free(root);
    do_cmdline_cmd((char_u *)"set nocompatible laststatus=2 noshowmode noshowcmd noruler noinsertmode selection=exclusive backspace=indent,eol,start ttimeout ttimeoutlen=20");
    do_highlight((char_u *)"default link XimStatus StatusLine", FALSE, FALSE);
    do_highlight((char_u *)"default link XimPrompt Pmenu", FALSE, FALSE);
    do_highlight((char_u *)"default link XimPalette Pmenu", FALSE, FALSE);
    do_highlight((char_u *)"default link XimPaletteSel PmenuSel", FALSE, FALSE);
    do_cmdline_cmd((char_u *)"set statusline=%#XimStatus#\\ Xim\\ %f\\ %m%=%l:%c\\ ");
    do_cmdline_cmd((char_u *)"syntax enable");
    p_lpl = FALSE;
    State = MODE_NORMAL;
}

    void
xim_step(void)
{
    int c;
    int key;
    int modifiers = 0;
    state_no_longer_safe("Xim input");
    if (selection_buffer != 0 && selection_buffer != curbuf->b_fnum)
        xim_engine_cancel();
    State = MODE_INSERT;
    xim_render();
    ++no_mapping;
    ++allow_keys;
    c = safe_vgetc();
    --allow_keys;
    --no_mapping;
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
    curwin->w_redr_status = TRUE;
    redraw_curbuf_later(UPD_VALID);
    if (overlay_visible)
    {
        redraw_all_later(UPD_NOT_VALID);
        overlay_visible = FALSE;
    }
}
