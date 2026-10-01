# src/CMakeLists.txt — top of the Xim source tree.
#
# Phase 2 (this file): the main `vim` target. Mirrors the reference build
# at src/Makefile: lines 1504-1838, with FEAT_HUGE hardcoded and the
# auto-generated artifacts read as pre-existing inputs that we copy into
# the build tree at configure time.
#
# Phase 3 will replace the configure_file(... COPYONLY) calls with
# add_custom_command() rules so a clean checkout still builds.

# ----------------------------------------------------------------------------
# Auto-generated inputs.
# ----------------------------------------------------------------------------
# Phase 2 copied these from src/auto/. Phase 3 generates them in-tree from
# their inputs: cmake/pathdef.c.in, src/osdef.sh + osdef1.h.in + osdef2.h.in,
# and the vendored Wayland protocol XMLs.
set(XIM_AUTO_DIR ${CMAKE_BINARY_DIR}/auto)
file(MAKE_DIRECTORY ${XIM_AUTO_DIR})
file(MAKE_DIRECTORY ${XIM_AUTO_DIR}/wayland)

# config.h is committed in the source tree in Phase 2 (a side-effect of
# the reference build's `make config` step having run before). Phase 6
# will replace this with a CMake-generated config.h.
configure_file(
    ${CMAKE_SOURCE_DIR}/src/auto/config.h
    ${XIM_AUTO_DIR}/config.h
    COPYONLY)

vim_generate_pathdef(${XIM_AUTO_DIR})
vim_generate_osdef(${XIM_AUTO_DIR} ${CMAKE_SOURCE_DIR}/src)
vim_generate_wayland(${XIM_AUTO_DIR}/wayland ${CMAKE_SOURCE_DIR}/src/auto/wayland)

# ----------------------------------------------------------------------------
# Source lists.
# ----------------------------------------------------------------------------
# The Makefile groups sources by directory but compiles them per-file into
# objects/<name>.o. CMake handles path naming, so the only grouping we need
# is to apply per-source compile flags to libvterm and xdiff.

# Main vim sources — alphabetical. The Makefile order in BASIC_SRC is
# essentially alphabetical, with version.c kept near the end. We keep that
# convention here for diff-ability with the Makefile source lists.
set(VIM_BASIC_SRC
    alloc.c
    arabic.c
    arglist.c
    autocmd.c
    beval.c
    blob.c
    blowfish.c
    buffer.c
    bufwrite.c
    change.c
    channel.c
    charset.c
    cindent.c
    clientserver.c
    clipboard.c
    cmdexpand.c
    cmdhist.c
    crypt.c
    crypt_zip.c
    debugger.c
    dict.c
    diff.c
    digraph.c
    drawline.c
    drawscreen.c
    edit.c
    eval.c
    evalbuffer.c
    evalfunc.c
    evalvars.c
    evalwindow.c
    ex_cmds.c
    ex_cmds2.c
    ex_docmd.c
    ex_eval.c
    ex_getln.c
    fileio.c
    filepath.c
    findfile.c
    float.c
    fold.c
    fuzzy.c
    gc.c
    getchar.c
    gui_xim.c
    hardcopy.c
    hardcopy_pango.c
    hardcopy_postscript.c
    hashtab.c
    help.c
    highlight.c
    if_cscope.c
    if_xcmdsrv.c
    image.c
    image_kitty.c
    image_sixel.c
    indent.c
    insexpand.c
    job.c
    json.c
    linematch.c
    list.c
    locale.c
    logfile.c
    main.c
    map.c
    mark.c
    match.c
    mbyte.c
    memfile.c
    memline.c
    menu.c
    message.c
    misc1.c
    misc2.c
    mouse.c
    move.c
    netbeans.c
    normal.c
    ops.c
    option.c
    optionstr.c
    os_unix.c
    popupmenu.c
    popupwin.c
    profiler.c
    pty.c
    quickfix.c
    regexp.c
    register.c
    screen.c
    scriptfile.c
    search.c
    session.c
    sha256.c
    sign.c
    socketserver.c
    sound.c
    spell.c
    spellfile.c
    spellsuggest.c
    strings.c
    syntax.c
    tabpanel.c
    tag.c
    term.c
    terminal.c
    testing.c
    textformat.c
    textobject.c
    textprop.c
    time.c
    tuple.c
    typval.c
    ui.c
    undo.c
    usercmd.c
    userfunc.c
    version.c
    vim9class.c
    vim9cmds.c
    vim9compile.c
    vim9execute.c
    vim9expr.c
    vim9generics.c
    vim9instr.c
    vim9script.c
    vim9type.c
    viminfo.c
    wayland.c
    window.c)

