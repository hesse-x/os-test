# modules/mman.cmake — musl mman integration (musl_worklist).
# Included by musl_rules.cmake after musl_generate_headers() + set(MUSL_DIR).
# Expects: MUSL_DIR, raw add_library, USER_FREESTANDING_FLAGS.
# ===================== musl mman integration (musl_worklist) =====================
# Build the upstream musl src/mman/*.c into libc, mirroring musl_fcntl_objs
# (same musl-internal include order, same dual -fno-pie/-fPIC build). These
# replace the repo's hand-written mmap/munmap/mprotect (sys_process.cc) and
# mremap/shm_unlink (musl_missing.c), which are deleted this batch.
#
# Every file here is a pure syscall passthrough (1-30 lines): mmap → SYS_mmap(9),
# munmap → SYS_munmap(11), mprotect → SYS_mprotect(10) (musl page-rounds both
# ends before the call; the kernel rejects only non-page-aligned addr, so the
# aligned value musl hands it is accepted), mremap → SYS_mremap(25), msync →
# SYS_msync(26), madvise/posix_madvise → SYS_madvise(28), mincore → SYS_mincore(27),
# mlock/munlock/mlockall/munlockall → SYS_mlock(149)/.../. musl mmap.c's
# weak_alias(__mmap,mmap) supplies the real __mmap/__munmap/__mprotect/__mremap
# the already-migrated musl pthread/malloc/locale/time reach for (their
# forward-declared internal refs); the prior hidden forwarders in musl_glue.c
# are deleted this batch to avoid a duplicate-definition clash.
#
# msync.c uses syscall_cp (cancellation point); __syscall_cp is provided by
# musl_pthread at link time — no shim needed here (same as musl_fcntl_objs).
# posix_madvise.c returns the raw (positive) errno from -__syscall; that is musl
# upstream behaviour, not our concern.
#
# EXCLUDED:
#   memfd_create — musl src/mman has no memfd_create (it lives in musl
#     src/linux/); the repo's hand-written wrapper in sys_process.cc is RETAINED
#     (routes to SYS_MEMFD_CREATE=319). Adding musl/src/linux/memfd_create.c
#     would pull the whole src/linux glob; the one wrapper is cheaper.
#   remap_file_pages — musl mremap.c does NOT define it (it is in src/linux/);
#     not adopted.
#
# msync/madvise/mlock/mlockall/munlock/mincore have NO kernel handler (sys_msync
# etc. are absent from kernel/bsd/syscall.c). The syscall dispatch returns
# -ENOSYS for unregistered numbers, so the musl wrappers expose the symbols
# (callers link) but return -1/ENOSYS — the same surface glibc/Linux would give
# before the feature existed. This is the documented "POSIX symbol present,
# kernel feature absent" middle state (doc/design/todo.md).
set(MUSL_MMAN_SOURCES
    ${MUSL_DIR}/src/mman/mmap.c
    ${MUSL_DIR}/src/mman/munmap.c
    ${MUSL_DIR}/src/mman/mprotect.c
    ${MUSL_DIR}/src/mman/mremap.c
    ${MUSL_DIR}/src/mman/msync.c
    ${MUSL_DIR}/src/mman/madvise.c
    ${MUSL_DIR}/src/mman/posix_madvise.c
    ${MUSL_DIR}/src/mman/mincore.c
    ${MUSL_DIR}/src/mman/mlock.c
    ${MUSL_DIR}/src/mman/munlock.c
    ${MUSL_DIR}/src/mman/mlockall.c
    ${MUSL_DIR}/src/mman/munlockall.c
    ${MUSL_DIR}/src/mman/shm_open.c)

add_library(musl_mman_objs OBJECT ${MUSL_MMAN_SOURCES})
target_include_directories(musl_mman_objs PRIVATE
    ${MUSL_DIR}/src/internal
    ${MUSL_DIR}/include
    ${MUSL_DIR}/arch/x86_64
    ${MUSL_DIR}/arch/generic
    ${CMAKE_SOURCE_DIR}/user/include
    ${CMAKE_SOURCE_DIR}/include/uapi)
target_compile_options(musl_mman_objs PRIVATE -m64 ${USER_FREESTANDING_FLAGS} -fno-pie -Wno-all -Wno-ignored-attributes)

# libc.so PIC mirror (same as musl_fcntl_objs_so above).
add_library(musl_mman_objs_so OBJECT ${MUSL_MMAN_SOURCES})
target_include_directories(musl_mman_objs_so PRIVATE
    ${MUSL_DIR}/src/internal
    ${MUSL_DIR}/include
    ${MUSL_DIR}/arch/x86_64
    ${MUSL_DIR}/arch/generic
    ${CMAKE_SOURCE_DIR}/user/include
    ${CMAKE_SOURCE_DIR}/include/uapi)
target_compile_options(musl_mman_objs_so PRIVATE -m64 ${USER_FREESTANDING_FLAGS} -fPIC -Wno-all -Wno-ignored-attributes)
