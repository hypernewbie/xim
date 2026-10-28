/* Native Xim engine adapters. All functions run on the editor's main thread. */
#include "vim.h"
#include "xim_input.h"
#include "xim_project.h"
#include "xim_recent.h"

static pos_T selection_anchor;
static int selection_buffer = 0;
static int typing_count = 0;
static int overlay_visible = FALSE;
static int overlay_top = 0;
// Size of the frame the shared background snapshot was captured from. A
// terminal resize invalidates the snapshot; an ordinary redraw does not, so
// the snapshot is not recaptured from a screen that still holds overlay
// pixels.
static int overlay_frame_rows = 0;
static int overlay_frame_columns = 0;
// Set while a recapture forces the document to repaint, so the redraw hook
// does not re-enter itself.
static int overlay_redraw_guard = FALSE;
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
 * Buffer inventory for the external-change service. Index order follows the
 * buffer list and skips the dummy buffer, matching xim_engine_buffers().
 */
    int
xim_engine_buffer_count(void)
{
    int count = 0;
    for (buf_T *buf = firstbuf; buf != NULL; buf = buf->b_next)
	if ((buf->b_flags & BF_DUMMY) == 0)
	    ++count;
    return count;
}

    int
xim_engine_buffer_info(int index, int *fnum, unsigned long long *tick,
	int *modified)
{
    int current = 0;
    for (buf_T *buf = firstbuf; buf != NULL; buf = buf->b_next)
    {
	if (buf->b_flags & BF_DUMMY)
	    continue;
	if (current++ != index)
	    continue;
	if (fnum != NULL) *fnum = buf->b_fnum;
	if (tick != NULL) *tick = (unsigned long long)CHANGEDTICK(buf);
	if (modified != NULL) *modified = bufIsChanged(buf) ? 1 : 0;
	return 1;
    }
    return 0;
}

    char *
xim_engine_buffer_name(int fnum)
{
    buf_T *buf = buflist_findnr(fnum);
    if (buf == NULL || buf->b_ffname == NULL || !bt_normal(buf))
	return NULL;
    return (char *)vim_strsave(buf->b_ffname);
}

    int
xim_engine_current_buffer(void)
{
    return curbuf->b_fnum;
}

    int
xim_engine_buffer_utf8(int fnum)
{
    buf_T *buf = buflist_findnr(fnum);
    if (buf == NULL || !enc_utf8 || buf->b_p_bin || !bt_normal(buf)
	    || !buf->b_p_ma)
	return 0;
    if (buf->b_p_fenc == NULL || *buf->b_p_fenc == NUL)
	return 1;
    return STRCMP(buf->b_p_fenc, (char_u *)"utf-8") == 0
	|| STRCMP(buf->b_p_fenc, (char_u *)"utf8") == 0;
}

/*
 * Apply staged disk bytes to a clean, current buffer while keeping undo.
 * The caller already verified the buffer number, name, modification state
 * and changedtick. Only UTF-8 text is staged; other encodings use the
 * explicit engine reload so the full conversion rules apply.
 */
    static int
xim_reload_valid(bufref_T *ref, const char_u *name,
	unsigned long long tick, unsigned long long version)
{
    if (!bufref_valid(ref) || curbuf != ref->br_buf
	    || curbuf->b_ffname == NULL || STRCMP(curbuf->b_ffname, name) != 0
	    || bufIsChanged(curbuf)
	    || (unsigned long long)CHANGEDTICK(curbuf) != tick
	    || !xim_engine_buffer_utf8(curbuf->b_fnum))
	return FALSE;
    return xim_project_file_version((const char *)name) == version;
}

    int
