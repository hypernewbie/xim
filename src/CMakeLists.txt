# src/CMakeLists.txt — top of the Xim source tree.
#
# Select sources and dependencies from the resolved feature configuration,
# and generate config.h from CMake's capability checks.

# ----------------------------------------------------------------------------
# Auto-generated inputs.
# ----------------------------------------------------------------------------
# Phase 3 generates them in-tree from cmake/pathdef.c.in, src/osdef.sh, and
# the vendored Wayland protocol XMLs.
set(XIM_AUTO_DIR ${CMAKE_BINARY_DIR}/auto)
file(MAKE_DIRECTORY ${XIM_AUTO_DIR})
file(MAKE_DIRECTORY ${XIM_AUTO_DIR}/wayland)

vim_generate_config_header(${XIM_AUTO_DIR} ${CMAKE_SOURCE_DIR}/src)

vim_generate_pathdef(${XIM_AUTO_DIR})
vim_generate_osdef(${XIM_AUTO_DIR} ${CMAKE_SOURCE_DIR}/src)
if(XIM_WITH_WAYLAND)
    vim_generate_wayland(${XIM_AUTO_DIR}/wayland ${CMAKE_SOURCE_DIR}/src/auto/wayland)
endif()

# ----------------------------------------------------------------------------
# Source lists.
# ----------------------------------------------------------------------------
# The Makefile groups sources by directory but compiles them per-file into
# objects/<name>.o. CMake handles path naming, so the only grouping we need
# is to apply per-source compile flags to libvterm and xdiff.

# Main vim sources — alphabetical. The Makefile order in BASIC_SRC is
# essentially alphabetical, with version.c kept near the end. We keep that
# convention here for diff-ability with the Makefile source lists.
#
# Note: `main.c`, `charset.c`, `json.c`, `memfile.c`, and `message.c` are
# excluded from this list. They form OBJ_MAIN in the Makefile. `main.c`
# is added to the `vim` executable only; the other four are added to
# vim_core so the unit tests can reuse them.
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
    # Provides the cscope function stubs when FEAT_CSCOPE is disabled.
    if_cscope.c
    if_xcmdsrv.c
    image.c
    image_kitty.c
    image_sixel.c
    indent.c
    insexpand.c
    linematch.c
    list.c
    locale.c
    logfile.c
    map.c
    mark.c
    match.c
    mbyte.c
    memline.c
    menu.c
    misc1.c
    misc2.c
    mouse.c
    move.c
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
    window.c)

if(XIM_WITH_CHANNEL)
    list(APPEND VIM_BASIC_SRC channel.c job.c)
endif()
if(XIM_WITH_NETBEANS)
    list(APPEND VIM_BASIC_SRC netbeans.c)
endif()
if(XIM_WITH_TERMINAL)
    list(APPEND VIM_BASIC_SRC terminal.c)
endif()
if(XIM_WITH_WAYLAND)
    list(APPEND VIM_BASIC_SRC wayland.c)
endif()

# OBJ_MAIN — the four .c files (plus main.c) that are added to each
# executable that needs them. They are NOT in vim_core: each unit test
# driver redefines some of the functions in its namesake file, so adding
# both would cause duplicate symbol errors at link time.
#
# The Makefile defines these in OBJ_MAIN (lines 1833-1838 of src/Makefile)
# and includes 4 of the 5 in each test's object list, omitting the .o whose
# name matches the test driver.
set(VIM_OBJ_MAIN_SRC
    charset.c
    json.c
    memfile.c
    message.c)

# xdiff — vendored under src/xdiff/. Compiled into vim directly, no library.
# Diff is implied by normal and huge feature levels; tiny omits xdiff.
set(VIM_XDIFF_SRC)
if(NOT XIM_FEATURE_LEVEL STREQUAL "tiny")
    list(APPEND VIM_XDIFF_SRC
        xdiff/xdiffi.c
        xdiff/xemit.c
        xdiff/xhistogram.c
        xdiff/xpatience.c
        xdiff/xprepare.c
        xdiff/xutils.c)
