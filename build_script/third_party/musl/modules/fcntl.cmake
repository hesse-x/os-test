# modules/fcntl.cmake — musl fcntl integration (fcntl_worklist §3d).
# Included by musl_rules.cmake after musl_generate_headers() + set(MUSL_DIR).
# Expects: MUSL_DIR, raw add_library, USER_FREESTANDING_FLAGS.
# ===================== musl fcntl integration (fcntl_worklist §3d) =====================
# Build the upstream musl src/fcntl/*.c into libc via a separate OBJECT library,
# mirroring musl_unistd_objs (same musl-internal include order, same shim).
# musl's open.c / openat.c / fcntl.c / creat.c / posix_fadvise.c /
# posix_fallocate.c replace the repo's hand-written file.cc wrappers —
# open/openat/fcntl are deleted from file.cc this batch.
#
# openat.c IS adopted: musl's openat passes AT_FDCWD straight to SYS_openat,
# and the kernel's sys_openat resolves AT_FDCWD to the process cwd (the M0.4
# resolve_dirfd_start fix in kernel/bsd/vfs.c resolves bp->cwd to its inode),
# so openat(AT_FDCWD, rel) honors a prior chdir. The repo's file.cc openat
# wrapper (which prepended a userspace cwd copy via resolve_at_path) and the
# fcntl_ext.h LIBC_EXPORT re-declaration are deleted.
#
# open.c routes to SYS_open (SYS_open is defined in bits/syscall.h, so musl's
# __sys_open_cp picks the 2/3-arg SYS_open path, not SYS_openat). The kernel's
# sys_open resolves relatives against bp->cwd via vfs_resolve_user. musl chdir
# (adopted via musl_unistd_objs) keeps bp->cwd in sync.
#
# No need to pull syscall_ret.c / __syscall_cp.c here: __syscall_ret and
# __syscall_cp are provided by musl_pthread (the single source for both);
# the fcntl wrappers' syscall(...) / syscall_cp(...) / __syscall(...) resolve
# at link time.
set(MUSL_FCNTL_SOURCES
    ${MUSL_DIR}/src/fcntl/open.c
    ${MUSL_DIR}/src/fcntl/openat.c
    ${MUSL_DIR}/src/fcntl/fcntl.c
    ${MUSL_DIR}/src/fcntl/creat.c
    ${MUSL_DIR}/src/fcntl/posix_fadvise.c
    ${MUSL_DIR}/src/fcntl/posix_fallocate.c)

add_library(musl_fcntl_objs OBJECT ${MUSL_FCNTL_SOURCES})
target_include_directories(musl_fcntl_objs PRIVATE
    ${MUSL_DIR}/src/include
    ${MUSL_DIR}/src/internal
    ${MUSL_DIR}/include
    ${MUSL_DIR}/arch/x86_64
    ${MUSL_DIR}/arch/generic
    ${CMAKE_SOURCE_DIR}/user/include
    ${CMAKE_SOURCE_DIR}/include/uapi)
target_compile_options(musl_fcntl_objs PRIVATE -m64 ${USER_FREESTANDING_FLAGS} -D_XOPEN_SOURCE=700 -fno-pie -Wno-all)

# libc.so needs PIC objects (mirror the libc.a(-fno-pie)/libc.so(-fPIC) dual
# build): a second OBJECT library, musl_fcntl_objs_so, compiles the SAME musl
# sources with -fPIC for the shared link.
add_library(musl_fcntl_objs_so OBJECT ${MUSL_FCNTL_SOURCES})
target_include_directories(musl_fcntl_objs_so PRIVATE
    ${MUSL_DIR}/src/include
    ${MUSL_DIR}/src/internal
    ${MUSL_DIR}/include
    ${MUSL_DIR}/arch/x86_64
    ${MUSL_DIR}/arch/generic
    ${CMAKE_SOURCE_DIR}/user/include
    ${CMAKE_SOURCE_DIR}/include/uapi)
target_compile_options(musl_fcntl_objs_so PRIVATE -m64 ${USER_FREESTANDING_FLAGS} -D_XOPEN_SOURCE=700 -fPIC -Wno-all)
