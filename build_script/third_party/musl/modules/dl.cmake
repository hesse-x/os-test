# modules/dl.cmake — musl dlfcn integration (dlfcn).
# Included by musl_rules.cmake after musl_generate_headers() + set(MUSL_DIR).
# Expects: MUSL_DIR, raw add_library, USER_FREESTANDING_FLAGS.
# ===================== musl dlfcn integration (dlfcn) =====================
# Build the upstream musl src/ldso/{dlsym,dlclose,dlinfo,dlerror}.c into libc,
# mirroring musl_fcntl_objs (same musl-internal include order, same dual
# -fno-pie/-fPIC build). dlsym/dlclose/dlinfo wrap symbols the fused loader
# already defines in dynlink.c (__dlsym, __dl_invalid_handle), so they pull the
# non-stub implementations in over musl's weak stubs (src/ldso/*.c ship a
# weak_alias "not supported" fallback that dynlink.c's strong defs override).
# dlerror.c provides the real dlerror()/__dl_seterr/__dl_vseterr: now that the
# runtime fs:0 points at musl's struct pthread (musl_pthread provides the full
# layout incl. dlerror_buf/dlerror_flag), musl's dlerror.c reads/writes the
# correct offsets — no stub needed.
#
# NOT included here:
#   dlopen.c / dladdr.c / dl_iterate_phdr.c — strong definitions already live
#     in the fused ldso/dynlink.c (spliced into libc.so via musl_loader_objs);
#     the weak stubs in src/ldso/*.c would only be needed for a static build
#     without the loader, which we don't have.
#   __dlsym.c — provides a weak stub __dlsym; dynlink.c already defines the
#     strong one, so the stub is dead weight. dlsym.c (the public wrapper) is
#     all we need.
# dlsym.c/dlclose.c/dlinfo.c need _GNU_SOURCE for dlfcn.h to declare dlinfo
# (glibc extension) and for link.h's struct link_map (dlinfo writes one).
# NOTE: dlinfo.c #defines _GNU_SOURCE itself, so we do NOT pass -D_GNU_SOURCE
# on the command line (would trip -Wmacro-redefined). dlsym.c/dlclose.c don't
# need it — dlsym/dlclose are declared unconditionally in dlfcn.h.
set(MUSL_DL_SOURCES
    ${MUSL_DIR}/src/ldso/dlsym.c
    ${MUSL_DIR}/src/ldso/dlclose.c
    ${MUSL_DIR}/src/ldso/dlinfo.c
    ${MUSL_DIR}/src/ldso/dlerror.c)

add_library(musl_dl_objs OBJECT ${MUSL_DL_SOURCES})
target_include_directories(musl_dl_objs PRIVATE
    ${MUSL_DIR}/src/include
    ${MUSL_DIR}/src/internal
    ${MUSL_DIR}/include
    ${MUSL_DIR}/arch/x86_64
    ${MUSL_DIR}/arch/generic
    ${CMAKE_SOURCE_DIR}/user/include
    ${CMAKE_SOURCE_DIR}/include/uapi)
target_compile_options(musl_dl_objs PRIVATE
    -m64 ${USER_FREESTANDING_FLAGS} -D_XOPEN_SOURCE=700 -fno-pie -Wno-all)

# libc.so PIC mirror (same as musl_fcntl_objs_so above).
add_library(musl_dl_objs_so OBJECT ${MUSL_DL_SOURCES})
target_include_directories(musl_dl_objs_so PRIVATE
    ${MUSL_DIR}/src/include
    ${MUSL_DIR}/src/internal
    ${MUSL_DIR}/include
    ${MUSL_DIR}/arch/x86_64
    ${MUSL_DIR}/arch/generic
    ${CMAKE_SOURCE_DIR}/user/include
    ${CMAKE_SOURCE_DIR}/include/uapi)
target_compile_options(musl_dl_objs_so PRIVATE
    -m64 ${USER_FREESTANDING_FLAGS} -D_XOPEN_SOURCE=700 -fPIC -Wno-all)