endif()

# libvterm — vendored under src/libvterm/. Compiled into vim directly.
# The library's Makefile is not used by the main vim build.
set(VIM_LIBVTERM_SRC)
if(XIM_WITH_TERMINAL)
    list(APPEND VIM_LIBVTERM_SRC
        libvterm/src/encoding.c
        libvterm/src/keyboard.c
        libvterm/src/mouse.c
        libvterm/src/parser.c
        libvterm/src/pen.c
        libvterm/src/screen.c
        libvterm/src/state.c
        libvterm/src/unicode.c
        libvterm/src/vterm.c)
endif()

# Wayland protocol stubs — generated from XML by wayland-scanner in Phase 3.
set(VIM_WAYLAND_GEN_SRC)
if(XIM_WITH_WAYLAND)
    list(APPEND VIM_WAYLAND_GEN_SRC
        ${XIM_AUTO_DIR}/wayland/ext-data-control-v1.c
        ${XIM_AUTO_DIR}/wayland/wlr-data-control-unstable-v1.c)
endif()

set(VIM_FEATURE_SRC)
set(VIM_FEATURE_INCLUDE_DIRS)
set(XIM_SYSTEM_LIBRARIES m nsl)
if(NOT XIM_TERMINFO_LIBRARY)
    message(FATAL_ERROR "A terminfo library (tinfo or ncurses) is required.")
endif()
list(APPEND XIM_SYSTEM_LIBRARIES ${XIM_TERMINFO_LIBRARY})
if(XIM_HAVE_SELINUX)
    list(APPEND XIM_SYSTEM_LIBRARIES ${XIM_SELINUX_LIBRARY})
    list(APPEND VIM_FEATURE_INCLUDE_DIRS ${XIM_SELINUX_INCLUDE_DIR})
endif()
if(XIM_WITH_X11)
    list(APPEND XIM_SYSTEM_LIBRARIES ${XIM_X11_LIBRARIES})
    list(APPEND VIM_FEATURE_INCLUDE_DIRS ${XIM_X11_INCLUDE_DIRS})
endif()
if(XIM_WITH_WAYLAND)
    list(APPEND XIM_SYSTEM_LIBRARIES PkgConfig::XIM_WAYLAND)
endif()
if(XIM_WITH_PIXMAN)
    list(APPEND XIM_SYSTEM_LIBRARIES PkgConfig::XIM_PIXMAN)
endif()

if(XIM_GUI_BACKEND STREQUAL "gtk2" OR XIM_GUI_BACKEND STREQUAL "gtk3")
    list(APPEND VIM_FEATURE_SRC
        gui.c gui_gtk.c gui_gtk_x11.c gui_gtk_f.c gui_beval.c)
    if(XIM_GUI_BACKEND STREQUAL "gtk2")
        list(APPEND XIM_SYSTEM_LIBRARIES PkgConfig::XIM_GTK2)
    else()
        list(APPEND XIM_SYSTEM_LIBRARIES PkgConfig::XIM_GTK3)
    endif()
elseif(XIM_GUI_BACKEND STREQUAL "gtk4")
    list(APPEND VIM_FEATURE_SRC
        gui.c gui_gtk4.c gui_gtk4_f.c gui_gtk4_cb.c gui_gtk4_da.c
        gui_beval.c gui_gtk4_tb.c gui_gtk4_menu.c)
    list(APPEND XIM_SYSTEM_LIBRARIES PkgConfig::XIM_GTK4)
elseif(XIM_GUI_BACKEND STREQUAL "motif")
    list(APPEND VIM_FEATURE_SRC
        gui.c gui_motif.c gui_x11.c gui_beval.c gui_xmdlg.c gui_xmebw.c)
    list(APPEND VIM_FEATURE_INCLUDE_DIRS ${XIM_MOTIF_INCLUDE_DIR})
    list(APPEND XIM_SYSTEM_LIBRARIES ${XIM_MOTIF_LIBRARY})
