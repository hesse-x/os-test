# zlib build rules — vendored zlib (third_party/zlib, v1.3.1).
#
# Shared `zlib.so` (add_third_party_lib) for Mesa's runtime dep (driconf / mesa_util
# compression paths). Mesa meson resolves zlib via pkg-config (zlib.pc, emitted by
# gen-pkgconfig.sh); the .so + zlib.h/zconf.h must land in the sysroot (install-libs.sh
# + install-headers.sh).
#
# zlib ships a configure-free zconf.h (the published zconf.h has no @placeholders@;
# zconf.h.in is only for autoconf). We use the source-tree zconf.h directly — no
# configure step, mirroring libexpat's hand-written config-header approach.

# ===================== libz.so =====================
set(ZLIB_DIR ${CMAKE_SOURCE_DIR}/third_party/zlib)
set(ZLIB_SOURCES
    ${ZLIB_DIR}/adler32.c
    ${ZLIB_DIR}/compress.c
    ${ZLIB_DIR}/crc32.c
    ${ZLIB_DIR}/deflate.c
    ${ZLIB_DIR}/gzclose.c
    ${ZLIB_DIR}/gzlib.c
    ${ZLIB_DIR}/gzread.c
    ${ZLIB_DIR}/gzwrite.c
    ${ZLIB_DIR}/infback.c
    ${ZLIB_DIR}/inffast.c
    ${ZLIB_DIR}/inflate.c
    ${ZLIB_DIR}/inftrees.c
    ${ZLIB_DIR}/trees.c
    ${ZLIB_DIR}/uncompr.c
    ${ZLIB_DIR}/zutil.c
)
set(ZLIB_INCLUDE_DIRS
    ${ZLIB_DIR}                 # <zlib.h> <zconf.h> (same dir; zlib.h does #include "zconf.h")
)

# zlib.so (shared). add_third_party_lib adds -fPIC -fvisibility=hidden -w.
#
# -fvisibility=default override: zlib's public API uses ZEXTERN/ZEXPORT macros,
# but on Linux zconf.h expands ZEXPORT to *nothing* (no __attribute__((visibility)),
# no dllexport — those are Windows-only branches). So with the rule's default
# -fvisibility=hidden, every symbol (deflate, inflate, ...) defaults to hidden and
# the .so exports ZERO symbols — Mesa's link then fails with undefined reference
# to deflate/inflate/deflateInit_ against a libz.so that is built but empty. The
# zlib.cmake comment that claimed "ZEXPORT exports are not affected by visibility"
# was wrong. -fvisibility=default (last-flag-wins on the line) re-exports the
# public API; zlib has no private symbols worth hiding anyway, so default
# visibility is the honest fix.
#
# -include unistd.h: gzguts.h includes <fcntl.h> (after #define _POSIX_SOURCE) but NOT
# <unistd.h>, where read/write/close/lseek are declared. Under -ffreestanding with musl,
# fcntl.h doesn't transitively pull unistd.h, so the gz*.c files fail with "call to
# undeclared function 'read'/'write'/'close'/'lseek'". Force-include unistd.h (mirrors
# libdrm's -include libdrm_config.h) so the POSIX decls are visible before gz*.c.
add_third_party_lib(z_so
    SOURCES ${ZLIB_SOURCES}
    C
    OUTPUT_NAME z
    SO_LINK_LIBS c
    INCLUDE_DIRS ${ZLIB_INCLUDE_DIRS}
    FLAGS "-include unistd.h -fvisibility=default"
)