# xdiff — vendored under src/xdiff/. Compiled into vim directly, no library.
# Phase 2 hardcodes FEAT_XDIFF; Phase 5 will gate it.
set(VIM_XDIFF_SRC
    xdiff/xdiffi.c
    xdiff/xemit.c
    xdiff/xhistogram.c
    xdiff/xpatience.c
    xdiff/xprepare.c
    xdiff/xutils.c)

# libvterm — vendored under src/libvterm/. Compiled into vim directly.
# The library's Makefile is not used by the main vim build.
set(VIM_LIBVTERM_SRC
    libvterm/src/encoding.c
    libvterm/src/keyboard.c
    libvterm/src/mouse.c
    libvterm/src/parser.c
    libvterm/src/pen.c
    libvterm/src/screen.c
    libvterm/src/state.c
    libvterm/src/unicode.c
    libvterm/src/vterm.c)

# Wayland protocol stubs — generated from XML by wayland-scanner in Phase 3.
# For now, copied as-is from src/auto/wayland/.
set(VIM_WAYLAND_GEN_SRC
    ${XIM_AUTO_DIR}/wayland/ext-data-control-v1.c
    ${XIM_AUTO_DIR}/wayland/wlr-data-control-unstable-v1.c)

# Auto-generated pathdef.c — defines default_vim_dir, default_vimruntime_dir,
# compiled_user/sys, and the link command string baked into the binary.
set(VIM_AUTO_SRC ${XIM_AUTO_DIR}/pathdef.c)

# ----------------------------------------------------------------------------
# Build the vim executable.
# ----------------------------------------------------------------------------
add_executable(vim
    ${VIM_BASIC_SRC}
    ${VIM_XDIFF_SRC}
    ${VIM_LIBVTERM_SRC}
    ${VIM_WAYLAND_GEN_SRC}
    ${VIM_AUTO_SRC})

# osdef.h is generated but not a compile input — it provides system header
# declarations the rest of vim #includes via vim.h. Force its generation
# before vim compiles.
add_dependencies(vim generate_xim_osdef generate_xim_wayland)

# libvterm needs a few macros redefined so its internal names don't collide
# with the global ones vim provides. The Makefile does this in CCCTERM at
# lines 3779-3783.
set_source_files_properties(${VIM_LIBVTERM_SRC}
    PROPERTIES
        COMPILE_OPTIONS
            "-DINLINE=;-DVSNPRINTF=vim_vsnprintf;-DSNPRINTF=vim_snprintf;-DIS_COMBINING_FUNCTION=utf_iscomposing_uint;-DWCWIDTH_FUNCTION=utf_uint2cells")

# xdiff follows the same pattern with -DXINCLUDE="" (Makefile line 3812).
set_source_files_properties(${VIM_XDIFF_SRC}
    PROPERTIES
        COMPILE_OPTIONS
            "-DXINCLUDE=")

target_include_directories(vim PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/proto
    ${XIM_AUTO_DIR}
    ${XIM_AUTO_DIR}/wayland
    ${CMAKE_SOURCE_DIR}/src/libvterm/include
    /usr/include/pixman-1)

target_compile_definitions(vim PRIVATE
    HAVE_CONFIG_H
    FEAT_HUGE)

# Reference link line was:
#   clang -L/usr/local/lib -Wl,--as-needed -o vim <objects>
#     -lSM -lICE -lXpm -lXt -lX11 -lwayland-client -lpixman-1 -lXdmcp
#     -lSM -lICE -lm -ltinfo -lnsl -lselinux
target_link_libraries(vim PRIVATE
    SM
    ICE
    Xpm
    Xt
    X11
    wayland-client
    pixman-1
    Xdmcp
    m
    tinfo
    nsl
    selinux)

xim_apply_default_flags(vim)

# Install alongside the binary. The full runtime install (runtime/ tree,
# man pages, lang) comes in a later phase — Phase 6 installs vim here as a
# sanity check.
include(GNUInstallDirs)
install(TARGETS vim RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

add_subdirectory(xxd)