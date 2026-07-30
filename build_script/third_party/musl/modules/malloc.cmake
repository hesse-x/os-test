# modules/malloc.cmake — musl malloc integration (mallocng).
# Included by musl_rules.cmake after musl_generate_headers() + set(MUSL_DIR).
# Expects: MUSL_DIR, add_musl_lib.
# ===================== musl mallocng integration =====================
# Build the upstream musl malloc (mallocng) into libc, replacing the repo's
# hand-written user/lib/malloc.cc (deleted this batch). musl 1.2.x's dynamic
# loader (ldso/dynlink.c) calls __libc_malloc/__libc_free/__libc_calloc/
# __libc_realloc and reads __malloc_replaced — these are mallocng's internal
# entry points (the loader #defines malloc __libc_malloc etc. so it always uses
# libc's own allocator, bypassing any user replacement). v1.1.19's loader called
# plain malloc(), so the hand-written allocator was enough; v1.2.x requires the
# __libc_* surface, which only mallocng provides.
#
# Source layout (mirrors musl's Makefile with MALLOC_DIR=mallocng, the default):
#   src/malloc/mallocng/*.c — the real allocator. glue.h renames the malloc/
#     free/realloc *definitions* to __libc_malloc_impl/__libc_realloc/__libc_free
#     (so the loader's __libc_* calls land here) and remaps mmap/madvise/mremap
#     to the __mmap/__madvise/__mremap already supplied by musl_mman_objs.
#     brk() is a glue.h macro → __syscall(SYS_brk); this kernel's sys_brk is a
#     stub returning 0 (no brk heap — user heap is mmap-managed), so mallocng's
#     brk probe fails cleanly (ctx.brk = -1) and it runs in mmap-only mode,
#     identical to musl on brk-less archs (aarch64 Linux). donate.c's
#     __malloc_donate is called only from ldso/dynlink.c (donates the main
#     program's BSS-tail pages); the static __libc_start_main path does NOT call
#     it — mallocng lazily inits its static ctx on first malloc.
#   src/malloc/*.c — public-symbol wrappers + replaced.c:
#     free.c/realloc.c:  public free/realloc → __libc_free/__libc_realloc.
#     calloc.c:          public calloc (calls malloc, then mal0_clear/memset).
#     libc_calloc.c:     #define calloc __libc_calloc + #include "calloc.c"
#                        → supplies __libc_calloc for the loader.
#     memalign.c/posix_memalign.c/reallocarray.c: thin wrappers over
#                        aligned_alloc/realloc.
#     replaced.c:        int __malloc_replaced, __aligned_alloc_replaced (the
#                        flags the loader sets when a user replaces malloc; 0
#                        here — no LD_PRELOAD/malloc-replace surface on this OS).
#   lite_malloc.c — ALSO included. It provides the public `malloc` symbol via
#     weak_alias(default_malloc, malloc) → __libc_malloc_impl. mallocng's
#     __libc_malloc_impl is a STRONG definition that overrides lite_malloc's
#     WEAK weak_alias(__simple_malloc, __libc_malloc_impl), so the bump
#     allocator in lite_malloc is dead at link time and mallocng wins. This is
#     musl's standard coexistence trick (both files always ship); excluding
#     lite_malloc.c would leave the public `malloc` symbol undefined (calloc.c
#     and user code call malloc(), not __libc_malloc). __libc_malloc is likewise
#     provided by lite_malloc.c.
#
# EXCLUDED:
#   src/malloc/oldmalloc/* — the alternate (pre-mallocng) allocator; building it
#     alongside mallocng would multi-define __libc_malloc_impl/malloc.
#
# Dependencies (all already in libc): __mmap/__madvise/__mremap/__munmap
# (musl_mman_objs), __lock/a_cas/LOCK_OBJ_DEF (musl_pthread), __syscall (inline
# in arch/x86_64/syscall_arch.h), memset/memcpy (musl_string_objs).
set(MUSL_MALLOC_SOURCES
    # mallocng core (the real allocator; glue.h namespaces malloc→__libc_malloc_impl)
    ${MUSL_DIR}/src/malloc/mallocng/malloc.c
    ${MUSL_DIR}/src/malloc/mallocng/free.c
    ${MUSL_DIR}/src/malloc/mallocng/realloc.c
    ${MUSL_DIR}/src/malloc/mallocng/aligned_alloc.c
    ${MUSL_DIR}/src/malloc/mallocng/malloc_usable_size.c
    ${MUSL_DIR}/src/malloc/mallocng/donate.c
    # public-symbol wrappers + loader entry points
    ${MUSL_DIR}/src/malloc/free.c
    ${MUSL_DIR}/src/malloc/realloc.c
    ${MUSL_DIR}/src/malloc/calloc.c
    ${MUSL_DIR}/src/malloc/libc_calloc.c
    ${MUSL_DIR}/src/malloc/memalign.c
    ${MUSL_DIR}/src/malloc/posix_memalign.c
    ${MUSL_DIR}/src/malloc/reallocarray.c
    ${MUSL_DIR}/src/malloc/replaced.c
    # public malloc/__libc_malloc provider (weak __libc_malloc_impl overridden by
    # mallocng's strong def; __simple_malloc becomes dead code at link). See above.
    ${MUSL_DIR}/src/malloc/lite_malloc.c)

add_musl_lib(musl_malloc_objs SOURCES ${MUSL_MALLOC_SOURCES})
add_musl_lib(musl_malloc_objs_so SOURCES ${MUSL_MALLOC_SOURCES})
