# libexpat build rules — extracted from user/CMakeLists.txt (was lines 1469-1500).
# Shared `expat.so` (add_third_party_lib) from third_party/libexpat. Defines
# LIBEXPAT_DIR, which wayland.cmake's host wayland-scanner consumes to recompile
# the vendored expat sources for the host — include this BEFORE wayland.cmake.

# ===================== libexpat.so =====================
#
# expat_config.h: hand-written, mirrors the fficonfig.h approach (config template
# lives alongside its .cmake).
configure_file(${CMAKE_CURRENT_LIST_DIR}/expat_config.h
               ${CMAKE_BINARY_DIR}/expat_config.h COPYONLY)

set(LIBEXPAT_DIR ${CMAKE_SOURCE_DIR}/third_party/libexpat/expat)
set(LIBEXPAT_SOURCES
    ${LIBEXPAT_DIR}/lib/xmlparse.c
    ${LIBEXPAT_DIR}/lib/xmlrole.c
    ${LIBEXPAT_DIR}/lib/xmltok.c
    ${LIBEXPAT_DIR}/lib/random_dev_urandom.c
)
set(LIBEXPAT_INCLUDE_DIRS
    ${LIBEXPAT_DIR}/lib              # <expat.h> <internal.h> 等
    ${CMAKE_BINARY_DIR}              # <expat_config.h>
)

# expat.so (shared). expat.h/expat_external.h use XML_ENABLE_VISIBILITY + -fvisibility=hidden
# to restrict exports to XMLPARSEAPI functions only. Unlike ffi_so, we don't force
# -fvisibility=default — let the library's own visibility annotations work.
# XML_DEV_URANDOM is defined in expat_config.h and pulls in random_dev_urandom.c
# for entropy (opens /dev/urandom, which our kernel provides via devtmpfs).
add_third_party_lib(expat_so
    SOURCES ${LIBEXPAT_SOURCES}
    C
    OUTPUT_NAME expat
    SO_LINK_LIBS c
    FLAGS "-O2 -DXML_ENABLE_VISIBILITY=1"
    INCLUDE_DIRS ${LIBEXPAT_INCLUDE_DIRS}
    GEN_HEADERS ${CMAKE_BINARY_DIR}/expat_config.h
)