xim_engine_reload_staged(int fnum, const char *data, size_t length,
	unsigned long long expected_tick, unsigned long long expected_version)
{
    buf_T *buf = buflist_findnr(fnum);
    bufref_T ref;
    stat_T info;
    if (buf == NULL || buf != curbuf || buf->b_ffname == NULL
	    || memchr(data, NUL, length) != NULL
	    || !utf_valid_string((char_u *)data, (char_u *)data + length))
	return 0;
    set_bufref(&ref, buf);
    char_u *name = vim_strsave(buf->b_ffname);
    if (name == NULL)
	return 0;
    // The bytes belong to a specific disk version, not whatever file now
    // occupies that path. Preserve this stamp for the next ordinary write.
    if (mch_stat((const char *)name, &info) != 0
	    || !xim_reload_valid(&ref, name, expected_tick, expected_version))
    {
	vim_free(name);
	return 0;
    }
    apply_autocmds(EVENT_BUFREADPRE, name, NULL, FALSE, buf);
    if (!xim_reload_valid(&ref, name, expected_tick, expected_version))
    {
	vim_free(name);
	return 0;
    }
    vim_free(name);

    int bomb = length >= 3 && (unsigned char)data[0] == 0xef
	&& (unsigned char)data[1] == 0xbb && (unsigned char)data[2] == 0xbf;
    if (bomb)
    {
	data += 3;
	length -= 3;
    }
    // Only consistent CRLF pairs select DOS. A lone CR in an LF file is
    // text, not evidence that every line must lose its last byte.
    int mac = memchr(data, '\n', length) == NULL
	&& memchr(data, '\r', length) != NULL;
    int crlf = !mac && memchr(data, '\n', length) != NULL;
    for (size_t i = 0; crlf && i < length; ++i)
	if (data[i] == '\n' && (i == 0 || data[i - 1] != '\r'))
	    crlf = FALSE;
    int eol = length == 0 || data[length - 1] == (mac ? '\r' : '\n');

    garray_T lines;
    ga_init2(&lines, sizeof(char_u *), 128);
    size_t start = 0;
    int added = 0;
    for (size_t i = 0; i <= length; ++i)
    {
	int at_end = i == length;
	int separator = !at_end && (mac ? data[i] == '\r' : data[i] == '\n');
	if (!separator && !at_end)
	    continue;
	// A trailing separator does not create an extra empty last line.
	if (at_end && start == length)
	    break;
	size_t end = i;
	if (crlf && !mac && end > start && data[end - 1] == '\r')
	    --end;
	char_u *line = vim_strnsave((char_u *)data + start, end - start);
	if (line == NULL)
	{
	    for (int n = 0; n < added; ++n)
		vim_free(((char_u **)lines.ga_data)[n]);
	    ga_clear(&lines);
	    return 0;
	}
	if (ga_grow(&lines, 1) == FAIL)
	{
	    vim_free(line);
	    for (int n = 0; n < added; ++n)
		vim_free(((char_u **)lines.ga_data)[n]);
	    ga_clear(&lines);
	    return 0;
	}
	((char_u **)lines.ga_data)[lines.ga_len++] = line;
	++added;
	if (at_end)
	    break;
	start = i + 1;
    }
    if (added == 0)
    {
	char_u *empty = vim_strsave((char_u *)"");
	if (empty == NULL || ga_grow(&lines, 1) == FAIL)
	{
	    vim_free(empty);
	    ga_clear(&lines);
	    return 0;
	}
	((char_u **)lines.ga_data)[lines.ga_len++] = empty;
	added = 1;
    }

    linenr_T old_count = buf->b_ml.ml_line_count;
    xim_engine_boundary();
    xim_engine_cancel();
    if (u_savecommon((linenr_T)0, old_count + 1, 0, TRUE) == FAIL)
    {
	for (int n = 0; n < added; ++n)
	    vim_free(((char_u **)lines.ga_data)[n]);
	ga_clear(&lines);
	return 0;
    }
    int ok = TRUE;
    for (int i = 0; i < added; ++i)
    {
	char_u *line = ((char_u **)lines.ga_data)[i];
	if (ok)
	    ok = i < old_count
		? ml_replace((linenr_T)(i + 1), line, TRUE) == OK
		: ml_append((linenr_T)i, line, 0, FALSE) == OK;
	// Both APIs above copy the text. The staged allocation stays ours.
	vim_free(line);
    }
    for (linenr_T lnum = old_count; ok && lnum > added; --lnum)
	ok = ml_delete(lnum) == OK;
    ga_clear(&lines);
    // This is a reload, not an edit of a readonly file. Avoid running the
    // FileChangedRO callback from changed() after the import has started.
    changed_internal();
    changed_lines(1, 0, old_count + 1,
	    buf->b_ml.ml_line_count - old_count);
    xim_engine_boundary();
    if (!ok)
    {
	u_undo(1);
	return 0;
    }
    set_fileformat(mac ? EOL_MAC : crlf ? EOL_DOS : EOL_UNIX, OPT_LOCAL);
    buf->b_p_eol = eol;
    buf->b_p_bomb = bomb;
    buf->b_no_eol_lnum = eol ? 0 : buf->b_ml.ml_line_count;
    buf_store_time(buf, &info, buf->b_ffname);
    buf->b_mtime_read = buf->b_mtime;
    buf->b_mtime_read_ns = buf->b_mtime_ns;
    unchanged(buf, TRUE, TRUE);
    u_unchanged(buf);
    check_cursor();
    curwin->w_set_curswant = TRUE;
    redraw_curbuf_later(UPD_NOT_VALID);
    apply_autocmds(EVENT_BUFREADPOST, buf->b_fname, NULL, FALSE, buf);
    return bufref_valid(&ref) ? 1 : 0;
}

/*
 * Explicit reload for the acknowledged Reload action. This is the only path
 * allowed to perform an unbounded synchronous read; the caller is a direct
 * user choice, never an event handler.
 */
    int
xim_engine_reload(int fnum)
{
    buf_T *buf = buflist_findnr(fnum);
    if (buf == NULL)
	return 0;
    int errors = called_emsg;
    if (buf != curbuf)
    {
	char command[64];
	vim_snprintf(command, sizeof(command), "hide buffer %d", fnum);
	do_cmdline_cmd((char_u *)command);
	buf = buflist_findnr(fnum);
	if (buf == NULL || buf != curbuf)
	    return 0;
    }
    do_cmdline_cmd((char_u *)"edit!");
    return called_emsg == errors ? 1 : 0;
}

    int