endif()

if(XIM_WITH_PYTHON)
    list(APPEND VIM_FEATURE_SRC if_python.c)
    list(APPEND XIM_SYSTEM_LIBRARIES Python2::Python)
    list(APPEND VIM_FEATURE_INCLUDE_DIRS ${Python2_INCLUDE_DIRS})
endif()
if(XIM_WITH_PYTHON3)
    list(APPEND VIM_FEATURE_SRC if_python3.c)
    list(APPEND XIM_SYSTEM_LIBRARIES Python3::Python)
    list(APPEND VIM_FEATURE_INCLUDE_DIRS ${Python3_INCLUDE_DIRS})
endif()
if(XIM_WITH_LUA)
    list(APPEND VIM_FEATURE_SRC if_lua.c)
    list(APPEND XIM_SYSTEM_LIBRARIES ${XIM_LUA_PKG_TARGET})
endif()
if(XIM_WITH_RUBY)
    list(APPEND VIM_FEATURE_SRC if_ruby.c)
    list(APPEND VIM_FEATURE_INCLUDE_DIRS ${Ruby_INCLUDE_DIRS})
    list(APPEND XIM_SYSTEM_LIBRARIES ${Ruby_LIBRARIES})
endif()
if(XIM_WITH_TCL)
    list(APPEND VIM_FEATURE_SRC if_tcl.c)
    list(APPEND VIM_FEATURE_INCLUDE_DIRS ${TCL_INCLUDE_PATH})
    list(APPEND XIM_SYSTEM_LIBRARIES ${TCL_LIBRARY})
endif()
if(XIM_WITH_PERL)
    add_custom_command(
        OUTPUT ${XIM_AUTO_DIR}/if_perl.c
        COMMAND ${PERL_EXECUTABLE} ${XIM_PERL_XSUBPP}
            -prototypes -typemap ${XIM_PERL_TYPEMAP}
            ${CMAKE_SOURCE_DIR}/src/if_perl.xs
            -output ${XIM_AUTO_DIR}/if_perl.c
        DEPENDS
            ${CMAKE_SOURCE_DIR}/src/if_perl.xs
            ${XIM_PERL_XSUBPP}
            ${XIM_PERL_TYPEMAP}
        COMMENT "Generating embedded Perl interface"
        VERBATIM)
    list(APPEND VIM_FEATURE_SRC ${XIM_AUTO_DIR}/if_perl.c)
    list(APPEND VIM_FEATURE_INCLUDE_DIRS ${PERL_INCLUDE_PATH})
    list(APPEND XIM_SYSTEM_LIBRARIES ${XIM_PERL_LINK_LIBRARIES})
    set_source_files_properties(${XIM_AUTO_DIR}/if_perl.c
        PROPERTIES COMPILE_OPTIONS "${XIM_PERL_COMPILE_OPTIONS}")
endif()
# Auto-generated pathdef.c — defines default_vim_dir, default_vimruntime_dir,
# compiled_user/sys, and the link command string baked into the binary.
set(VIM_AUTO_SRC ${XIM_AUTO_DIR}/pathdef.c)

# ----------------------------------------------------------------------------
# Shared per-source compile options.
# ----------------------------------------------------------------------------
# libvterm needs a few macros redefined so its internal names don't collide
# with the global ones vim provides. The Makefile does this in CCCTERM at
# lines 3779-3783.
if(VIM_LIBVTERM_SRC)
    set_source_files_properties(${VIM_LIBVTERM_SRC}
        PROPERTIES
            COMPILE_OPTIONS
                "-DINLINE=;-DVSNPRINTF=vim_vsnprintf;-DSNPRINTF=vim_snprintf;-DIS_COMBINING_FUNCTION=utf_iscomposing_uint;-DWCWIDTH_FUNCTION=utf_uint2cells")
endif()

# xdiff follows the same pattern with -DXINCLUDE="" (Makefile line 3812).
if(VIM_XDIFF_SRC)
    set_source_files_properties(${VIM_XDIFF_SRC}
        PROPERTIES
            COMPILE_OPTIONS
                "-DXINCLUDE=")
