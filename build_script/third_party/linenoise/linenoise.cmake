# linenoise build rules — vendored linenoise (third_party/linenoise, antirez).
#
# linenoise is a single-file (~2000 line) line-editing library (history, Tab
# completion, Ctrl-R search) used by redis/sqlite/mongo. The shell (user/shell)
# links it dynamically so the interactive prompt gains readline-class editing
# without dragging in termcap/ncurses (linenoise hard-codes the few CSI
# sequences it needs + queries winsize via ioctl).
#
# Upstream is not subject to our -Werror gate, so we build it through
# add_third_party_lib (branch B: custom-command .so) which disables warnings
# (-w) and uses os_base_options-equivalent freestanding basics — same posture
# as libdrm.so / libudev.so.
#
# Product: liblinenoise.so → /lib/liblinenoise.so (ld.so default search path),
# DT_NEEDED libc.so. Target `linenoise_so` is what add_user_dyn_elf's LINK_LIBS
# dependency scan matches (`${lib}_so` for `LINK_LIBS linenoise`).
# linenoise.h is exposed via INTERFACE_INCLUDE_DIRS so the shell gets the
# header path by linking `linenoise` without an explicit -I at the call site.
#
# FLAGS "-fvisibility=default": add_third_party_lib compiles with
# -fvisibility=hidden (only export-marked symbols are exported). linenoise
# upstream uses no visibility attributes, so the default-hidden would hide its
# whole public API and the shell would fail to resolve any linenoise* symbol.
# Overriding to default exports every non-static function — linenoise's public
# API is exactly its non-static surface (linenoise.h), same posture as libudev.so
# (user/CMakeLists.txt libudev_so FLAGS).

set(LINENOISE_DIR ${CMAKE_SOURCE_DIR}/third_party/linenoise)

add_third_party_lib(linenoise_so
    C
    SOURCES ${LINENOISE_DIR}/linenoise.c
    OUTPUT_NAME linenoise
    INTERFACE_INCLUDE_DIRS ${LINENOISE_DIR}
    FLAGS "-fvisibility=default"
    SO_LINK_LIBS c)
