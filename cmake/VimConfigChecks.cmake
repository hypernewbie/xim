# VimConfigChecks.cmake — discover optional system capabilities used by Vim.
#
# Keep checks independent of the selected feature set. VimFeatures.cmake applies
# the user's AUTO/ON/OFF choices and rejects explicitly requested capabilities
# whose headers or libraries are unavailable.

include(CheckCSourceCompiles)
include(CheckCSourceRuns)
include(CheckFunctionExists)
include(CheckIncludeFile)
include(CheckStructHasMember)
include(CheckSymbolExists)
include(CheckTypeSize)
include(TestBigEndian)

find_package(PkgConfig QUIET)
find_package(X11 QUIET)

set(XIM_HAVE_X11 ${X11_FOUND})
if(XIM_HAVE_X11)
    set(XIM_X11_INCLUDE_DIRS ${X11_INCLUDE_DIR})
    foreach(_xim_xlib IN ITEMS SM ICE Xpm Xt X11 Xdmcp)
        string(TOUPPER "${_xim_xlib}" _xim_xlib_upper)
        find_library(XIM_${_xim_xlib_upper}_LIBRARY NAMES ${_xim_xlib})
        if(XIM_${_xim_xlib_upper}_LIBRARY)
            list(APPEND XIM_X11_LIBRARIES ${XIM_${_xim_xlib_upper}_LIBRARY})
        endif()
    endforeach()
    if(NOT XIM_X11_LIBRARY OR NOT XIM_XT_LIBRARY)
        set(XIM_HAVE_X11 FALSE)
        set(XIM_X11_LIBRARIES)
        set(XIM_X11_INCLUDE_DIRS)
    endif()
endif()

if(PkgConfig_FOUND)
    pkg_check_modules(XIM_WAYLAND QUIET IMPORTED_TARGET wayland-client)
    pkg_check_modules(XIM_PIXMAN QUIET IMPORTED_TARGET pixman-1)
    pkg_check_modules(XIM_GTK3 QUIET IMPORTED_TARGET gtk+-3.0)
    pkg_check_modules(XIM_GTK4 QUIET IMPORTED_TARGET gtk4>=4.10)
    pkg_check_modules(XIM_GTK2 QUIET IMPORTED_TARGET gtk+-2.0>=2.2)

    foreach(_xim_lua_package IN ITEMS lua5.4 lua5.3 lua5.2 lua5.1 luajit)
        string(MAKE_C_IDENTIFIER "${_xim_lua_package}" _xim_lua_id)
        set(_xim_lua_prefix "XIM_LUA_${_xim_lua_id}")
        pkg_check_modules(${_xim_lua_prefix} QUIET IMPORTED_TARGET ${_xim_lua_package})
        if(${_xim_lua_prefix}_FOUND AND NOT XIM_LUA_PKG_TARGET)
            set(XIM_LUA_PKG_TARGET "PkgConfig::${_xim_lua_prefix}")
            set(XIM_HAVE_LUA TRUE)
        endif()
    endforeach()
endif()

find_program(XIM_WAYLAND_SCANNER wayland-scanner)
set(XIM_HAVE_WAYLAND FALSE)
if(XIM_WAYLAND_FOUND AND XIM_WAYLAND_SCANNER)
    set(XIM_HAVE_WAYLAND TRUE)
endif()
set(XIM_HAVE_PIXMAN ${XIM_PIXMAN_FOUND})

find_package(Python2 QUIET COMPONENTS Development.Embed)
find_package(Python3 QUIET COMPONENTS Development.Embed)
set(XIM_HAVE_PYTHON ${Python2_Development.Embed_FOUND})
set(XIM_HAVE_PYTHON3 ${Python3_Development.Embed_FOUND})