endif()

# ----------------------------------------------------------------------------
# vim_core OBJECT library — every C object file that participates in either
# the main `vim` executable or one of the unit tests. Linking both against
# this avoids recompiling the bulk of the source per unit test.
# ----------------------------------------------------------------------------
add_library(vim_core OBJECT
    ${VIM_BASIC_SRC}
    ${VIM_FEATURE_SRC}
    ${VIM_XDIFF_SRC}
    ${VIM_LIBVTERM_SRC}
    ${VIM_WAYLAND_GEN_SRC}
    ${VIM_AUTO_SRC})

target_include_directories(vim_core PUBLIC
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/proto
    ${XIM_AUTO_DIR}
    ${XIM_AUTO_DIR}/wayland
    ${VIM_FEATURE_INCLUDE_DIRS})

if(XIM_WITH_TERMINAL)
    target_include_directories(vim_core PUBLIC ${CMAKE_SOURCE_DIR}/src/libvterm/include)
endif()

target_compile_definitions(vim_core PUBLIC
    HAVE_CONFIG_H
    XIM_CMAKE_CONFIG)

target_link_libraries(vim_core PUBLIC ${XIM_SYSTEM_LIBRARIES})

# osdef.h is generated but not a compile input — it provides system header
# declarations the rest of vim #includes via vim.h. Force its generation
# before vim_core compiles.
add_dependencies(vim_core generate_xim_osdef)
if(XIM_WITH_WAYLAND)
    add_dependencies(vim_core generate_xim_wayland)
endif()

# ----------------------------------------------------------------------------
# Main `vim` executable.
# ----------------------------------------------------------------------------
add_executable(vim main.c ${VIM_OBJ_MAIN_SRC})
target_link_libraries(vim PRIVATE vim_core)
xim_apply_default_flags(vim)

include(GNUInstallDirs)
install(TARGETS vim RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# ----------------------------------------------------------------------------
# C unit tests — small executables that link vim_core plus a driver .c
# and 3 of the 4 OBJ_MAIN files (omitting the one whose name matches the
# driver, since the driver redefines some of those functions).
#
# Makefile reference (src/Makefile lines 1842-1873):
#   json_test:    charset.o, memfile.o, message.o + json_test.o
#   kword_test:   json.o,    memfile.o, message.o + kword_test.o
#   memfile_test: charset.o, json.o,    message.o + memfile_test.o
#   message_test: charset.o, json.o,    memfile.o + message_test.o
# ----------------------------------------------------------------------------

add_executable(json_test json_test.c charset.c memfile.c message.c)
target_link_libraries(json_test PRIVATE vim_core)
add_test(NAME vim_unit_json COMMAND json_test)
set_tests_properties(vim_unit_json PROPERTIES LABELS "vim_unit")

add_executable(kword_test kword_test.c json.c memfile.c message.c)
target_link_libraries(kword_test PRIVATE vim_core)
add_test(NAME vim_unit_kword COMMAND kword_test)
set_tests_properties(vim_unit_kword PROPERTIES LABELS "vim_unit")

add_executable(memfile_test memfile_test.c charset.c json.c message.c)
target_link_libraries(memfile_test PRIVATE vim_core)
add_test(NAME vim_unit_memfile COMMAND memfile_test)
set_tests_properties(vim_unit_memfile PROPERTIES LABELS "vim_unit")

add_executable(message_test message_test.c charset.c json.c memfile.c)
target_link_libraries(message_test PRIVATE vim_core)
add_test(NAME vim_unit_message COMMAND message_test)
set_tests_properties(vim_unit_message PROPERTIES LABELS "vim_unit")

add_subdirectory(xxd)

option(BUILD_FULL_TEST "Run the full src/testdir/ script suite (slow)" OFF)
if(BUILD_FULL_TEST)
    add_subdirectory(testdir)
endif()