xim_engine_rename_buffer(int fnum, const char *name)
{
    buf_T *buf = buflist_findnr(fnum);
    if (buf == NULL || name == NULL || *name == NUL)
	return 0;
    // Never wipe another (even unloaded) buffer as a side effect of a
    // namespace operation. The controller can retain the text for recovery.
    buf_T *other = buflist_findname_exp((char_u *)name);
    if (other != NULL && other != buf)
	return 0;
    return setfname(buf, (char_u *)name, NULL, FALSE) == OK;
}

    void
xim_engine_mark_missing(int fnum)
{
    buf_T *buf = buflist_findnr(fnum);
    if (buf == NULL || !bt_normal(buf) || buf->b_ml.ml_mfp == NULL)
	return;
    // The only remaining copy now lives in memory. Protect it on Close/Quit
    // even if it was clean before deletion, and after any undo/redo.
    if (!buf->b_changed)
    {
	buf->b_changed = TRUE;
	++CHANGEDTICK(buf);
	ml_setflags(buf);
	check_status(buf);
	redraw_tabline = TRUE;
	need_maketitle = TRUE;
    }
    u_unchanged(buf);
}

    void
xim_engine_notice(const char *text)
{
    if (text == NULL || *text == NUL)
	return;
    msg((char *)text);
    redraw_cmdline = TRUE;
}

    void
