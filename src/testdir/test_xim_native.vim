" Native UI screen assertions; executed by the compatibility Vim test runner.
source util/check.vim
CheckScreendump
source util/screendump.vim
source util/term_util.vim

func s:StartXim(theme = 'default', readonly = 0)
  if empty($XIM_BINARY)
    throw 'Skipped: XIM_BINARY is required'
  endif
  call writefile(['alpha beta', "tab\there", 'third line'], 'Xxim.txt')
  let command = [$XIM_BINARY, '-n', '-X', '--cmd', 'set t_u7= t_RB= t_RF= t_RV= t_RK=', '-c', 'set background=light', '-c', 'colorscheme ' .. a:theme, 'Xxim.txt']
  if a:readonly
    call add(command, '-R')
  endif
  let buf = RunVimInTerminal('', #{cmd: command, rows: 10, cols: 75, wait_for_ruler: 0})
  call WaitForAssert({-> assert_match('Xim Xxim.txt', term_getline(buf, 9))})
  return buf
endfunc

func s:StopXim(buf)
  call term_sendkeys(a:buf, "\<F1>Ex command\<CR>")
  call WaitForAssert({-> assert_match('Ex:', term_getline(a:buf, 10))})
  call term_sendkeys(a:buf, "qall!\<CR>")
  call WaitForAssert({-> assert_equal('finished', term_getstatus(a:buf))})
  execute a:buf .. 'bwipe!'
  call delete('Xxim.txt')
endfunc

func s:StartBlankXim()
  if empty($XIM_BINARY)
    throw 'Skipped: XIM_BINARY is required'
  endif
  let command = [$XIM_BINARY, '-n', '-X', '--cmd', 'set t_u7= t_RB= t_RF= t_RV= t_RK=', '-c', 'set background=light', '-c', 'colorscheme default']
  let buf = RunVimInTerminal('', #{cmd: command, rows: 10, cols: 75, wait_for_ruler: 0})
  call WaitForAssert({-> assert_match('Xim', term_getline(buf, 9))})
  return buf
endfunc

func s:Dump(buf, name)
  call VerifyScreenDump(a:buf, a:name,
        \ {'FileComparisonPreAction': function('s:TrimDumpFinalBlank')})
endfunc

func s:TrimDumpFinalBlank(_state, testdump, refdump)
  call map(a:testdump, 'substitute(v:val, " $", "", "")')
  call map(a:refdump, 'substitute(v:val, " $", "", "")')
endfunc

func Test_xim_native_screens()
  let buf = s:StartXim()
  call s:Dump(buf, 'Test_xim_status')
  call term_sendkeys(buf, "\<S-Right>\<S-Right>\<S-Right>")
  call WaitForAssert({-> assert_match('1:4', term_getline(buf, 9))})
  call s:Dump(buf, 'Test_xim_selection')
  call term_sendkeys(buf, "Z\<C-O>example.txt")
  call WaitForAssert({-> assert_match('Open: example.txt', term_getline(buf, 10))})
  call s:Dump(buf, 'Test_xim_open')
  call term_sendkeys(buf, "\<Esc>")
  call TermWait(buf, 50)
  call term_sendkeys(buf, "\<C-F>third")
  call WaitForAssert({-> assert_match('Find: third', term_getline(buf, 10))})
  call s:Dump(buf, 'Test_xim_find')
  call term_sendkeys(buf, "\<Esc>")
  call TermWait(buf, 50)
  call term_sendkeys(buf, "\<F1>Save")
  call WaitForAssert({-> assert_match('Command: Save', term_getline(buf, 10))})
  call s:Dump(buf, 'Test_xim_palette')
  call term_sendkeys(buf, "\<Esc>")
  call TermWait(buf, 50)
  call term_sendkeys(buf, "\<C-Q>")
  call WaitForAssert({-> assert_match('Save changes?', term_getline(buf, 10))})
  call s:Dump(buf, 'Test_xim_unsaved')
  call term_sendkeys(buf, "\<Esc>")
  call TermWait(buf, 50)
  call s:StopXim(buf)
endfunc

func Test_xim_native_theme_screens()
  for theme in ['desert', 'catppuccin']
    let buf = s:StartXim(theme)
    call term_sendkeys(buf, "\<F1>Save")
    call WaitForAssert({-> assert_match('Command: Save', term_getline(buf, 10))})
    call s:Dump(buf, 'Test_xim_theme_' .. theme)
    call term_sendkeys(buf, "\<Esc>")
    call TermWait(buf, 50)
    call s:StopXim(buf)
  endfor
endfunc

func Test_xim_palette_scroll()
  let buf = s:StartXim()
  call term_sendkeys(buf, "\<F1>" .. repeat("\<Down>", 15))
  call WaitForAssert({-> assert_match('> Ex command', term_getline(buf, 8))}, 1000)
  call s:Dump(buf, 'Test_xim_palette_scroll')
  call term_sendkeys(buf, "\<Esc>")
  call TermWait(buf, 50)
  call WaitForAssert({-> assert_match('alpha beta', term_getline(buf, 2))})
  call s:StopXim(buf)
endfunc

func Test_xim_overlay_restoration()
  let buf = s:StartXim()
  call term_sendkeys(buf, "\<F1>Save")
  call WaitForAssert({-> assert_match('Command: Save', term_getline(buf, 10))})
  call term_sendkeys(buf, "\<Down>\<Up>\<Esc>")
  call WaitForAssert({-> assert_match('alpha beta', term_getline(buf, 2))})
  call s:Dump(buf, 'Test_xim_status')
  call term_sendkeys(buf, "\<F1>Save")
  call WaitForAssert({-> assert_match('Command: Save', term_getline(buf, 10))})
  call term_setsize(buf, 7, 45)
  call TermWait(buf, 100)
  call WaitForAssert({-> assert_match('Command: Save', term_getline(buf, 7))})
  call term_setsize(buf, 10, 75)
  call TermWait(buf, 100)
  call WaitForAssert({-> assert_match('Command: Save', term_getline(buf, 10))})
  call term_sendkeys(buf, "\<Esc>")
  call WaitForAssert({-> assert_match('alpha beta', term_getline(buf, 2))})
  call s:Dump(buf, 'Test_xim_status')
  call s:StopXim(buf)
endfunc

func Test_xim_failed_unnamed_save()
  let buf = s:StartBlankXim()
  call term_sendkeys(buf, "unsaved")
  call WaitForAssert({-> assert_match('unsaved', term_getline(buf, 2))})
  call term_sendkeys(buf, "\<C-Q>")
  call WaitForAssert({-> assert_match('Save changes?', term_getline(buf, 10))})
  call term_sendkeys(buf, 's')
  call WaitForAssert({-> assert_match('Save as:', term_getline(buf, 10))})
  call term_sendkeys(buf, "missing/parent/dialog.txt\<CR>")
  call WaitForAssert({-> assert_match('E212:', term_getline(buf, 8))})
  call assert_match('Save changes?', term_getline(buf, 10))
  call s:Dump(buf, 'Test_xim_failed_unnamed_save')
  call term_sendkeys(buf, 'd')
  call WaitForAssert({-> assert_equal('finished', term_getstatus(buf))})
  execute buf .. 'bwipe!'
endfunc

func s:Screen(buf)
  return join(map(range(1, 10), {-> term_getline(a:buf, v:val)}), "\n")
endfunc

func Test_xim_project_pickers()
  call mkdir('Xximproj/src', 'p')
  call writefile(['alpha', ''], 'Xximproj/src/alpha.cpp')
  call writefile(['beta', ''], 'Xximproj/beta.cpp')
  let command = [$XIM_BINARY, '-n', '-X', '--cmd', 'set t_u7= t_RB= t_RF= t_RV= t_RK=',
        \ '-c', 'set background=light', '-c', 'colorscheme default', 'Xximproj']
  let buf = RunVimInTerminal('', #{cmd: command, rows: 10, cols: 75, wait_for_ruler: 0})
  call WaitForAssert({-> assert_match('Xim', term_getline(buf, 9))})
  " Quick open lists project-relative paths once the background index lands.
  call term_sendkeys(buf, "\<C-P>")
  call WaitForAssert({-> assert_match('Files:', s:Screen(buf))})
  call WaitForAssert({-> assert_match('src/alpha\.cpp', s:Screen(buf))})
  call s:Dump(buf, 'Test_xim_quick_open')
  call term_sendkeys(buf, "\<Esc>")
  " The explorer expands a directory lazily.
  call term_sendkeys(buf, "\<C-E>")
  call WaitForAssert({-> assert_match('Explorer:', s:Screen(buf))})
  call WaitForAssert({-> assert_match('+ src/', s:Screen(buf))})
  call term_sendkeys(buf, "\<CR>")
  call WaitForAssert({-> assert_match('- src/', s:Screen(buf))})
  call WaitForAssert({-> assert_match('src/alpha\.cpp', s:Screen(buf))})
  call s:Dump(buf, 'Test_xim_explorer')
  call term_sendkeys(buf, "\<Esc>")
  call s:StopXim(buf)
  call delete('Xximproj', 'rf')
endfunc

func Test_xim_failed_confirmation_save()
  let buf = s:StartXim('default', 1)
  call term_sendkeys(buf, "Z\<C-Q>")
  call WaitForAssert({-> assert_match('Save changes?', term_getline(buf, 10))})
  call term_sendkeys(buf, 's')
  call WaitForAssert({-> assert_match('E45:', term_getline(buf, 8))})
  call assert_match('Save changes?', term_getline(buf, 10))
  call s:Dump(buf, 'Test_xim_failed_save')
  call term_sendkeys(buf, "\<Esc>")
  call TermWait(buf, 50)
  call WaitForAssert({-> assert_match('Zalpha beta', term_getline(buf, 2))})
  call s:StopXim(buf)
endfunc

func s:StartProjectXim(layout)
  let cmd = [$XIM_BINARY, '-n', '-X', '--cmd', 'set t_u7= t_RB= t_RF= t_RV= t_RK=',
        \ '-c', 'set background=light', '-c', 'colorscheme default']
  call add(cmd, a:layout)
  let buf = RunVimInTerminal('', #{cmd: cmd, rows: 10, cols: 75, wait_for_ruler: 0})
  call WaitForAssert({-> assert_match('Xim', s:Screen(buf))})
  return buf
endfunc

func Test_xim_project_gitignore()
  call mkdir('Xximignore/src', 'p')
  call writefile(['*.log'], 'Xximignore/.gitignore')
  call writefile(['keep', ''], 'Xximignore/src/keep.txt')
  call writefile(['run', ''], 'Xximignore/src/run.log')
  call writefile(['!keep.log'], 'Xximignore/src/.gitignore')
  call writefile(['never', ''], 'Xximignore/src/keep.log')
  let buf = s:StartProjectXim('Xximignore')
  call term_sendkeys(buf, "\<C-P>")
  call WaitForAssert({-> assert_match('src/keep\.txt', s:Screen(buf))}, 5000)
  call WaitForAssert({-> assert_match('src/keep\.log', s:Screen(buf))}, 5000)
  call WaitForAssert({-> assert_notmatch('run\.log', s:Screen(buf))}, 5000)
  call term_sendkeys(buf, "\<Esc>")
  call s:StopXim(buf)
  call delete('Xximignore', 'rf')
endfunc

func Test_xim_project_symlinks()
  call mkdir('Xximsym/real', 'p')
  call writefile(['alpha', ''], 'Xximsym/real/alpha.cpp')
  " A self-link must not cause infinite recursion.
  call system('ln -s . Xximsym/loop')
  " A symlink to a directory outside the root must not escape.
  call system('mkdir Xximsymoutside')
  call system('ln -s ../Xximsymoutside Xximsym/escape')
  " A regular file symlink is still indexed.
  call system('ln -s real/alpha.cpp Xximsym/alias.cpp')
  let buf = s:StartProjectXim('Xximsym')
  call term_sendkeys(buf, "\<C-P>")
  call WaitForAssert({-> assert_match('real/alpha\.cpp', s:Screen(buf))}, 5000)
  " A file symlink is indexed under its own name; either form opens it.
  call WaitForAssert({-> assert_match('alias\.cpp', s:Screen(buf))}, 5000)
  call WaitForAssert({-> assert_notmatch('outside', s:Screen(buf))}, 5000)
  call term_sendkeys(buf, "\<Esc>")
  " Explorer never lists the loop or escape directories, nor any path
  " outside the project root.
  call term_sendkeys(buf, "\<C-E>")
  call WaitForAssert({-> assert_match('Explorer:', s:Screen(buf))})
  call assert_notmatch('loop', s:Screen(buf))
  call assert_notmatch('escape', s:Screen(buf))
  call assert_notmatch('\.\.', s:Screen(buf))
  " A file symlink opens its target under the in-tree name.
  call term_sendkeys(buf, "alias.cpp\<CR>")
  call WaitForAssert({-> assert_match('alpha', s:Screen(buf))}, 5000)
  call s:StopXim(buf)
  call delete('Xximsym', 'rf')
  call delete('Xximsymoutside', 'rf')
endfunc

func Test_xim_project_explicit_path()
  call mkdir('Xximabs', 'p')
  call writefile(['alpha', ''], 'Xximabs/inside.cpp')
  let buf = s:StartProjectXim('Xximabs')
  " Filter to the indexed entry, then Enter to open it.
  call term_sendkeys(buf, "\<C-P>inside")
  call WaitForAssert({-> assert_match('inside\.cpp', s:Screen(buf))}, 5000)
  call term_sendkeys(buf, "\<CR>")
  call WaitForAssert({-> assert_match('alpha', s:Screen(buf))}, 5000)
  call s:StopXim(buf)
  call delete('Xximabs', 'rf')
endfunc

func Test_xim_project_single_file_no_scan()
  call mkdir('Xximfile', 'p')
  call writefile(['hello', ''], 'Xximfile/hello.txt')
  let cmd = [$XIM_BINARY, '-n', '-X', '--cmd', 'set t_u7= t_RB= t_RF= t_RV= t_RK=',
        \ '-c', 'set background=light', '-c', 'colorscheme default', 'Xximfile/hello.txt']
  let buf = RunVimInTerminal('', #{cmd: cmd, rows: 10, cols: 75, wait_for_ruler: 0})
  call WaitForAssert({-> assert_match('Xim', s:Screen(buf))})
  call WaitForAssert({-> assert_match('hello', s:Screen(buf))})
  " The picker has no project to index; out-of-root absolute paths still open.
  call term_sendkeys(buf, "\<C-P>")
  call WaitForAssert({-> assert_match('Files:', s:Screen(buf))}, 5000)
  let other = fnamemodify(getcwd(), ':p') .. 'Xximfile/hello.txt'
  let escaped = substitute(other, "'", "''", 'g')
  call term_sendkeys(buf, "\<C-U>" .. escaped .. "\<CR>")
  call WaitForAssert({-> assert_match('hello', s:Screen(buf))}, 5000)
  call s:StopXim(buf)
  call delete('Xximfile', 'rf')
endfunc

func Test_xim_project_refresh_preserves_expansion()
  call mkdir('Xximrefresh/src', 'p')
  call writefile(['alpha', ''], 'Xximrefresh/src/alpha.cpp')
  call writefile(['beta', ''], 'Xximrefresh/beta.cpp')
  let buf = s:StartProjectXim('Xximrefresh')
  " Expand src/, then refresh, then check the expansion is still there.
  call term_sendkeys(buf, "\<C-E>")
  call WaitForAssert({-> assert_match('Explorer:', s:Screen(buf))})
  call WaitForAssert({-> assert_match('+ src/', s:Screen(buf))})
  call term_sendkeys(buf, "\<CR>")
  call WaitForAssert({-> assert_match('- src/', s:Screen(buf))})
  call WaitForAssert({-> assert_match('src/alpha\.cpp', s:Screen(buf))})
  call term_sendkeys(buf, "\<Esc>")
  call term_sendkeys(buf, "\<F2>Explorer\<CR>")
  call WaitForAssert({-> assert_match('Explorer:', s:Screen(buf))})
  call term_sendkeys(buf, "\<Esc>")
  call writefile(['new', ''], 'Xximrefresh/src/new.cpp')
  call term_sendkeys(buf, "\<F2>Refresh project\<CR>")
  call term_sendkeys(buf, "\<C-P>new")
  call WaitForAssert({-> assert_match('src/new\.cpp', s:Screen(buf))})
  call term_sendkeys(buf, "\<Esc>")
  call term_sendkeys(buf, "\<F2>Explorer\<CR>")
  call WaitForAssert({-> assert_match('- src/', s:Screen(buf))})
  call WaitForAssert({-> assert_match('src/alpha\.cpp', s:Screen(buf))})
  " Close the explorer overlay before handing the terminal back to StopXim.
  call term_sendkeys(buf, "\<Esc>")
  call s:StopXim(buf)
  call delete('Xximrefresh', 'rf')
endfunc

func Test_xim_project_gitignore_anchored()
  call mkdir('Xximanchor/cache', 'p')
  call mkdir('Xximanchor/src/cache', 'p')
  call writefile(['/cache/'], 'Xximanchor/.gitignore')
  call writefile(['ignored', ''], 'Xximanchor/cache/root.txt')
  call writefile(['kept', ''], 'Xximanchor/src/cache/kept.txt')
  let buf = s:StartProjectXim('Xximanchor')
  " An anchored single-component rule excludes the owner's directory
  " only; a deeper directory with the same name stays visible.
  call term_sendkeys(buf, "\<C-P>")
  call WaitForAssert({-> assert_match('src/cache/kept\.txt', s:Screen(buf))}, 5000)
  call WaitForAssert({-> assert_notmatch('root\.txt', s:Screen(buf))}, 5000)
  call term_sendkeys(buf, "\<Esc>")
  call s:StopXim(buf)
  call delete('Xximanchor', 'rf')
endfunc

func Test_xim_project_arrow_select()
  call mkdir('Xximarrow', 'p')
  call writefile(['A', ''], 'Xximarrow/a.txt')
  call writefile(['B', ''], 'Xximarrow/b.txt')
  let buf = s:StartProjectXim('Xximarrow')
  call term_sendkeys(buf, "\<C-P>")
  call WaitForAssert({-> assert_match('b\.txt', s:Screen(buf))}, 5000)
  call term_sendkeys(buf, "\<Down>\<CR>")
  call WaitForAssert({-> assert_match('B', s:Screen(buf))}, 5000)
  call s:StopXim(buf)
  call delete('Xximarrow', 'rf')
endfunc

func Test_xim_project_dirty_switch()
  call mkdir('Xximdirty', 'p')
  call writefile(['A', ''], 'Xximdirty/a.txt')
  call writefile(['B', ''], 'Xximdirty/b.txt')
  let buf = s:StartProjectXim('Xximdirty')
  call term_sendkeys(buf, "\<C-P>a\<CR>")
  call WaitForAssert({-> assert_match('A', s:Screen(buf))}, 5000)
  call term_sendkeys(buf, 'dirty')
  " Picking b.txt keeps the modified buffer without prompting.
  call term_sendkeys(buf, "\<C-P>b\<CR>")
  call WaitForAssert({-> assert_match('B', s:Screen(buf))}, 5000)
  call assert_notmatch('Save changes', s:Screen(buf))
  " The buffer picker (palette fallback) still lists the unsaved buffer.
  call term_sendkeys(buf, "\<F2>Buffers\<CR>")
  call WaitForAssert({-> assert_match('a\.txt', s:Screen(buf))}, 5000)
  call WaitForAssert({-> assert_match('b\.txt', s:Screen(buf))}, 5000)
  call term_sendkeys(buf, "\<Esc>")
  " Quit still protects the unsaved buffer.
  call term_sendkeys(buf, "\<C-Q>")
  call WaitForAssert({-> assert_match('Save changes', s:Screen(buf))}, 5000)
  call term_sendkeys(buf, "\<Esc>")
  call s:StopXim(buf)
  call delete('Xximdirty', 'rf')
endfunc

func Test_xim_project_duplicate_basenames()
  call mkdir('Xximdup/a', 'p')
  call mkdir('Xximdup/b', 'p')
  call writefile(['x', ''], 'Xximdup/a/foo.cpp')
  call writefile(['y', ''], 'Xximdup/b/foo.cpp')
  let buf = s:StartProjectXim('Xximdup')
  call term_sendkeys(buf, "\<C-P>")
  call WaitForAssert({-> assert_match('a/foo\.cpp', s:Screen(buf))}, 5000)
  call WaitForAssert({-> assert_match('b/foo\.cpp', s:Screen(buf))}, 5000)
  call term_sendkeys(buf, "\<Esc>")
  call s:StopXim(buf)
  call delete('Xximdup', 'rf')
endfunc

func Test_xim_menu_dropdown()
  let buf = s:StartXim()
  call WaitForAssert({-> assert_match('File.*Edit.*View', term_getline(buf, 1))})
  call term_sendkeys(buf, "\<F10>")
  call WaitForAssert({-> assert_match('New', s:Screen(buf))})
  call WaitForAssert({-> assert_match('Ctrl-N', s:Screen(buf))})
  call s:Dump(buf, 'Test_xim_menu_file')
  call term_sendkeys(buf, "\<Right>")
  call WaitForAssert({-> assert_match('Undo', s:Screen(buf))})
  call s:Dump(buf, 'Test_xim_menu_edit')
  call term_sendkeys(buf, "\<Esc>")
  call TermWait(buf, 50)
  call WaitForAssert({-> assert_match('alpha beta', term_getline(buf, 2))})
  call s:StopXim(buf)
endfunc

func Test_xim_menu_view_options()
  let buf = s:StartXim()
  call term_sendkeys(buf, "\<F10>\<Right>\<Right>")
  call WaitForAssert({-> assert_match('Wrap', s:Screen(buf))})
  call s:Dump(buf, 'Test_xim_menu_view')
  call term_sendkeys(buf, "\<Esc>")
  call TermWait(buf, 50)
  call s:StopXim(buf)
endfunc

func Test_xim_mouse_selection()
  let buf = s:StartXim()
  " SGR click positions the caret; drag selects alpha's tail for replacement.
  call term_sendkeys(buf, "\<Esc>[<0;7;2M\<Esc>[<0;7;2m")
  call WaitForAssert({-> assert_match('1:7', term_getline(buf, 9))})
  call term_sendkeys(buf, "\<Esc>[<0;2;2M\<Esc>[<32;7;2M\<Esc>[<0;7;2m!")
  call WaitForAssert({-> assert_match('a!beta', term_getline(buf, 2))})
  call s:Dump(buf, 'Test_xim_mouse_selection')
  call s:StopXim(buf)
endfunc
