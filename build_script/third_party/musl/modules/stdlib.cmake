# modules/stdlib.cmake — musl stdlib integration (stdlib.md).
# Included by musl_rules.cmake after musl_generate_headers() + set(MUSL_DIR).
# Expects: MUSL_DIR, add_musl_lib.
# ===================== musl stdlib integration (stdlib.md) =====================
# Build the upstream musl stdlib + prng + env + exit chain into libc, mirroring
# musl_string_objs / musl_math_objs (one -fPIC OBJECT sub-library via add_musl_lib,
# wired into BOTH libc.a and libc.so via EXTRA_OBJS — -fPIC objects link fine
# into a -no-pie static ELF too, so a single compile serves both).
#
# This batch RETIRES the repo's hand-written startup/env/exit shim:
#   user/lib/start_main.cc (deleted)   — __libc_start_main, atexit,
#     __libc_run_atexit, __libc_run_init_array/__libc_run_fini_array,
#     __libc_fini_array_trampoline, decode_auxv, environ/__environ,
#     getenv/setenv/putenv/unsetenv/clearenv, __libc_env_init.
#   user/lib/musl_startup.c (deleted)  — musl_libc_init_aux (set libc.page_size/
#     auxv); redundant now that musl __init_libc sets them.
#   user/lib/stdlib_misc.c (slimmed)   — exit, rand/srand/rand_r,
#     abs/labs/llabs/imaxabs/imaxdiv/div/ldiv/lldiv, qsort/bsearch removed.
#   user/lib/signal.cc (abort deleted) — musl src/exit/abort.c takes over.
#   user/lib/strtol.c (deleted)        — musl strtol.c/strtod.c/atoi/atol/atoll
#     cover the whole strto*/ato* family.
# musl's __libc_start_main (src/env/__libc_start_main.c) drives BOTH paths:
#   static (DYNAMIC=0): __init_libc decodes the kernel pair-form auxv into the
#     flat AT_*-indexed array musl expects, sets __environ/libc.page_size/
#     __hwcap/__sysinfo/__progname, calls __init_tls (mmaps PT_TLS, sets
#     FS_BASE) + __init_ssp (canary); then libc_start_init walks
#     __init_array_start..end. exit() runs __libc_exit_fini (exit.c walks
#     __fini_array_start..end + _fini) — the linker script provides these.
#   dynamic (DYNAMIC=1): the fused loader (__dls3 in dynlink.c) already set
#     __environ/libc.page_size/__hwcap and ran __init_tp for TLS before jumping
#     to the main ELF _start; musl __libc_start_main then calls __init_libc
#     (idempotent — __init_tls is a no-op in dynlink.c, __init_ssp re-sets the
#     canary) and __libc_start_init = dynlink do_init_fni(tail) over every
#     DSO's .init_array. exit() runs __libc_exit_fini = dynlink's fini walk.
# The kernel-built auxv (kernel/bsd/proc.c execve / proc_create.c) provides
# AT_PHDR/PHENT/PHNUM/ENTRY/PAGESZ/RANDOM/EXECFN + AT_UID==AT_EUID==AT_GID==
# AT_EGID==0, AT_SECURE=0, so __init_libc's secure-check returns early (no
# /dev/null open) and getenv("LD_LIBRARY_PATH") works in the loader.
#
# EXCLUDED from this batch (kept as repo implementations, tracked in todo.md):
#   src/misc/realpath.c   — needs /proc/self/fd/N + readlink + O_PATH (no
#     procfs); repo stdlib_misc.c:realpath (getcwd + lexical collapse) kept.
#   src/temp/{mkstemp,__randname,...}.c — __randname needs __clock_gettime
#     (time module not yet migrated; would clash with repo time.cc); repo
#     stdlib_misc.c:mkstemp/mktemp (getpid-based) kept.
#   src/conf/sysconf.c + src/legacy/getpagesize.c — musl sysconf redefines
#     _SC_NPROCESSORS_ONLN semantics; repo stdlib_misc.c (sys_sysconf-backed)
#     kept.
#   src/stdlib/{wcstod,wcstol}.c — compiled in musl_wchar_objs (wchar tier).
#   src/stdlib/{ecvt,fcvt,gcvt}.c — deprecated BSD, 0 callers, sprintf chain.
#   src/env/{__init_tls,__stack_chk_fail,__reset_tls}.c — already in
#     musl_pthread; compiling here would multi-define.
#   src/exit/assert.c — repo lib/assert.c keeps __assert_fail.
#   src/exit/_Exit.c — already in musl_unistd_objs (+ _so); would multi-def _Exit.
#   src/prng/{random,drand48,...}.c — 0 callers (rand/rand_r suffice).
set(MUSL_STDLIB_SOURCES
    # pure compute
    ${MUSL_DIR}/src/stdlib/abs.c
    ${MUSL_DIR}/src/stdlib/labs.c
    ${MUSL_DIR}/src/stdlib/llabs.c
    ${MUSL_DIR}/src/stdlib/imaxabs.c
    ${MUSL_DIR}/src/stdlib/imaxdiv.c
    ${MUSL_DIR}/src/stdlib/div.c
    ${MUSL_DIR}/src/stdlib/ldiv.c
    ${MUSL_DIR}/src/stdlib/lldiv.c
    ${MUSL_DIR}/src/stdlib/atoi.c
    ${MUSL_DIR}/src/stdlib/atol.c
    ${MUSL_DIR}/src/stdlib/atoll.c
    # one file provides strtol/strtoul/strtoll/strtoull/strtoimax/strtoumax +
    # the __strtol_internal weak aliases.
    ${MUSL_DIR}/src/stdlib/strtol.c
    # one file provides strtod/strtof/strtold + _l weak aliases (deps on
    # floatscan + shgetc, both in src/internal, + libm already merged in libc).
    ${MUSL_DIR}/src/stdlib/strtod.c
    ${MUSL_DIR}/src/stdlib/atof.c
    ${MUSL_DIR}/src/stdlib/bsearch.c
    # qsort_r lives in qsort.c (__qsort_r + weak_alias qsort_r); the public
    # qsort (no _r) lives in qsort_nr.c — a thin wrapper that calls __qsort_r
    # with a 2-arg→3-arg cmp adapter. v1.1.19 had qsort in qsort.c directly;
    # v1.2.6 split it (the nr variant is the default non-recursive qsort). Both
    # must be compiled or the public qsort (libc.map) is undefined.
    ${MUSL_DIR}/src/stdlib/qsort.c
    ${MUSL_DIR}/src/stdlib/qsort_nr.c
    # prng: musl's LCG (31-bit, RAND_MAX=0x7fffffff) replaces the repo's
    # 15-bit LCG. random/drand48 excluded (0 callers).
    ${MUSL_DIR}/src/prng/rand.c
    ${MUSL_DIR}/src/prng/rand_r.c
    # strtod/strtol scanner deps (src/internal, pure compute — __floatscan uses
    # only libm + errno; __shgetc fallback-references __uflow). __uflow is now
    # supplied by musl src/stdio/__uflow.c in the musl_stdio_objs glob (the old
    # scan_uflow_stub.c stub is deleted — see the __stdio_exit/at_quick_exit note).
    ${MUSL_DIR}/src/internal/intscan.c
    ${MUSL_DIR}/src/internal/shgetc.c
    ${MUSL_DIR}/src/internal/floatscan.c
    # startup + env + exit chain (the shim retirement core)
    ${MUSL_DIR}/src/env/__libc_start_main.c
    ${MUSL_DIR}/src/env/__environ.c
    ${MUSL_DIR}/src/env/getenv.c
    ${MUSL_DIR}/src/env/setenv.c
    ${MUSL_DIR}/src/env/putenv.c
    ${MUSL_DIR}/src/env/unsetenv.c
    ${MUSL_DIR}/src/env/clearenv.c
    # _Exit.c intentionally NOT here (already in musl_unistd_objs/_so).
    ${MUSL_DIR}/src/exit/exit.c
    ${MUSL_DIR}/src/exit/atexit.c
    # abort_lock.c defines `volatile int __abort_lock[1];` — the lock abort.c
    # (line 19) and _Fork/clone/posix_spawn take around the abort path. v1.1.19
    # inlined this lock; v1.2.6 split it out (src/exit/abort_lock.c), so it is
    # now a separate TU that must be compiled or `abort.c` fails with an
    # undefined reference to __abort_lock. Sits here next to abort.c.
    ${MUSL_DIR}/src/exit/abort_lock.c
    ${MUSL_DIR}/src/exit/abort.c
    ${MUSL_DIR}/src/exit/quick_exit.c
    ${MUSL_DIR}/src/exit/at_quick_exit.c
    # __uflow: musl's src/stdio/__uflow.c (musl_stdio_objs glob) now provides the
    # real definition that satisfies the link-time reference musl's shgetc.c
    # emits. The hand-written user/lib/musl_shim/scan_uflow_stub.c stub (added
    # when stdio was NOT yet migrated and __uflow existed nowhere) is deleted —
    # it would multi-define __uflow against the real one.
)
add_musl_lib(musl_stdlib_objs SOURCES ${MUSL_STDLIB_SOURCES})
