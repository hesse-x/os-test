# Core <sys/stat.h> path/fd metadata and directory creation wrappers.
# Other src/stat sources are owned by existing modules (umask/statvfs) or stay
# out until their complete syscall/dependency sets are supported.
set(MUSL_STAT_SOURCES
    ${MUSL_DIR}/src/stat/stat.c
    ${MUSL_DIR}/src/stat/lstat.c
    ${MUSL_DIR}/src/stat/fstat.c
    ${MUSL_DIR}/src/stat/fstatat.c
    ${MUSL_DIR}/src/stat/futimens.c
    ${MUSL_DIR}/src/stat/mkdir.c
    ${MUSL_DIR}/src/stat/mkdirat.c)

add_musl_lib(musl_stat_objs SOURCES ${MUSL_STAT_SOURCES})
