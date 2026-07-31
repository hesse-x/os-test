# Complete musl dirent implementation. getdents is Linux-specific and lives
# outside src/dirent, so include it alongside the directory-wide glob.
file(GLOB MUSL_DIRENT_SOURCES CONFIGURE_DEPENDS
    ${MUSL_DIR}/src/dirent/*.c
    ${MUSL_DIR}/src/linux/getdents.c)

add_musl_lib(musl_dirent_objs SOURCES ${MUSL_DIRENT_SOURCES})
add_musl_lib(musl_dirent_objs_so SOURCES ${MUSL_DIRENT_SOURCES})
