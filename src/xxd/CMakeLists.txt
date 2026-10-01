# src/xxd/CMakeLists.txt — standalone `xxd` hex dumper.
#
# xxd is the smallest subproject in Xim. It compiles from a single .c file
# with `-DUNIX`. The reference Makefile invokes:
#
#   $(CC) $(SANITIZER_CFLAGS) $(CFLAGS) $(CPPFLAGS) $(LDFLAGS) -DUNIX \
#       -o xxd xxd.c $(LIBS)
#
# Phase 1 omits SANITIZER_CFLAGS and LIBS — neither is needed on this host,
# and the asan/tsan presets will add the sanitizers via CMake's normal
# mechanism (CMAKE_C_FLAGS_INIT).

add_executable(xxd xxd.c)
target_compile_definitions(xxd PRIVATE UNIX)
xim_apply_default_flags(xxd)

# Install alongside the binaries. The full Phase 2 build will install the
# `vim` binary and runtime tree; xxd is a faithful preview of that flow.
include(GNUInstallDirs)
install(TARGETS xxd RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})