xim_engine_status_note(const char *text)
{
    char_u line[512];
    const char *note = text == NULL ? "" : text;
    vim_snprintf((char *)line, sizeof(line),
	    "%%#XimStatus# Xim %%f %%m%%=%%#XimAlert#%s"
	    "%%#XimStatus# %%l:%%c ", note);
    set_option_value((char_u *)"statusline", 0, line, 0);
    redraw_curbuf_later(UPD_VALID);
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
    // Report real undo state without side effects. undo_allowed() is not
    // called here: it emits E21 when 'modifiable' is off, which would spam
    // every menu render and prompt key. The engine still refuses the
    // operation when the buffer cannot be changed.
    if (!curbuf->b_p_ma)
        return FALSE;
    if (redo)
        return curbuf->b_u_curhead != NULL;
    if (curbuf->b_u_curhead != NULL)
        // More to undo when another header lies past the current one.
        return curbuf->b_u_curhead->uh_next.ptr != NULL;
    return curbuf->b_u_newhead != NULL;
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
    // A captured window can close before the release arrives (native Ctrl-W,
    // :only, a buffer close). Validate the pointer against the live window
    // list before using it, so a stale capture cannot read freed memory.
    if (xim_sep_window != NULL && !win_valid(xim_sep_window))
    {
        // The captured window is gone; the drag has nothing to resize.
        xim_sep_window = NULL;
        return;
    }
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
    if (wp == NULL)
        return;
    // Save the scrolled window's own cursor and the global selection. A
    // same-buffer split keeps one cursor per window, so saving only the
    // current window's cursor and restoring it by buffer identity moved the
    // other window's insertion point.
    pos_T saved_cursor = wp->w_cursor;
    pos_T saved_anchor = selection_anchor;
    int saved_selbuf = selection_buffer;
    int saved_visual = VIsual_active;
    pos_T saved_visual_pos = VIsual;
    int saved_visual_mode = VIsual_mode;
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
    wp->w_cursor = saved_cursor;
    selection_anchor = saved_anchor;
    selection_buffer = saved_selbuf;
    VIsual_active = saved_visual;
    VIsual = saved_visual_pos;
    VIsual_mode = saved_visual_mode;
    wp->w_redr_status = TRUE;
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

    /*
     * Build the literal search pattern for native find/replace.  The text is
     * taken verbatim (\V very nomagic) with backslashes escaped and newlines
     * written as "\n"; the case rule is explicit (\c case-insensitive,
     * \C case-sensitive) so it never depends on the 'ignorecase' option.
     * Caller frees the result.
     */
    static char_u *
xim_literal_pattern(const char *text, int case_sensitive)
{
    char_u *escaped = vim_strsave_escaped((char_u *)text, (char_u *)"\\");
    char_u *pattern;
    size_t i;
    size_t end;
    size_t nls = 0;
    if (escaped == NULL)
        return NULL;
    for (i = 0; escaped[i] != NUL; ++i)
        if (escaped[i] == NL)
            ++nls;
    pattern = alloc(STRLEN(escaped) + 5 + nls);
    if (pattern == NULL)
    {
        vim_free(escaped);
        return NULL;
    }
    STRCPY(pattern, (char_u *)"\\V");
    STRCAT(pattern, (char_u *)(case_sensitive ? "\\C" : "\\c"));
    for (i = 0; escaped[i] != NUL; ++i)
    {
        if (escaped[i] == NL)
        {
            STRCAT(pattern, (char_u *)"\\n");
        }
        else
        {
            end = STRLEN(pattern);
            pattern[end] = escaped[i];
            pattern[end + 1] = NUL;
        }
    }
    vim_free(escaped);
    return pattern;
}

// One literal match: line, byte offset of the start, byte length.  The
// length can differ from the needle length under Unicode case folding.
typedef struct
{
    linenr_T lnum;
    size_t   col;
    size_t   len;
} xim_span_T;

// Cached compiled literal pattern so per-line scans do not recompile.
static regprog_T *xim_literal_prog = NULL;
static char_u *xim_literal_prog_text = NULL;
static int xim_literal_prog_case = -1;

/*
 * Return the compiled literal pattern for text and case rule, reusing the
 * cached program when they are unchanged.  Returns NULL when compilation
 * fails, in which case only byte-exact matching remains.
 */
    static regprog_T *
xim_pattern_prog(const char *text, int case_sensitive)
{
    char_u *pattern;
    char_u *copy;

    if (xim_literal_prog != NULL && xim_literal_prog_case == case_sensitive
            && xim_literal_prog_text != NULL
            && STRCMP(xim_literal_prog_text, (char_u *)text) == 0)
        return xim_literal_prog;
    pattern = xim_literal_pattern(text, case_sensitive);
    if (pattern == NULL)
        return NULL;
    copy = vim_strsave((char_u *)text);
    if (copy == NULL)
    {
        vim_free(pattern);
        return NULL;
    }
    if (xim_literal_prog != NULL)
        vim_regfree(xim_literal_prog);
    xim_literal_prog = vim_regcomp(pattern, RE_MAGIC + RE_STRING);
    vim_free(pattern);
    if (xim_literal_prog == NULL)
    {
        vim_free(copy);
        return NULL;
    }
    vim_free(xim_literal_prog_text);
    xim_literal_prog_text = copy;
    xim_literal_prog_case = case_sensitive;
    return xim_literal_prog;
}

    void
xim_engine_find(const char *text, int backward, int case_sensitive)
{
    pos_T result = curwin->w_cursor;
    char_u *pattern;
    if (*text == NUL)
        return;
    xim_engine_cancel();
    pattern = xim_literal_pattern(text, case_sensitive);
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

    static int
xim_ascii_needle(char_u *needle, size_t nlen)
{
    size_t i;
    for (i = 0; i < nlen; ++i)
        if (needle[i] >= 0x80)
            return FALSE;
    return TRUE;
}

    static int
xim_line_ascii(char_u *line, size_t len)
{
    size_t i;
    for (i = 0; i < len; ++i)
        if (line[i] >= 0x80)
            return FALSE;
    return TRUE;
}

    static int
xim_match_at(char_u *line, char_u *needle, size_t nlen, int case_sensitive,
        int ascii)
{
    size_t i;
    if (case_sensitive)
        return STRNCMP(line, needle, nlen) == 0;
    if (ascii)
    {
        // An all-ASCII needle can only match ASCII bytes, so fold bytes
        // directly and skip the UTF-8 decode on the common path.
        for (i = 0; i < nlen; ++i)
        {
            int left = line[i] >= 'A' && line[i] <= 'Z' ? line[i] + 32 : line[i];
            int right = needle[i] >= 'A' && needle[i] <= 'Z'
                ? needle[i] + 32 : needle[i];
            if (left != right)
                return FALSE;
        }
        return TRUE;
    }
    return mb_strnicmp2(line, needle, nlen, nlen) == 0;
}

/*
 * Find the next literal match in "line" (length "len") at or after byte
 * "col".  Return the byte length of the match, or 0 when there is none,
 * and store the match start byte offset in "*start".  "fast" selects the
 * byte path, which is valid when matching is case-sensitive or both the
 * needle and the line are all ASCII; otherwise the compiled literal
 * regexp is used so count, replace, highlight and navigation agree under
 * Vim's full case folding (for example a long s matches "s").
 */
    static size_t
xim_span_in_line(char_u *line, size_t len, size_t col, const char *needle,
        size_t nlen, int case_sensitive, int fast, regprog_T *prog,
        size_t *start)
{
    if (fast)
    {
        size_t i = col;
        while (i + nlen <= len)
        {
            if (xim_match_at(line + i, (char_u *)needle, nlen,
                        case_sensitive, TRUE))
            {
                *start = i;
                return nlen;
            }
            ++i;
        }
        return 0;
    }
    if (prog == NULL)
        return 0;
    {
        regmatch_T rm;
        rm.regprog = prog;
        if (vim_regexec_nl(&rm, line, (colnr_T)col))
        {
            size_t s = (size_t)(rm.startp[0] - line);
            size_t e = (size_t)(rm.endp[0] - line);
            if (e <= s || s < col)
                return 0;
            *start = s;
            return e - s;
        }
    }
    return 0;
}

    /*
     * Count literal matches of text in the current buffer under the case rule,
     * capped so a huge match set cannot run away.  Non-overlapping and in
     * agreement with the window highlight regexp.
     */
    static long
xim_count_matches(const char *text, int case_sensitive, long cap)
{
    long count = 0;
    size_t nlen = STRLEN(text);
    linenr_T lnum;
    int ascii;
    regprog_T *prog;

    if (nlen == 0)
        return 0;
    ascii = xim_ascii_needle((char_u *)text, nlen);
    prog = xim_pattern_prog(text, case_sensitive);
    for (lnum = 1; lnum <= curbuf->b_ml.ml_line_count && count < cap; ++lnum)
    {
        char_u *line = ml_get(lnum);
        colnr_T len = ml_get_buf_len(curbuf, lnum);
        int fast = case_sensitive || (ascii && xim_line_ascii(line, len));
        size_t col = 0;
        while (col < (size_t)len)
        {
            size_t start = 0;
            size_t mlen = xim_span_in_line(line, len, col, text, nlen,
                    case_sensitive, fast, prog, &start);
            if (mlen == 0)
                break;
            ++count;
            if (count >= cap)
                return count;
            col = start + mlen;
        }
    }
    return count;
}

    static int xim_find_match_id = 0;
// The window that owns the match list; match IDs are window-local, so the
// highlight must be deleted from its own window even after a menu action
// changed the current window.
static win_T *xim_find_match_win = NULL;

    int
xim_engine_peek_key(void)
{
    return vpeekc();
}

    void
xim_engine_clear_highlight(void)
{
    win_T *wp;
    char command[64];

    if (xim_find_match_id <= 0)
        return;
    // Match IDs are window-local: delete from the window that owns the
    // highlight, which can differ from the current window after a menu
    // action opened another window (for example Help).
    wp = win_valid(xim_find_match_win) ? xim_find_match_win : curwin;
    if (wp == curwin)
        vim_snprintf(command, sizeof(command),
                "silent! call matchdelete(%d)", xim_find_match_id);
    else
        vim_snprintf(command, sizeof(command),
                "silent! call matchdelete(%d, %d)", xim_find_match_id,
                wp->w_id);
    do_cmdline_cmd((char_u *)command);
    xim_find_match_id = 0;
    xim_find_match_win = NULL;
    if (wp == curwin)
        redraw_curbuf_later(UPD_NOT_VALID);
    else
        redraw_win_later(wp, UPD_NOT_VALID);
}

    void
xim_engine_highlight(const char *text, int case_sensitive, int *count)
{
    char_u *pattern;
    typval_T argv[5];
    typval_T rettv;
    long matches = 0;

    xim_engine_clear_highlight();
    if (text == NULL || *text == NUL)
    {
        if (count != NULL)
            *count = 0;
        return;
    }
    matches = xim_count_matches(text, case_sensitive, 1000000L);
    if (count != NULL)
        *count = (int)matches;
    if (matches == 0)
        return;
    pattern = xim_literal_pattern(text, case_sensitive);
    if (pattern == NULL)
        return;
    CLEAR_FIELD(argv);
    CLEAR_FIELD(rettv);
    rettv.v_type = VAR_NUMBER;
    argv[0].v_type = VAR_STRING;
    argv[0].vval.v_string = (char_u *)"XimFindMatch";
    argv[1].v_type = VAR_STRING;
    argv[1].vval.v_string = pattern;
    argv[2].v_type = VAR_NUMBER;
    argv[2].vval.v_number = 100;
    f_matchadd(argv, &rettv);
    vim_free(pattern);
    if (rettv.vval.v_number > 0)
    {
        xim_find_match_id = (int)rettv.vval.v_number;
        xim_find_match_win = curwin;
    }
    redraw_curbuf_later(UPD_NOT_VALID);
}

/*
 * Smart case must see uppercase in any script: report whether the UTF-8
 * text contains a codepoint that changes under lowercasing.  Used by the
 * controller for the default case rule.
 */
    int
xim_engine_has_upper(const char *text)
{
    char_u *p = (char_u *)text;
    int c;

    while (*p != NUL)
    {
        c = mb_ptr2char_adv(&p);
        if (utf_tolower(c) != c)
            return 1;
    }
    return 0;
}

    int
xim_engine_modifiable(void)
{
    return curbuf->b_p_ma && !curbuf->b_p_ro ? 1 : 0;
}

/*
 * Build the produced lines for one original line: every span is replaced
 * by "replace" (which may contain newlines, splitting the line).  "*out"
 * receives one allocated char_u * per produced line; the caller frees
 * each line and the array.  Returns FAIL only on allocation failure.
 */
    static int
xim_build_spliced(char_u *line, size_t len, const xim_span_T *spans, int nspans,
        const char *replace, garray_T *out)
{
    garray_T text;
    size_t pos = 0;
    int i;

    ga_init2(&text, 1, (int)len + 64);
    ga_init2(out, sizeof(char_u *), 4);
    for (i = 0; i < nspans; ++i)
    {
        if (spans[i].col > pos)
            ga_concat_len(&text, line + pos, spans[i].col - pos);
        ga_concat_len(&text, (char_u *)replace, STRLEN(replace));
        pos = spans[i].col + spans[i].len;
    }
    if (pos < len)
        ga_concat_len(&text, line + pos, len - pos);
    ga_append(&text, NUL);
    if (text.ga_data != NULL)
    {
        char_u *content = (char_u *)text.ga_data;
        char_u *endp = content + STRLEN((char *)content);
        char_u *p = content;
        char_u *seg = content;

        for (;;)
        {
            if (p == endp || *p == NL)
            {
                char_u *copy = vim_strnsave(seg, p - seg);

                if (copy == NULL)
                    return FAIL;
                if (ga_grow(out, 1) == FAIL)
                {
                    vim_free(copy);
                    return FAIL;
                }
                ((char_u **)out->ga_data)[out->ga_len++] = copy;
                if (p == endp)
                    break;
                seg = p + 1;
            }
            ++p;
        }
    }
    ga_clear(&text);
    return OK;
}

    static void
xim_free_lines(garray_T *out)
{
    int i;

    for (i = 0; i < out->ga_len; ++i)
        vim_free(((char_u **)out->ga_data)[i]);
    ga_clear(out);
}

/*
 * Commit the produced lines for one original line.  The u_save range covers
 * the line replacement and the adjacent insertions, like ":s" with "\r":
 * one undo restores the saved block and removes the appended lines.
 */
    static int
xim_splice_line(linenr_T lnum, garray_T *out)
{
    int i;

    if (out->ga_len < 1 || u_save(lnum - 1, lnum + 1) != OK)
        return FAIL;
    if (ml_replace(lnum, ((char_u **)out->ga_data)[0], TRUE) == FAIL)
        return FAIL;
    for (i = 1; i < out->ga_len; ++i)
        if (ml_append(lnum + i - 1, ((char_u **)out->ga_data)[i],
                    (colnr_T)0, FALSE) == FAIL)
            return FAIL;
    return OK;
}

/*
 * Collect the literal match spans of the whole buffer, capped like the
 * prompt count so memory stays bounded.  Returns the span count.
 */
    static long
xim_collect_spans(const char *text, int case_sensitive, garray_T *spans,
        long cap)
{
    size_t nlen = STRLEN(text);
    linenr_T lnum;
    int ascii;
    regprog_T *prog;

    ga_init2(spans, sizeof(xim_span_T), 256);
    if (nlen == 0)
        return 0;
    ascii = xim_ascii_needle((char_u *)text, nlen);
    prog = xim_pattern_prog(text, case_sensitive);
    for (lnum = 1; lnum <= curbuf->b_ml.ml_line_count; ++lnum)
    {
        char_u *line = ml_get(lnum);
        colnr_T len = ml_get_buf_len(curbuf, lnum);
        int fast = case_sensitive || (ascii && xim_line_ascii(line, len));
        size_t col = 0;

        while (col < (size_t)len)
        {
            xim_span_T span;
            size_t start = 0;
            size_t mlen = xim_span_in_line(line, len, col, text, nlen,
                    case_sensitive, fast, prog, &start);

            if (mlen == 0)
                break;
            span.lnum = lnum;
            span.col = start;
            span.len = mlen;
            if (spans->ga_len >= cap)
                return spans->ga_len;
            if (ga_grow(spans, 1) == FAIL)
                return spans->ga_len;
            ((xim_span_T *)spans->ga_data)[spans->ga_len++] = span;
            col = start + mlen;
        }
    }
    return spans->ga_len;
}

    /*
     * Replace every literal match of find with replace in the current buffer.
     * All replacements form one undo block; matching is UTF-8 aware and uses
     * the explicit case rule, so it agrees with the highlight.  A
     * replacement containing newlines splits into real lines.  Returns the
     * replacement count.
     */
    int
xim_engine_replace_all(const char *find, const char *replace, int case_sensitive,
        int *count)
{
    long replaced = 0;
    size_t flen = STRLEN(find);
    size_t rlen = STRLEN(replace);
    int multiline = vim_strchr((char_u *)replace, NL) != NULL;

    if (count != NULL)
        *count = 0;
    if (flen == 0 || !xim_engine_modifiable())
        return 0;

    if (!multiline)
    {
        garray_T newline;
        int ascii = xim_ascii_needle((char_u *)find, flen);
        regprog_T *prog = xim_pattern_prog(find, case_sensitive);
        linenr_T lnum;

        ++no_u_sync;
        for (lnum = 1; lnum <= curbuf->b_ml.ml_line_count; ++lnum)
        {
            char_u *line = ml_get(lnum);
            colnr_T len = ml_get_buf_len(curbuf, lnum);
            int fast = case_sensitive || (ascii && xim_line_ascii(line, len));
            size_t col = 0;
            size_t out_col = 0;
            int changed = FALSE;
            long hits = 0;

            ga_init2(&newline, 1, (int)len + 16);
            while (col < (size_t)len)
            {
                size_t start = 0;
                size_t mlen = xim_span_in_line(line, len, col, find, flen,
                        case_sensitive, fast, prog, &start);

                if (mlen == 0)
                {
                    if (out_col < len)
                        ga_concat_len(&newline, line + out_col,
                                len - out_col);
                    break;
                }
                if (start > out_col)
                    ga_concat_len(&newline, line + out_col, start - out_col);
                if (rlen > 0)
                    ga_concat_len(&newline, (char_u *)replace, rlen);
                col = start + mlen;
                out_col = col;
                ++hits;
                changed = TRUE;
            }
            if (changed)
            {
                ga_append(&newline, NUL);
                if (u_savesub(lnum) == OK)
                {
                    ml_replace(lnum, newline.ga_data, TRUE);
                    replaced += hits;
                }
                // A failed undo save leaves the line and the count alone.
            }
            ga_clear(&newline);
        }
        --no_u_sync;
    }
    else
    {
        garray_T spans;
        long total = xim_collect_spans(find, case_sensitive, &spans,
                1000000L);

        if (total > 0)
        {
            int i = (int)total - 1;

            ++no_u_sync;
            // Splices change the line count, so commit bottom-up and the
            // line numbers of earlier spans stay valid.
            while (i >= 0)
            {
                linenr_T span_lnum = ((xim_span_T *)spans.ga_data)[i].lnum;
                int first = i;
                garray_T out;

                while (first > 0
                        && ((xim_span_T *)spans.ga_data)[first - 1].lnum
                            == span_lnum)
                    --first;
                if (xim_build_spliced(ml_get(span_lnum),
                            ml_get_buf_len(curbuf, span_lnum),
                            (xim_span_T *)spans.ga_data + first,
                            i - first + 1, replace, &out) == OK)
                {
                    if (xim_splice_line(span_lnum, &out) == OK)
                        replaced += i - first + 1;
                }
                xim_free_lines(&out);
                i = first - 1;
            }
            --no_u_sync;
        }
        ga_clear(&spans);
    }
    u_sync(TRUE);
    if (replaced > 0)
    {
        changed_lines(1, 0, curbuf->b_ml.ml_line_count + 1, 0);
        check_cursor();
        curwin->w_set_curswant = TRUE;
        redraw_curbuf_later(UPD_NOT_VALID);
    }
    if (count != NULL)
        *count = (int)replaced;
    return (int)replaced;
}

    /*
     * Replace the first literal match at or after the cursor and leave the
     * cursor after the replacement.  One undo block; UTF-8 aware; a
     * replacement with newlines splits into real lines.
     */
    int
xim_engine_replace_one(const char *find, const char *replace, int case_sensitive)
{
    size_t flen = STRLEN(find);
    size_t rlen = STRLEN(replace);
    linenr_T lnum;
    size_t start_col;
    int ascii;
    regprog_T *prog;

    if (flen == 0 || !xim_engine_modifiable())
        return 0;
    ascii = xim_ascii_needle((char_u *)find, flen);
    prog = xim_pattern_prog(find, case_sensitive);
    lnum = curwin->w_cursor.lnum;
    start_col = curwin->w_cursor.col;
    for (; lnum <= curbuf->b_ml.ml_line_count; ++lnum, start_col = 0)
    {
        char_u *line = ml_get(lnum);
        colnr_T len = ml_get_buf_len(curbuf, lnum);
        int fast = case_sensitive || (ascii && xim_line_ascii(line, len));
        size_t col = start_col;

        while (col < (size_t)len)
        {
            size_t start = 0;
            size_t mlen = xim_span_in_line(line, len, col, find, flen,
                    case_sensitive, fast, prog, &start);

            if (mlen > 0)
            {
                xim_span_T span;
                garray_T out;
                char_u *tail;

                span.lnum = lnum;
                span.col = start;
                span.len = mlen;
                if (xim_build_spliced(line, len, &span, 1, replace, &out)
                        != OK)
                {
                    xim_free_lines(&out);
                    return 0;
                }
                if (xim_splice_line(lnum, &out) != OK)
                {
                    xim_free_lines(&out);
                    return 0;
                }
                // Cursor after the replacement: on the last produced line,
                // at the byte length of the replacement tail.
                tail = (char_u *)replace + rlen;
                while (tail > (char_u *)replace && tail[-1] != NL)
                    --tail;
                curwin->w_cursor.lnum = lnum + out.ga_len - 1;
                curwin->w_cursor.col = (colnr_T)((char_u *)replace + rlen - tail);
                xim_free_lines(&out);
                check_cursor();
                curwin->w_set_curswant = TRUE;
                u_sync(TRUE);
                changed_lines(lnum, 0, curwin->w_cursor.lnum + 1, 0);
                redraw_curbuf_later(UPD_NOT_VALID);
                return 1;
            }
            col += fast ? 1 : (size_t)(*mb_ptr2len)(line + col);
        }
    }
    return 0;
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
        // Share one full-screen snapshot for prompts and menus so a
        // top drop-down and a bottom prompt do not overwrite each other.
        // The snapshot is refreshed only when neither overlay is visible;
        // freeing it while the prompt still owns it left a dangling pointer.
        if (!overlay_visible)
        {
            vim_free(overlay_background);
            overlay_background = ALLOC_CLEAR_MULT(xim_cell_T, (size_t)Rows * Columns);
            if (overlay_background != NULL)
                for (int rr = 0; rr < Rows; ++rr)
                    for (int cc = 0; cc < Columns; ++cc)
                    {
                        xim_cell_T *cell = &overlay_background[(size_t)rr * Columns + cc];
                        screen_getbytes(rr, cc, cell->bytes, &cell->attr);
                    }
            overlay_frame_rows = Rows;
            overlay_frame_columns = Columns;
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
                    else
                        screen_putchar(' ', r, cc, cell->attr);
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
    // Terminal output optimization skips unchanged popup cells, so the
    // cursor would otherwise land on whichever cell last changed (often a
    // menu-bar heading). Anchor it to the last visible drop-down cell so the
    // overlay frame is deterministic.
    if (r > row)
    {
        int last_row = r - 1;
        if (last_row > Rows - 2)
            last_row = Rows - 2;
        if (last_row >= row)
            windgoto(last_row, MIN(col + width, (int)Columns - 1));
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
                // A blank position stores NUL; repaint it as a space so menu
                // filler over blank cells does not survive dismissal.
                if (*cell->bytes != NUL)
                    screen_puts(cell->bytes, r, cc, cell->attr);
                else
                    screen_putchar(' ', r, cc, cell->attr);
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
xim_engine_overlay(const char *prompt, const char *items, const char *info,
        int error)
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
        overlay_frame_rows = Rows;
        overlay_frame_columns = Columns;
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
                    else
                        screen_putchar(' ', r, col, cell->attr);
                }
        else
        {
            redraw_all_later(UPD_NOT_VALID);
            update_screen(0);
        }
    }
    overlay_visible = TRUE;
    overlay_top = row;
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
    if (info != NULL && *info != NUL)
    {
        // Right-aligned status (match count, case rule) on the prompt row.
        int info_attr = syn_name2attr((char_u *)"XimPromptInfo");
        if (info_attr == 0)
            info_attr = attr;
        int info_cells = vim_strsize((char_u *)info);
        int info_col = (int)Columns - info_cells;
        if (info_col < 0)
            info_col = 0;
        screen_puts_len((char_u *)info, (int)STRLEN(info), (int)Rows - 1,
                info_col, info_attr);
    }
    {
        int cursor_cells = 0;
        int length_cells = 0;
        extern void xim_prompt_get(int *, int *);
        // The controller reports the caret cell inside the full prompt text.
        xim_prompt_get(&cursor_cells, &length_cells);
        (void)length_cells;
        int cursor_col = cursor_cells;
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
    if (!xim_prompt_active() && !xim_menu_active())
    {
        if (menu_visible)
            xim_clear_menu_popup();
        return;
    }
    if (overlay_redraw_guard)
        return;
    if (overlay_frame_rows != Rows || overlay_frame_columns != Columns)
    {
        // A resize invalidates the stored frame and both overlay layouts.
        // Repaint the document before recapturing, or the snapshot would
        // hold the overlay pixels instead of the editor underneath.
        overlay_visible = FALSE;
        menu_visible = FALSE;
        overlay_redraw_guard = TRUE;
        redraw_all_later(UPD_NOT_VALID);
        update_screen(0);
        overlay_redraw_guard = FALSE;
    }
    // The engine has produced a frame. Repaint the overlays on top of it;
    // the snapshot is only refreshed above, on a size change.
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
    xim_recent_init();
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
    do_highlight((char_u *)"default link XimFindMatch Search", FALSE, FALSE);
    do_highlight((char_u *)"default link XimPromptInfo Comment", FALSE, FALSE);
    do_highlight((char_u *)"default link XimAlert WarningMsg", FALSE, FALSE);
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
    // Preserve Alt through decoding for this read: merge_modifyOtherKeys
    // would otherwise fold Alt into the character's high bit before the
    // native seam can see the modifier.
    xim_preserve_modifiers = TRUE;
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
            xim_preserve_modifiers = FALSE;
            c = K_IGNORE;
            goto received;
        }
        xim_render();
    }
    c = safe_vgetc();
    xim_preserve_modifiers = FALSE;
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
        // K_MOUSELEFT is the inherited name for wheel-right and
        // K_MOUSERIGHT for wheel-left; map each to its view direction so
        // the gesture matches the movement.
        else if (c == K_MOUSELEFT)
            xim_mouse_wheel(mouse_row, mouse_col, 3, m_modifiers);
        else if (c == K_MOUSERIGHT)
            xim_mouse_wheel(mouse_row, mouse_col, 2, m_modifiers);
        else if (c == K_MOUSEMOVE)
            xim_mouse_move(mouse_row, mouse_col, m_modifiers);
        got_int = FALSE;
        State = MODE_INSERT;
        check_cursor();
        curwin->w_redr_status = TRUE;
        redraw_curbuf_later(UPD_VALID);
        xim_set_hover(xim_menu_active());
        // A menu dismissed while a prompt is open still has to be erased;
        // the shared snapshot stays valid for the prompt's own rows.
        if (!xim_menu_active() && menu_visible)
            xim_clear_menu_popup();
        if (!xim_prompt_active() && overlay_visible && !menu_visible)
        {
            redraw_all_later(UPD_NOT_VALID);
            clear_cmdline = TRUE;
            redraw_cmdline = TRUE;
            overlay_visible = FALSE;
            VIM_CLEAR(overlay_background);
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
        case K_S_TAB: modifiers |= XIM_SHIFT; /* FALLTHROUGH */
        case K_TAB: key = '\t'; break;
        case K_F1: key = XIM_F1; break;
        case K_F2: key = XIM_F2; break;
        case K_S_F3: modifiers |= XIM_SHIFT; /* FALLTHROUGH */
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
    // Clear a dismissed menu even while a prompt is open. The prompt snapshot
    // remains valid for its own rows, so only the menu rectangle is restored.
    if (!xim_menu_active() && menu_visible)
        xim_clear_menu_popup();
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
    }
}
