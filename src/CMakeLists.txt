# src/CMakeLists.txt — top of the Xim source tree.
#
# Phase 1 (this file): builds only the `xxd` subproject so we can prove the
# Clang + Ninja + out-of-tree pipeline works before adding the main `vim`
# target. Subsequent phases will add:
#
#   Phase 2: the main `vim` executable target
#   Phase 3: add_custom_command() for auto/{osdef.h, pathdef.c} and
#            src/auto/wayland/*.{c,h}
#   Phase 4: CTest integration in src/testdir/CMakeLists.txt
#   Phase 5: feature configuration (cmake/VimFeatures.cmake)
#   Phase 6: drop the autoconf-based `src/auto/config.h`

add_subdirectory(xxd)