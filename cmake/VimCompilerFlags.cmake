# VimCompilerFlags.cmake — Clang-specific compile and link flags.
#
# These flags mirror the reference build captured in temp/XIM_BASELINE.md.
# Every flag here must trace to an equivalent in the autoconf-generated
# `auto/config.mk` so the CMake build does not silently deviate.
#
# AGENTS.md forbids new warnings, so we silence only the warnings the
# reference build silences. CI presets may add `-Werror` later.

# Compile flags shared by every C target in Xim.
#
# - `-Wno-deprecated-declarations` matches the reference. Without it Clang
#   warns about glib / GTK declarations that are still functional.
# - `-D_REENTRANT` matches the reference's POSIX reentrant macros.
# - `-U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=1` is the reference's choice —
#   `_FORTIFY_SOURCE=2` interacts badly with some legacy macros in Vim's
#   older headers, so the build deliberately downgrades to level 1.
set(XIM_COMPILE_FLAGS
    -Wno-deprecated-declarations
    -D_REENTRANT
    -U_FORTIFY_SOURCE
    -D_FORTIFY_SOURCE=1
)

# Link flags shared by every executable target.
#
# `-Wl,--as-needed` matches the reference Makefile's `LINK_AS_NEEDED=yes`
# branch, which calls the linker directly. Skipping link.sh is intentional:
# its only job was iterative library pruning that `--as-needed` already does.
set(XIM_LINK_FLAGS
    -Wl,--as-needed
)

# Apply these to every target in this directory tree (and below) by
# attaching them as default flags. Targets can still override per-file
# flags via set_source_files_properties().
function(xim_apply_default_flags target)
    target_compile_options(${target} PRIVATE ${XIM_COMPILE_FLAGS})
    target_link_options(${target} PRIVATE ${XIM_LINK_FLAGS})
endfunction()