find_package(Perl QUIET)
find_package(PerlLibs QUIET)
set(XIM_HAVE_PERL FALSE)
if(PERL_FOUND AND PERLLIBS_FOUND)
    execute_process(
        COMMAND ${PERL_EXECUTABLE} -MExtUtils::Embed -e ccopts
        RESULT_VARIABLE _xim_perl_ccopts_status
        OUTPUT_VARIABLE XIM_PERL_CFLAGS
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    execute_process(
        COMMAND ${PERL_EXECUTABLE} -MExtUtils::Embed -e ldopts
        RESULT_VARIABLE _xim_perl_ldopts_status
        OUTPUT_VARIABLE XIM_PERL_LDFLAGS
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    execute_process(
        COMMAND ${PERL_EXECUTABLE} -MConfig -e [=[print "$Config{privlib}/ExtUtils/xsubpp"]]=]
        RESULT_VARIABLE _xim_perl_xsubpp_status
        OUTPUT_VARIABLE XIM_PERL_XSUBPP
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    execute_process(
        COMMAND ${PERL_EXECUTABLE} -MConfig -e [=[print "$Config{privlib}/ExtUtils/typemap"]]=]
        RESULT_VARIABLE _xim_perl_typemap_status
        OUTPUT_VARIABLE XIM_PERL_TYPEMAP
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    if(_xim_perl_ccopts_status EQUAL 0
        AND _xim_perl_ldopts_status EQUAL 0
        AND _xim_perl_xsubpp_status EQUAL 0
        AND _xim_perl_typemap_status EQUAL 0
        AND EXISTS "${XIM_PERL_XSUBPP}"
        AND EXISTS "${XIM_PERL_TYPEMAP}")
        separate_arguments(XIM_PERL_COMPILE_OPTIONS UNIX_COMMAND "${XIM_PERL_CFLAGS}")
        separate_arguments(XIM_PERL_LINK_LIBRARIES UNIX_COMMAND "${XIM_PERL_LDFLAGS}")
        set(XIM_HAVE_PERL TRUE)
    endif()
endif()

find_package(Ruby QUIET)
set(XIM_HAVE_RUBY FALSE)
if(Ruby_FOUND OR RUBY_FOUND)
    set(XIM_HAVE_RUBY TRUE)
endif()

find_package(TCL QUIET)
set(XIM_HAVE_TCL ${TCL_FOUND})

find_path(XIM_MOTIF_INCLUDE_DIR Xm/Xm.h)
find_library(XIM_MOTIF_LIBRARY NAMES Xm)
set(XIM_HAVE_MOTIF FALSE)
if(XIM_MOTIF_INCLUDE_DIR AND XIM_MOTIF_LIBRARY AND XIM_HAVE_X11)
    set(XIM_HAVE_MOTIF TRUE)
endif()

# Network code is optional on systems without the POSIX socket interfaces.
check_c_source_compiles([=[
    #include <sys/types.h>
    #include <sys/socket.h>
    #include <netdb.h>
    int main(void)
    {
        struct addrinfo hints = {0};
        struct addrinfo *result = 0;
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        (void)getaddrinfo("localhost", 0, &hints, &result);
        freeaddrinfo(result);
        return fd < -1;
    }
]=] XIM_HAVE_POSIX_CHANNELS)

check_c_source_compiles([=[
    #include <sys/types.h>
    #include <sys/socket.h>
    #include <netdb.h>
    #include <netinet/in.h>
    int main(void)
    {
        struct sockaddr_in6 address = {0};
        struct addrinfo hints = {0};
        struct addrinfo *result = 0;
        hints.ai_family = AF_INET6;
        (void)address;
        (void)getaddrinfo("localhost", 0, &hints, &result);
        freeaddrinfo(result);
        return 0;
    }
]=] XIM_HAVE_IPV6)

# Configure the platform macros from src/config.h.in. These are separated
# from VimFeatures.cmake because they describe host/compiler capabilities,
# not user-selected editor features.
set(_xim_config_headers
    "HAVE_AVAILABILITYMACROS_H|AvailabilityMacros.h"
    "HAVE_DISPATCH_DISPATCH_H|dispatch/dispatch.h"
    "HAVE_DIRENT_H|dirent.h"
    "HAVE_ERRNO_H|errno.h"
    "HAVE_FCNTL_H|fcntl.h"
    "HAVE_ICONV_H|iconv.h"
    "HAVE_INTTYPES_H|inttypes.h"
    "HAVE_LANGINFO_H|langinfo.h"
    "HAVE_LIBC_H|libc.h"
    "HAVE_LIBGEN_H|libgen.h"
    "HAVE_LIBINTL_H|libintl.h"
    "HAVE_LOCALE_H|locale.h"
    "HAVE_MATH_H|math.h"
    "HAVE_POLL_H|poll.h"
    "HAVE_PTHREAD_NP_H|pthread_np.h"
    "HAVE_PWD_H|pwd.h"
    "HAVE_SETJMP_H|setjmp.h"
    "HAVE_SGTTY_H|sgtty.h"
    "HAVE_STDINT_H|stdint.h"
    "HAVE_STRINGS_H|strings.h"
    "HAVE_STROPTS_H|stropts.h"
    "HAVE_SYS_ACCESS_H|sys/access.h"
    "HAVE_SYS_ACL_H|sys/acl.h"
    "HAVE_SYS_IOCTL_H|sys/ioctl.h"
    "HAVE_SYS_PARAM_H|sys/param.h"
    "HAVE_SYS_POLL_H|sys/poll.h"
    "HAVE_SYS_PTEM_H|sys/ptem.h"
    "HAVE_SYS_PTMS_H|sys/ptms.h"
    "HAVE_SYS_RESOURCE_H|sys/resource.h"
    "HAVE_SYS_SELECT_H|sys/select.h"
    "HAVE_SYS_STATFS_H|sys/statfs.h"
    "HAVE_SYS_STREAM_H|sys/stream.h"
    "HAVE_SYS_SYSCTL_H|sys/sysctl.h"
    "HAVE_SYS_SYSINFO_H|sys/sysinfo.h"
    "HAVE_SYS_SYSTEMINFO_H|sys/systeminfo.h"
    "HAVE_SYS_TIME_H|sys/time.h"
    "HAVE_SYS_TYPES_H|sys/types.h"
    "HAVE_SYS_UTSNAME_H|sys/utsname.h"
    "HAVE_TERMCAP_H|termcap.h"
    "HAVE_TERMIOS_H|termios.h"
    "HAVE_TERMIO_H|termio.h"
    "HAVE_WCHAR_H|wchar.h"
    "HAVE_WCTYPE_H|wctype.h"
    "HAVE_UNISTD_H|unistd.h"
    "HAVE_UTIL_DEBUG_H|util/debug.h"
    "HAVE_UTIL_MSGI18N_H|util/msgi18n.h"
    "HAVE_UTIME_H|utime.h"
    "HAVE_X11_SUNKEYSYM_H|X11/Sunkeysym.h"
    "HAVE_X11_XPM_H|X11/xpm.h"
    "HAVE_X11_SM_SMLIB_H|X11/SM/SMlib.h"
    "HAVE_XM_XM_H|Xm/Xm.h"
    "HAVE_XM_XPMP_H|Xm/XpmP.h"
    "HAVE_XM_TRAITP_H|Xm/TraitP.h"
    "HAVE_XM_MANAGER_H|Xm/Manager.h"
    "HAVE_XM_UNHIGHLIGHTT_H|Xm/UnhighlightT.h"
    "HAVE_XM_JOINSIDET_H|Xm/JoinSideT.h"
    "HAVE_XM_NOTEBOOK_H|Xm/Notebook.h"
    "HAVE_SYS_WAIT_H|sys/wait.h"
    "HAVE_STDLIB_H|stdlib.h"
    "HAVE_STRING_H|string.h"
    "HAVE_DLFCN_H|dlfcn.h"
    "HAVE_SYS_XATTR_H|sys/xattr.h")

set(_xim_saved_required_includes ${CMAKE_REQUIRED_INCLUDES})
list(APPEND CMAKE_REQUIRED_INCLUDES ${X11_INCLUDE_DIR})
foreach(_xim_header IN LISTS _xim_config_headers)
    string(REPLACE "|" ";" _xim_header_parts "${_xim_header}")
    list(GET _xim_header_parts 0 _xim_header_macro)
    list(GET _xim_header_parts 1 _xim_header_name)
    check_include_file("${_xim_header_name}" ${_xim_header_macro})
endforeach()
set(CMAKE_REQUIRED_INCLUDES ${_xim_saved_required_includes})

# Autoconf selects the first header that defines DIR, rather than enabling all
# four historical header names independently.
set(HAVE_DIRENT_H FALSE)
set(HAVE_SYS_NDIR_H FALSE)
set(HAVE_SYS_DIR_H FALSE)
set(HAVE_NDIR_H FALSE)
foreach(_xim_dirent_header IN ITEMS
        "dirent.h|HAVE_DIRENT_H"
        "sys/ndir.h|HAVE_SYS_NDIR_H"
        "sys/dir.h|HAVE_SYS_DIR_H"
        "ndir.h|HAVE_NDIR_H")
    string(REPLACE "|" ";" _xim_dirent_parts "${_xim_dirent_header}")
    list(GET _xim_dirent_parts 0 _xim_dirent_file)
    list(GET _xim_dirent_parts 1 _xim_dirent_macro)
    string(MAKE_C_IDENTIFIER "${_xim_dirent_file}" _xim_dirent_id)
    check_c_source_compiles("#include <sys/types.h>\n#include <${_xim_dirent_file}>\nint main(void) { DIR *d = 0; return d != 0; }"
        XIM_DIR_HEADER_${_xim_dirent_id})
    if(XIM_DIR_HEADER_${_xim_dirent_id})
        set(${_xim_dirent_macro} TRUE)
        break()
    endif()
endforeach()

set(_xim_saved_required_libraries ${CMAKE_REQUIRED_LIBRARIES})
find_library(XIM_TERMINFO_LIBRARY NAMES tinfo ncursesw ncurses)
list(APPEND CMAKE_REQUIRED_LIBRARIES m)
list(APPEND CMAKE_REQUIRED_LIBRARIES ${XIM_X11_LIBRARIES})
if(XIM_TERMINFO_LIBRARY)
    list(APPEND CMAKE_REQUIRED_LIBRARIES ${XIM_TERMINFO_LIBRARY})
endif()
set(_xim_saved_required_includes ${CMAKE_REQUIRED_INCLUDES})
list(APPEND CMAKE_REQUIRED_INCLUDES ${X11_INCLUDE_DIR})

set(_xim_config_symbols
    "HAVE_FCHDIR|fchdir"
    "HAVE_FCHOWN|fchown"
    "HAVE_FCHMOD|fchmod"
    "HAVE_FSEEKO|fseeko"
    "HAVE_FSYNC|fsync"
    "HAVE_FTRUNCATE|ftruncate"
    "HAVE_GETCWD|getcwd"
    "HAVE_GETPGID|getpgid"
    "HAVE_GETPWENT|getpwent"
    "HAVE_GETPWNAM|getpwnam"
    "HAVE_GETPWUID|getpwuid"
    "HAVE_GETRLIMIT|getrlimit"
    "HAVE_GETTIMEOFDAY|gettimeofday"
    "HAVE_ICONV|iconv_open"
    "HAVE_INET_NTOP|inet_ntop"
    "HAVE_LOCALTIME_R|localtime_r"
    "HAVE_LSTAT|lstat"
    "HAVE_MEMSET|memset"
    "HAVE_MKDTEMP|mkdtemp"
    "HAVE_NANOSLEEP|nanosleep"
    "HAVE_NL_LANGINFO_CODESET|nl_langinfo"
    "HAVE_OPENDIR|opendir"
    "HAVE_POSIX_OPENPT|posix_openpt"
    "HAVE_PUTENV|putenv"
    "HAVE_QSORT|qsort"
    "HAVE_READLINK|readlink"
    "HAVE_RENAME|rename"
    "HAVE_SELECT|select"
    "HAVE_SETENV|setenv"
    "HAVE_SETPGID|setpgid"
    "HAVE_SETSID|setsid"
    "HAVE_SIGACTION|sigaction"
    "HAVE_SIGALTSTACK|sigaltstack"
    "HAVE_SIGSET|sigset"
    "HAVE_SIGSTACK|sigstack"
    "HAVE_SIGPROCMASK|sigprocmask"
    "HAVE_STRCASECMP|strcasecmp"
    "HAVE_STRCOLL|strcoll"
    "HAVE_STRERROR|strerror"
    "HAVE_STRFTIME|strftime"
    "HAVE_STRNCASECMP|strncasecmp"
    "HAVE_STRPBRK|strpbrk"
    "HAVE_STRPTIME|strptime"
    "HAVE_STRTOL|strtol"
    "HAVE_SYNC|sync"
    "HAVE_SYSCONF|sysconf"
    "HAVE_SYSINFO|sysinfo"
    "HAVE_TGETENT|tgetent"
    "HAVE_TOWLOWER|towlower"
    "HAVE_TOWUPPER|towupper"
    "HAVE_ISWUPPER|iswupper"
    "HAVE_TZSET|tzset"
    "HAVE_UNSETENV|unsetenv"
    "HAVE_USLEEP|usleep"
    "HAVE_UTIME|utime"
    "HAVE_MBLEN|mblen"
    "HAVE_TIMER_CREATE|timer_create"
    "HAVE_CLOCK_GETTIME|clock_gettime"
    "HAVE_UTIMES|utimes"
    "HAVE_ISINF|isinf"
    "HAVE_ISNAN|isnan"
    "HAVE_DIRFD|dirfd"
    "HAVE_FLOCK|flock"
    "HAVE_DLOPEN|dlopen"
    "HAVE_DLSYM|dlsym"
    "HAVE_XUTF8SETWMPROPERTIES|Xutf8SetWMProperties")

foreach(_xim_symbol IN LISTS _xim_config_symbols)
    string(REPLACE "|" ";" _xim_symbol_parts "${_xim_symbol}")
    list(GET _xim_symbol_parts 0 _xim_symbol_macro)
    list(GET _xim_symbol_parts 1 _xim_symbol_name)
    check_function_exists(${_xim_symbol_name} XIM_FUNCTION_${_xim_symbol_macro})
    set(${_xim_symbol_macro} ${XIM_FUNCTION_${_xim_symbol_macro}})
endforeach()

set(CMAKE_REQUIRED_LIBRARIES ${_xim_saved_required_libraries})
set(CMAKE_REQUIRED_INCLUDES ${_xim_saved_required_includes})

# Probes that need a value macro or test more than symbol visibility.
check_type_size("int" VIM_SIZEOF_INT LANGUAGE C)
check_type_size("long" VIM_SIZEOF_LONG LANGUAGE C)
set(_xim_saved_extra_includes ${CMAKE_EXTRA_INCLUDE_FILES})
set(CMAKE_EXTRA_INCLUDE_FILES sys/types.h)
check_type_size("off_t" SIZEOF_OFF_T LANGUAGE C)
set(CMAKE_EXTRA_INCLUDE_FILES time.h)
check_type_size("time_t" SIZEOF_TIME_T LANGUAGE C)
set(CMAKE_EXTRA_INCLUDE_FILES ${_xim_saved_extra_includes})

test_big_endian(WORDS_BIGENDIAN)

check_c_source_compiles([=[
    int main(void)
    {
        static const char build_date[] = __DATE__ " " __TIME__;
        return build_date[0] == 0;
    }
]=] HAVE_DATE_TIME)
check_c_source_compiles([=[
    static void unused_fn(void) __attribute__((unused));
    static void unused_fn(void) { }
    int main(void) { return 0; }
]=] HAVE_ATTRIBUTE_UNUSED)
check_c_source_compiles([=[
    #include <signal.h>
    int main(void) { struct sigcontext *context = 0; return context != 0; }
]=] HAVE_SIGCONTEXT)
check_c_source_compiles([=[
    #include <sys/types.h>
    #include <sys/stat.h>
    int main(void) { struct stat status; return (int)status.st_mtim.tv_nsec; }
]=] XIM_HAVE_ST_MTIM_TV_NSEC)
if(XIM_HAVE_ST_MTIM_TV_NSEC)
    set(ST_MTIM_NSEC st_mtim.tv_nsec)
endif()

check_struct_has_member("struct stat" st_blksize sys/stat.h HAVE_ST_BLKSIZE)
check_struct_has_member("struct sysinfo" mem_unit sys/sysinfo.h
    HAVE_SYSINFO_MEM_UNIT)
check_struct_has_member("struct sysinfo" uptime sys/sysinfo.h
    HAVE_SYSINFO_UPTIME)
check_symbol_exists(FD_CLOEXEC fcntl.h HAVE_FD_CLOEXEC)
check_symbol_exists(_SC_SIGSTKSZ unistd.h HAVE_SYSCONF_SIGSTKSZ)
check_include_file(sys/xattr.h HAVE_XATTR)

check_c_source_compiles([=[
    #include <sys/types.h>
    #include <sys/time.h>
    #include <sys/select.h>
    int main(void) { return 0; }
]=] SYS_SELECT_WITH_SYS_TIME)
if(HAVE_SYS_SELECT_H)
    set(SELECT_TYPE_ARG234 "(fd_set *)")
endif()

if(UNIX)
    set(UNIX TRUE)
    set(USE_XSMP_INTERACT TRUE)
endif()
if(HAVE_MEMSET)
    check_symbol_exists(memmove string.h XIM_HAVE_MEMMOVE)
    if(XIM_HAVE_MEMMOVE)
        set(USEMEMMOVE TRUE)
    endif()
endif()

if(EXISTS "/dev/ptmx")
    check_c_source_compiles([=[
        char *ptsname(int);
        int unlockpt(int);
        int grantpt(int);
        int main(void) { ptsname(0); grantpt(0); unlockpt(0); return 0; }
    ]=] HAVE_SVR4_PTYS)
endif()

if(HAVE_TERMCAP_H AND XIM_TERMINFO_LIBRARY)
    set(_xim_saved_required_libraries ${CMAKE_REQUIRED_LIBRARIES})
    set(CMAKE_REQUIRED_LIBRARIES ${XIM_TERMINFO_LIBRARY})
    check_symbol_exists(ospeed termcap.h HAVE_OSPEED)
    check_symbol_exists(UP termcap.h XIM_HAVE_TERM_UP)
    check_symbol_exists(BC termcap.h XIM_HAVE_TERM_BC)
    check_symbol_exists(PC termcap.h XIM_HAVE_TERM_PC)
    if(XIM_HAVE_TERM_UP AND XIM_HAVE_TERM_BC AND XIM_HAVE_TERM_PC)
        set(HAVE_UP_BC_PC TRUE)
    endif()
    check_c_source_compiles([=[
        #include <termcap.h>
        #include <term.h>
        int main(void) { if (cur_term) del_curterm(cur_term); return 0; }
    ]=] HAVE_DEL_CURTERM)
    check_c_source_compiles([=[
        #include <string.h>
        #include <termcap.h>
        int main(void)
        {
            char *value = tgoto("%p1%d", 0, 1);
            return strcmp(value == 0 ? "" : value, "1") != 0;
        }
    ]=] XIM_HAVE_TERMINFO)
    if(XIM_HAVE_TERMINFO)
        set(TERMINFO TRUE)
    endif()
    if(NOT CMAKE_CROSSCOMPILING)
        check_c_source_runs([=[
            #include <termcap.h>
            int main(void)
            {
                char entry[10000];
                return tgetent(entry, "xim-no-such-terminal") != 0;
            }
        ]=] XIM_TGETENT_RETURNS_ZERO)
    endif()
    if(XIM_TGETENT_RETURNS_ZERO OR CMAKE_CROSSCOMPILING)
        set(TGETENT_ZERO_ERR TRUE)
    endif()
    set(CMAKE_REQUIRED_LIBRARIES ${_xim_saved_required_libraries})
endif()

if(IS_SYMLINK "/proc/self/exe")
    set(PROC_EXE_LINK "/proc/self/exe")
elseif(IS_SYMLINK "/proc/self/path/a.out")
    set(PROC_EXE_LINK "/proc/self/path/a.out")
elseif(IS_SYMLINK "/proc/curproc/file")
    set(PROC_EXE_LINK "/proc/curproc/file")
endif()

find_path(XIM_SELINUX_INCLUDE_DIR selinux/selinux.h)
find_library(XIM_SELINUX_LIBRARY NAMES selinux)
if(XIM_SELINUX_INCLUDE_DIR AND XIM_SELINUX_LIBRARY)
    set(_xim_saved_required_includes ${CMAKE_REQUIRED_INCLUDES})
    set(_xim_saved_required_libraries ${CMAKE_REQUIRED_LIBRARIES})
    set(CMAKE_REQUIRED_INCLUDES ${XIM_SELINUX_INCLUDE_DIR})
    set(CMAKE_REQUIRED_LIBRARIES ${XIM_SELINUX_LIBRARY})
    check_symbol_exists(is_selinux_enabled selinux/selinux.h
        XIM_SELINUX_SYMBOL_FOUND)
    set(CMAKE_REQUIRED_INCLUDES ${_xim_saved_required_includes})
    set(CMAKE_REQUIRED_LIBRARIES ${_xim_saved_required_libraries})
else()
    set(XIM_SELINUX_SYMBOL_FOUND FALSE)
endif()
set(XIM_HAVE_SELINUX ${XIM_SELINUX_SYMBOL_FOUND})
set(HAVE_SELINUX ${XIM_HAVE_SELINUX})

find_program(XIM_MAN_EXECUTABLE man)
if(XIM_MAN_EXECUTABLE)
    execute_process(
        COMMAND ${CMAKE_COMMAND} -E env MANPAGER=cat PAGER=cat
            ${XIM_MAN_EXECUTABLE} -s 2 read
        RESULT_VARIABLE _xim_man_status
        OUTPUT_QUIET ERROR_QUIET)
    if(_xim_man_status EQUAL 0)
        set(USEMAN_S TRUE)
    endif()
endif()

if(DEFINED ENV{SOURCE_DATE_EPOCH} AND NOT "$ENV{SOURCE_DATE_EPOCH}" STREQUAL "")
    find_program(XIM_DATE_EXECUTABLE date)
    if(XIM_DATE_EXECUTABLE)
        execute_process(
            COMMAND ${CMAKE_COMMAND} -E env LC_ALL=C
                ${XIM_DATE_EXECUTABLE} -u -d "@$ENV{SOURCE_DATE_EPOCH}"
                "+%b %d %Y %H:%M:%S"
            OUTPUT_VARIABLE BUILD_DATE
            OUTPUT_STRIP_TRAILING_WHITESPACE
            RESULT_VARIABLE _xim_build_date_status
            ERROR_QUIET)
        if(NOT _xim_build_date_status EQUAL 0)
            execute_process(
                COMMAND ${CMAKE_COMMAND} -E env LC_ALL=C
                    ${XIM_DATE_EXECUTABLE} -u -r "$ENV{SOURCE_DATE_EPOCH}"
                    "+%b %d %Y %H:%M:%S"
                OUTPUT_VARIABLE BUILD_DATE
                OUTPUT_STRIP_TRAILING_WHITESPACE
                RESULT_VARIABLE _xim_build_date_status
                ERROR_QUIET)
        endif()
    endif()
endif()
