# modules/dirent.cmake — musl dirent integration (dirent.md).
# Included by musl_rules.cmake after musl_generate_headers() + set(MUSL_DIR).
# Expects: MUSL_DIR, raw add_library, USER_FREESTANDING_FLAGS.
# ===================== musl dirent integration (dirent.md) =====================
# Build the upstream musl src/dirent/*.c into libc, mirroring musl_fcntl_objs
# (same musl-internal include order, same dual -fno-pie/-fPIC build). These
# replace the repo's hand-written opendir/readdir/closedir/seekdir/telldir/
# rewinddir/dirfd/readdir_r/scandir block that lived in user/lib/file.cc
# (deleted this batch).
#
# Layout match is the enabler: musl's struct dirent { ino_t d_ino; off_t d_off;
# unsigned short d_reclen; unsigned char d_type; char d_name[256]; } is
# field-for-field identical to the kernel's struct dirent64
# (include/uapi/xos/dirent.h), so musl's readdir() returns a pointer straight
# into the DIR's getdents buffer — no per-field copy. fat32 already fills
# d_type (DT_DIR/DT_REG), which the old hand-written readdir discarded.
#
# musl's readdir.c calls __syscall(SYS_getdents, …) = 78 directly; the kernel
# aliases SYS_GETDENTS (78) onto sys_getdents in kernel/bsd/syscall.c (returns
# dirent64 records), so musl's path resolves with NO libc-side glue. The public
# getdents() symbol is kept for the Linux ABI surface: musl 1.2.x split the old
# src/dirent/__getdents.c (which weak_alias'd __getdents→getdents + LFS64) into
# src/dirent/posix_getdents.c (POSIX posix_getdents) and src/linux/getdents.c
# (Linux getdents, likewise routing through 78). We compile the linux copy so
# libc still exports getdents; getdents64 (the old LFS64 alias) is gone upstream
# and has no in-tree consumer.
#
# LOCK/UNLOCK (used by readdir_r/seekdir/rewinddir) resolve to __lock/__unlock
# from musl_pthread's src/thread/__lock.c (file(GLOB …/src/thread/*.c)). No extra
# source needed. fdopendir.c's fstat + fcntl deps come from file.cc's statx
# AT_EMPTY_PATH fstat and musl_fcntl_objs respectively.
#
# alphasort.c IS included (alongside versionsort.c): it calls strcoll, whose
# locale chain (src/locale/strcoll.c — musl_locale_objs, libc.map <locale.h>
# block) is now built (see doc/design/todo.md §412). The original §4a exclusion
# ("not worth dragging in the locale chain for a no-consumer API") is stale:
# strcoll is already exported (strcoll@@LIBC_1.0), so alphasort is free, and
# Mesa's xmlconfig.c (scandir(..., alphasort) under _GNU_SOURCE) needs it.
# versionsort's only dep strverscmp is supplied by the musl_string_objs glob
# (src/string/*.c, see string.md) — no separate strverscmp.c here, or both
# tiers multi-define strverscmp.
set(MUSL_DIRENT_SOURCES
    ${MUSL_DIR}/src/dirent/opendir.c
    ${MUSL_DIR}/src/dirent/readdir.c
    ${MUSL_DIR}/src/dirent/readdir_r.c
    ${MUSL_DIR}/src/dirent/closedir.c
    ${MUSL_DIR}/src/dirent/seekdir.c
    ${MUSL_DIR}/src/dirent/telldir.c
    ${MUSL_DIR}/src/dirent/rewinddir.c
    ${MUSL_DIR}/src/dirent/dirfd.c
    ${MUSL_DIR}/src/dirent/scandir.c
    ${MUSL_DIR}/src/dirent/alphasort.c
    ${MUSL_DIR}/src/linux/getdents.c
    ${MUSL_DIR}/src/dirent/fdopendir.c
    ${MUSL_DIR}/src/dirent/versionsort.c)

add_library(musl_dirent_objs OBJECT ${MUSL_DIRENT_SOURCES})
target_include_directories(musl_dirent_objs PRIVATE
    ${MUSL_DIR}/src/include
    ${MUSL_DIR}/src/internal
    ${MUSL_DIR}/include
    ${MUSL_DIR}/arch/x86_64
    ${MUSL_DIR}/arch/generic
    ${CMAKE_SOURCE_DIR}/user/include
    ${CMAKE_SOURCE_DIR}/include/uapi)
target_compile_options(musl_dirent_objs PRIVATE
    -m64 ${USER_FREESTANDING_FLAGS} -D_XOPEN_SOURCE=700 -fno-pie -Wno-all -Wno-ignored-attributes)

# libc.so PIC mirror (same as musl_fcntl_objs_so above).
add_library(musl_dirent_objs_so OBJECT ${MUSL_DIRENT_SOURCES})
target_include_directories(musl_dirent_objs_so PRIVATE
    ${MUSL_DIR}/src/include
    ${MUSL_DIR}/src/internal
    ${MUSL_DIR}/include
    ${MUSL_DIR}/arch/x86_64
    ${MUSL_DIR}/arch/generic
    ${CMAKE_SOURCE_DIR}/user/include
    ${CMAKE_SOURCE_DIR}/include/uapi)
target_compile_options(musl_dirent_objs_so PRIVATE
    -m64 ${USER_FREESTANDING_FLAGS} -D_XOPEN_SOURCE=700 -fPIC -Wno-all -Wno-ignored-attributes)
