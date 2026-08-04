# libpng build rules — vendored libpng (third_party/libpng).
#
# Shared `png.so` (add_third_party_lib) for the compositor wallpaper client
# (user/compositor/wallpaper.c decodes wallpaper.png through libpng).
# SO_LINK_LIBS z c → DT_NEEDED libz.so (deflate/inflate) + libc.so.
#
# libpng is configure-driven upstream, but ships pnglibconf.h.prebuilt: the
# published default configuration with no @placeholders@. Copy it to
# ${CMAKE_BINARY_DIR}/pnglibconf.h at configure time (mirrors zlib's
# configure-free zconf.h approach) and put the binary dir on the include path;
# pngpriv.h does #include "pnglibconf.h".
#
# -fvisibility=default: like zlib, libpng's Linux PNG_EXPORT expands to nothing
# (the dllexport/visibility branches are Windows/optional), so the rule's
# default -fvisibility=hidden would produce a .so exporting zero symbols.

set(LIBPNG_DIR ${CMAKE_SOURCE_DIR}/third_party/libpng)

# pngsimd.c pulls intel/filter_sse2_intrinsics.c (__SSE2__ is always defined on
# x86_64, so intel/check.h enables the SSE2 path), which needs <immintrin.h>.
# -nostdinc drops the compiler resource headers, so add clang's resource
# include dir back explicitly.
execute_process(COMMAND ${CMAKE_C_COMPILER} -print-resource-dir
    OUTPUT_VARIABLE CLANG_RESOURCE_DIR OUTPUT_STRIP_TRAILING_WHITESPACE)

configure_file(${LIBPNG_DIR}/pnglibconf.h.prebuilt
    ${CMAKE_BINARY_DIR}/pnglibconf.h COPYONLY)

set(LIBPNG_SOURCES
    ${LIBPNG_DIR}/png.c
    ${LIBPNG_DIR}/pngerror.c
    ${LIBPNG_DIR}/pngget.c
    ${LIBPNG_DIR}/pngmem.c
    ${LIBPNG_DIR}/pngpread.c
    ${LIBPNG_DIR}/pngread.c
    ${LIBPNG_DIR}/pngrio.c
    ${LIBPNG_DIR}/pngrtran.c
    ${LIBPNG_DIR}/pngrutil.c
    ${LIBPNG_DIR}/pngset.c
    ${LIBPNG_DIR}/pngsimd.c
    ${LIBPNG_DIR}/pngtrans.c
    ${LIBPNG_DIR}/pngwio.c
    ${LIBPNG_DIR}/pngwrite.c
    ${LIBPNG_DIR}/pngwtran.c
    ${LIBPNG_DIR}/pngwutil.c
)

add_third_party_lib(png_so
    SOURCES ${LIBPNG_SOURCES}
    C
    OUTPUT_NAME png
    SO_LINK_LIBS z c
    INCLUDE_DIRS ${LIBPNG_DIR} ${CMAKE_BINARY_DIR} ${CMAKE_SOURCE_DIR}/third_party/zlib
        ${CLANG_RESOURCE_DIR}/include
    FLAGS "-fvisibility=default"
)
