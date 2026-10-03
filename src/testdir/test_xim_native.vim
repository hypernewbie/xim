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
  call term_sendkeys(a:buf, "\<C-P>Ex command\<CR>")
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
  call term_sendkeys(buf, "\<C-P>Save")
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
    call term_sendkeys(buf, "\<C-P>Save")
    call WaitForAssert({-> assert_match('Command: Save', term_getline(buf, 10))})
    call s:Dump(buf, 'Test_xim_theme_' .. theme)
    call term_sendkeys(buf, "\<Esc>")
    call TermWait(buf, 50)
    call s:StopXim(buf)
  endfor
endfunc

func Test_xim_palette_scroll()
  let buf = s:StartXim()
  call term_sendkeys(buf, "\<C-P>" .. repeat("\<Down>", 14))
  call WaitForAssert({-> assert_match('> Ex command', term_getline(buf, 8))}, 1000)
  call s:Dump(buf, 'Test_xim_palette_scroll')
  call term_sendkeys(buf, "\<Esc>")
  call TermWait(buf, 50)
  call WaitForAssert({-> assert_match('alpha beta', term_getline(buf, 1))})
  call s:StopXim(buf)
endfunc

func Test_xim_failed_unnamed_save()
  let buf = s:StartBlankXim()
  call term_sendkeys(buf, "unsaved")
  call WaitForAssert({-> assert_match('unsaved', term_getline(buf, 1))})
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
  call WaitForAssert({-> assert_match('Zalpha beta', term_getline(buf, 1))})
  call s:StopXim(buf)
endfunc
