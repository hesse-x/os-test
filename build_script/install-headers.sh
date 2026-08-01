#!/bin/bash
# install-headers.sh — publish UAPI headers to a sysroot (Linux `make headers_install` equivalent).
#
# Source form == publish form (zero rewrite): the repo's headers already carry the
# include paths the published sysroot must satisfy, e.g. user/include/time.h does
#   #include <xos/time.h>
# and this script copies include/uapi/xos/ verbatim to $DEST/xos/, so that path
# resolves in the sysroot exactly as it does in the source tree (where -Iinclude/uapi
# maps <xos/...> to include/uapi/xos/...). No sed, no path munging.
#
# What gets published:
#   include/uapi/xos/*.h → $DEST/xos/          (UAPI contract headers — shared kernel/user ABI)
#   user/include/*.h     → $DEST/              (POSIX/C standard headers — the libc side)
#   user/include/sys/*.h → $DEST/sys/
#   user/include/bits/*.h → $DEST/bits/         (musl-aligned arch bits: alltypes/posix/syscall/
#                                              stdint. musl's <unistd.h> does #include <bits/alltypes.h>,
#                                              <bits/posix.h>; <stdint.h> does #include <bits/stdint.h>;
#                                              published so the closure resolves.)
#   third_party/musl/include/unistd.h → $DEST/unistd.h   (musl's real <unistd.h> replaces the
#   third_party/musl/include/sys/time.h → $DEST/sys/time.h  source-tree shim at publish time —
#   third_party/musl/include/fcntl.h  → $DEST/fcntl.h        the shim forwards via "musl/include/..."
#   third_party/musl/include/time.h   → $DEST/time.h         which only resolves with -I third_party;
#   third_party/musl/include/dlfcn.h  → $DEST/dlfcn.h        the sysroot has no third_party, so musl's
#                                              real header ships at the standard path instead.
#                                              dlfcn.h is musl verbatim — user/include has no
#                                              dlfcn.h shim (dlopen/dlsym/dlclose/dlerror/dladdr/Dl_info).
#   third_party/musl/include/{pthread,signal,sched}.h → $DEST/  (musl's pthread/signal/sched —
#                                              repo's own were deleted when pthread switched to musl)
#   third_party/musl/arch/x86_64/bits/signal.h → $DEST/bits/signal.h  (<signal.h>'s arch bits;
#                                              self-contained, no collision with repo bits)
#   third_party/musl/include/{stdint,stddef,stdarg,stdbool}.h → $DEST/  (musl freestanding std
#                                              headers — replace the compiler's -isystem freestanding
#                                              dir so the sysroot is self-contained without it.)
#
# What is NOT published (deliberately):
#   utils/             — non-UAPI shared implementation:
#                            macro.h        (generic ALIGN_* macros, also used by kernel)
#                            kvformat.*     (shared printf formatting impl, compiled into both)
#   boot/boot.h          — EFI stub ↔ kernel internal contract (not a user ABI)
#   kernel/**, arch/**   — kernel-internal headers
#
# Usage:
#   ./install-headers.sh [dest]
#   ./install-headers.sh                       # default: build/sysroot/usr/include
#   ./install-headers.sh /path/to/sysroot/usr/include
#
# Verification (no cross toolchain required):
#   ${CC:-clang} -nostdinc -ffreestanding -I<dest> -E -H user/hello.c
#   → every header in <stdio.h>+<time.h>'s closure resolves under <dest> with no
#     "No such file" lines, proving the published tree is self-contained.
set -euo pipefail

SRC="$(cd "$(dirname "$0")/.." && pwd)"
DEST="${1:-$SRC/build/sysroot/usr/include}"

echo "Installing UAPI headers → $DEST"
# Preserve the libc++ header tree (c++/v1/) across the republish: it is a
# co-managed artifact owned by build_libcxx.sh (--cxx), NOT part of this
# script's UAPI/POSIX closure. The blanket rm below would otherwise wipe it,
# and a subsequent default-flow build (./build.sh without --cxx) would then
# run the Mesa cross-build with -stdlib=libc++ but no c++/v1 headers in the
# sysroot → fatal "thread"/"vector"/... "file not found" in the C++ GLSL
# compiler. Move it aside and restore after republishing (no-op if absent,
# i.e. libc++ never built).
libcxx_hdr_backup=""
if [ -d "$DEST/c++" ]; then
    libcxx_hdr_backup="$(mktemp -d)"
    mv "$DEST/c++" "$libcxx_hdr_backup/c++"
fi
rm -rf "$DEST"
mkdir -p "$DEST/sys"

# 1. UAPI contract headers (include/uapi/xos/) — the OS's include/uapi/.
#    Only *.h: the dir also holds CMakeLists.txt (a build file, not an installed header).
#    Plus the user-side OS-extension headers under user/include/xos/ (e.g.
#    xos/unistd_ext.h — OS-specific declarations musl's standard <unistd.h>
#    doesn't carry; consumers #include it explicitly, like musl's own
#    <bits/*.h> companions). These are NOT UAPI (kernel has no copy), but
#    live alongside the UAPI xos/ set in the published sysroot.
mkdir -p "$DEST/xos"
cp "$SRC"/include/uapi/xos/*.h "$DEST/xos/"
cp "$SRC"/user/include/xos/*.h "$DEST/xos/" 2>/dev/null || true

# 1b. Linux-compatible UAPI headers (include/uapi/linux/) — headers under the
#     <linux/...> path that cross-built consumers expect from a Linux-ABI target.
#     e.g. libc++'s <atomic> wait/wake #include <linux/futex.h> and syscall
#     (SYS_futex, FUTEX_WAIT_PRIVATE). The kernel side mirrors Linux's futex(2)
#     ABI, so these are the genuine Linux UAPI constants published verbatim.
mkdir -p "$DEST/linux"
cp "$SRC"/include/uapi/linux/*.h "$DEST/linux/"

# 2. Standard / POSIX headers (user/include/) — the libc side.
#    Top-level *.h → $DEST/; sys/*.h → $DEST/sys/; bits/*.h → $DEST/bits/.
cp    "$SRC"/user/include/*.h  "$DEST/"
cp -r "$SRC"/user/include/sys/. "$DEST/sys/"
mkdir -p "$DEST/bits"
cp    "$SRC"/user/include/bits/*.h "$DEST/bits/"

# 3. Replace the shim <unistd.h> / <sys/time.h> / <fcntl.h> / <time.h> with
#    musl's real headers. The repo's user/include/unistd.h, sys/time.h, fcntl.h,
#    and time.h are source-tree shims that forward to musl via
#    #include "musl/include/..." (resolved at build time by -I third_party).
#    The published sysroot has no third_party on its search path, so it ships
#    musl's headers directly at the standard paths instead. musl's <unistd.h>
#    pulls <features.h>, <bits/alltypes.h>, <bits/posix.h>; <sys/time.h> pulls
#    <sys/select.h>; <fcntl.h> pulls <bits/fcntl.h> (published above); <time.h>
#    pulls <features.h> + <bits/alltypes.h> — all already published. Note: the
#    source-tree <time.h> shim additionally pulls struct timeval via
#    __NEED_struct_timeval (a compat convenience for in-tree sources that
#    historically got timeval from <time.h> via <xos/time.h>); that compat
#    block is NOT in the published sysroot copy — sysroot consumers use musl's
#    real <time.h> and get struct timeval from <sys/time.h>/<sys/select.h>
#    (POSIX). Note: <xos/fcntl.h> is NO LONGER published (moved to the
#    kernel-private kernel/bsd/kfcntl.h during the fcntl header split;
#    fcntl needs no OS-specific extension header — musl's <fcntl.h> plus the
#    kernel M0.4 resolve_dirfd_start fix cover openat fully).
cp "$SRC"/third_party/musl/include/unistd.h     "$DEST/unistd.h"
cp "$SRC"/third_party/musl/include/sys/time.h   "$DEST/sys/time.h"
cp "$SRC"/third_party/musl/include/fcntl.h      "$DEST/fcntl.h"
cp "$SRC"/third_party/musl/include/time.h       "$DEST/time.h"
cp "$SRC"/third_party/musl/include/sys/timerfd.h "$DEST/sys/timerfd.h"

# 3b. musl dlfcn.h — dynamic linking API (dlopen/dlsym/dlclose/dlerror/dladdr/
#     Dl_info). user/include has no dlfcn.h, so publish musl's verbatim. Its
#     only include is <features.h> (already published in step 2), so the closure
#     self-check stays green. The dlinfo symbol is still compiled into libc (see
#     musl_dl_objs).
cp "$SRC"/third_party/musl/include/dlfcn.h      "$DEST/dlfcn.h"

# 3b'. musl link.h + elf.h + bits/link.h — the dynamic-linker/link-map headers
#      (struct link_map, dl_phdr_info, dl_iterate_phdr, ElfW() macros). link.h
#      pulls musl's elf.h (3121 lines, the ELF format spec) + arch/generic
#      bits/link.h. These were once deliberately withheld to avoid dragging the
#      large elf.h into the public ABI before any consumer needed dl_iterate_phdr
#      userspace-side. libunwind (built as a runtimes sibling of libc++ for the
#      Mesa C++ stdlib) is now that consumer — its AddressSpace.hpp #includes
#      <link.h>. musl carries all three verbatim, so publish them. elf.h is
#      self-contained (no further includes); bits/link.h needs only <elf.h> +
#      the already-published <bits/alltypes.h>.
cp "$SRC"/third_party/musl/include/elf.h        "$DEST/elf.h"
cp "$SRC"/third_party/musl/include/link.h       "$DEST/link.h"
cp "$SRC"/third_party/musl/arch/generic/bits/link.h "$DEST/bits/link.h"

# 3b''. musl nl_types.h — POSIX message-catalogue API (catopen/catgets/catclose,
#       nl_catd/nl_item). Tiny and dependency-free (no further includes). libc++
#       #includes <nl_types.h> under _LIBCPP_HAS_CATOPEN, which its <locale> defines
#       for any __unix__ target that isn't BIONIC/newlib/emscripten — musl matches,
#       so the header must be present for libc++ to compile. musl implements the
#       symbols; this was simply not previously published (no prior consumer).
cp "$SRC"/third_party/musl/include/nl_types.h   "$DEST/nl_types.h"

# 3b'''. musl langinfo.h — POSIX locale-langinfo API (nl_langinfo/nl_langinfo_l,
#        the DAY_*/ABDAY_*/MON_*/ABMON_*/AM_STR/PM_STR/codeset constants). libc++'s
#        <locale> #includes <langinfo.h> under _LIBCPP_HAS_CATOPEN/musl. Depends
#        only on <features.h> + <nl_types.h> (published in 3b'') + <bits/alltypes.h>
#        (for locale_t via __NEED_locale_t). musl implements nl_langinfo.
cp "$SRC"/third_party/musl/include/langinfo.h  "$DEST/langinfo.h"

# 3b''''. musl sys/syscall.h — thin wrapper that #include <bits/syscall.h> (the
#        repo's __NR_*/SYS_* table, already published in $DEST/bits/syscall.h).
#        libc++'s <atomic> does `#include <sys/syscall.h>` then syscall(SYS_futex,
#        ...); without the wrapper the <sys/syscall.h> path is absent even though
#        the bits table is present.
cp "$SRC"/third_party/musl/include/sys/syscall.h "$DEST/sys/syscall.h"

# 3b'''''. musl sys/statvfs.h — POSIX statvfs/fstatvfs (struct statvfs +
#         ST_* flags). libc++'s filesystem posix_compat.h #includes it. Depends
#         only on <features.h> + <bits/alltypes.h> (fsblkcnt_t/fsfilcnt_t, both
#         already in the published alltypes). musl implements statvfs.
cp "$SRC"/third_party/musl/include/sys/statvfs.h "$DEST/sys/statvfs.h"

# 3c. musl pthread/signal/sched headers. The repo's user/include/pthread.h,
#     signal.h, sched.h were deleted when pthread switched to musl (pthread.md
#     §8) — these three now come from musl. <signal.h> pulls <bits/signal.h>
#     (musl arch/x86_64/bits/signal.h, self-contained: sigset_t/stack_t/size_t
#     come from the already-published <bits/alltypes.h>); it does not collide
#     with any repo user/include/bits/*.h (alltypes/fcntl/posix/stdint/syscall),
#     so publish it. <pthread.h> needs only <bits/alltypes.h> + <sched.h> +
#     <time.h>; <sched.h> needs only <bits/alltypes.h>. <features.h> (pulled by
#     all three) is the repo's own, already published in step 2 (it is
#     equivalent to musl's).
cp "$SRC"/third_party/musl/include/pthread.h    "$DEST/pthread.h"
cp "$SRC"/third_party/musl/include/threads.h    "$DEST/threads.h"
cp "$SRC"/third_party/musl/include/signal.h     "$DEST/signal.h"
cp "$SRC"/third_party/musl/include/sched.h      "$DEST/sched.h"
cp "$SRC"/third_party/musl/arch/x86_64/bits/signal.h "$DEST/bits/signal.h"

# 3d. musl string/strings headers. The repo's user/include/string.h, strings.h
#     were deleted when string switched to musl (string.md) — these now come
#     from musl. <string.h> pulls <features.h> (repo's own, step 2) + <bits/
#     alltypes.h> (step 2; defines locale_t via __NEED_locale_t at 425-427);
#     under _BSD_SOURCE/_GNU_SOURCE it auto-#includes <strings.h>. <strings.h>
#     pulls <bits/alltypes.h> (locale_t again). Both closures resolve within
#     the sysroot (verified by the per-header self-check below). basename is
#     declared only under _GNU_SOURCE in <string.h>; consumers that don't
#     define it get it from <libgen.h> (published in step 3e).
cp "$SRC"/third_party/musl/include/string.h  "$DEST/string.h"
cp "$SRC"/third_party/musl/include/strings.h "$DEST/strings.h"

# 3e. musl libgen.h. The repo's user/include/libgen.h was deleted when libgen
#     switched to musl (declares basename only; basename.c+dirname.c are in
#     src/misc/, compiled via musl_string_objs). musl's libgen.h declares both
#     basename and dirname — both now have implementations — so publish it
#     verbatim. Consumers (libdrm xf86drm.c, libinput util-files.h, Mesa
#     u_process.c) get the declaration here in the sysroot.
cp "$SRC"/third_party/musl/include/libgen.h "$DEST/libgen.h"

# 3f. musl socket headers. The repo's user/include/sys/socket.h was deleted
#     when socket switched to musl — <sys/socket.h> now comes from musl both at
#     build time (-I third_party/musl/include, MUSL_INCLUDE_FLAGS) and in the
#     published sysroot. <sys/socket.h> pulls <features.h> (step 2) +
#     <bits/alltypes.h> (step 2; defines struct iovec via __NEED_struct_iovec,
#     which musl's socket.h sets) + <bits/socket.h> (struct
#     msghdr/cmsghdr, self-contained). <sys/un.h> pulls <features.h> +
#     <bits/alltypes.h>. The published xos/socket.h user face forwards to
#     <sys/socket.h> + <sys/un.h>, so both must ship here or its closure breaks.
#     <sys/poll.h> stays the repo shim (includes <xos/socket.h> for pollfd).
#     bits/socket.h: musl 1.2.x moved this from arch/x86_64/bits/ to
#     arch/generic/bits/ (x86_64 now shares the generic struct msghdr/cmsghdr
#     layout — same shift as bits/fcntl.h). v1.1.19 had it under arch/x86_64.
cp "$SRC"/third_party/musl/include/sys/socket.h     "$DEST/sys/socket.h"
cp "$SRC"/third_party/musl/include/sys/un.h         "$DEST/sys/un.h"
cp "$SRC"/third_party/musl/arch/generic/bits/socket.h "$DEST/bits/socket.h"

# 3f'. musl syslog.h. The syslog API implementation is compiled from
# src/misc/syslog.c; publish its standalone public header with the socket
# headers it depends on at runtime.
cp "$SRC"/third_party/musl/include/syslog.h "$DEST/syslog.h"

# 3g. musl math/fenv headers. The repo's user/include/math.h was deleted when
#     libm switched to musl (the old header made acos/sqrt/... static inline
#     __builtin_* wrappers; musl's declares them out-of-line, and adds the
#     long-double (*l) set + fenv). <math.h> pulls <features.h> (repo's own,
#     step 2) + <bits/alltypes.h> (step 2; defines float_t/double_t via
#     __NEED_float_t/__NEED_double_t — on x86_64 both are long double). <fenv.h>
#     pulls only <bits/fenv.h> (arch/x86_64), published here. fenv.h is needed
#     because musl src/math/{nearbyint,fma,...}.c #include <fenv.h>; consumers
#     using <fenv.h> get it here too. All three closures resolve within the
#     sysroot (verified by the per-header self-check below).
cp "$SRC"/third_party/musl/include/math.h           "$DEST/math.h"
cp "$SRC"/third_party/musl/include/fenv.h           "$DEST/fenv.h"
cp "$SRC"/third_party/musl/arch/x86_64/bits/fenv.h  "$DEST/bits/fenv.h"

# 3h. musl stdlib.h. The repo's user/include/stdlib.h was deleted when stdlib
#     switched to musl (stdlib.md): the pure-compute subset (abs/labs/llabs/
#     imaxabs/imaxdiv/div/ldiv/lldiv, atoi/atol/atoll, strtol/strtoul/strtoll/
#     strtoull/strtoimax/strtoumax, strtod/strtof/strtold, atof, qsort/bsearch,
#     rand/srand/rand_r) AND the startup/env/exit chain (__libc_start_main,
#     environ/getenv/setenv/putenv/unsetenv/clearenv, exit/atexit/abort/
#     quick_exit/at_quick_exit, _Exit) all come from musl upstream now.
#     musl's stdlib.h typedefs div_t/ldiv_t/lldiv_t inline and pulls
#     <features.h> (repo's own, step 2) + <bits/alltypes.h> (step 2;
#     __NEED_size_t/__NEED_wchar_t). imaxdiv_t/strtoimax/strtoumax are declared
#     in <inttypes.h> (musl), published below. RAND_MAX is 0x7fffffff (31-bit)
#     in musl vs the old repo 32767 — more correct; the parked subset
#     (mkstemp/mktemp/realpath/mknod/chmod/remove/getline/fscanf/scanf/
#     getpagesize/sysconf) is declared in stdio.h / sys/stat.h / xos/unistd_ext.h,
#     NOT stdlib.h, so switching drops no parked declaration. arc4random_*
#     (repo-only decls, 0 callers, no definition) are dropped with the switch.
cp "$SRC"/third_party/musl/include/stdlib.h "$DEST/stdlib.h"

# 3i. musl inttypes.h. Declares imaxdiv_t / strtoimax / strtoumax / imaxabs /
#     imaxdiv (now all provided by musl_stdlib_objs's strtol.c + src/stdlib/
#     {imaxabs,imaxdiv}.c), which the old repo stdlib.h used to declare.
#     musl's <inttypes.h> pulls <features.h> (step 2) + <bits/alltypes.h>
#     (step 2; __NEED_imaxdiv_t/__NEED_intmax_t/__NEED_uintmax_t) + <stdint.h>
#     (step 4). Closure self-check below verifies it.
cp "$SRC"/third_party/musl/include/inttypes.h "$DEST/inttypes.h"

# 3j. musl sys/mman.h. The repo's user/include/sys/mman.h was a source-tree
#     shim forwarding to musl via #include "musl/include/sys/mman.h" (resolved
#     at build time by -I third_party). The published sysroot has no third_party
#     on its search path, so publish musl's real <sys/mman.h> here. musl's
#     <sys/mman.h> pulls <bits/alltypes.h> (step 2) + <bits/mman.h>; the repo's
#     user/include/bits/mman.h is already published in step 2 (it supplies
#     MAP_32BIT + the OS-specific MAP_FIXED_NOREPLACE/MAP_SHARED_VALIDATE/
#     MAP_GROWSUP/PROT_SEM/MFD_* musl's generic header lacks, with static_assert
#     parity against include/uapi/xos/mman.h — published in step 1 as $DEST/xos/
#     mman.h, so the #ifdef __XOS_MMAN_UAPI_AVAILABLE asserts resolve).
#
#     The shim's ONE addition over musl — the memfd_create declaration (musl
#     src/mman has no memfd_create; the wrapper is retained in
#     user/lib/sys_process.cc) — is appended to the published copy so consumers
#     of <sys/mman.h> see it. Done by post-processing the file rather than
#     shipping a divergent copy, keeping musl's header the source of truth for
#     everything else.
cp "$SRC"/third_party/musl/include/sys/mman.h "$DEST/sys/mman.h"
cat >> "$DEST/sys/mman.h" <<'__MMAN_EXT__'

/* OS-specific extension appended by install-headers.sh (mman §3j). musl's
 * <sys/mman.h> has no memfd_create (it lives in musl src/linux/, not adopted);
 * this OS exposes it under <sys/mman.h> (matching glibc's _GNU_SOURCE placement).
 * Defined in user/lib/sys_process.cc, routes to SYS_MEMFD_CREATE (319).
 * MFD_CLOEXEC/MFD_ALLOW_SEALING come from <bits/mman.h> above. */
#include <sys/cdefs.h>
#ifdef __cplusplus
extern "C" {
#endif
int memfd_create(const char *, unsigned int);
#ifdef __cplusplus
}
#endif
__MMAN_EXT__

# 3k. musl stdio.h. The repo's user/include/stdio.h was a full hand-written
#     header (custom struct _FILE + _F_* flags + LIBC_EXPORT decls); the stdio
#     → musl migration (stdio.md) reduced it to a source-tree shim forwarding to
#     musl via #include "musl/include/stdio.h" (resolved at build time by
#     -I third_party). The published sysroot has no third_party on its search
#     path, so publish musl's real <stdio.h> here. musl's <stdio.h> pulls
#     <bits/alltypes.h> (step 2; __NEED_FILE → FILE = struct _IO_FILE, plus
#     size_t/off_t/ssize_t/va_list). This OS has NO declaration musl's
#     <stdio.h> lacks (unlike the mman module's memfd_create), so — unlike §3j —
#     there is no heredoc extension to append: the verbatim copy is complete.
#     Closure self-check below verifies the <stdio.h> → <bits/alltypes.h> chain.
cp "$SRC"/third_party/musl/include/stdio.h "$DEST/stdio.h"
cp "$SRC"/third_party/musl/include/spawn.h "$DEST/spawn.h"

# 3l. musl wchar/wctype/uchar headers. user/include has no source-tree shim for
#     these three (zero in-tree consumers — same publish-only pattern as
#     dlfcn.h §3b). All three closures need <bits/alltypes.h> (step 2; provides
#     wchar_t/wint_t/wctype_t/mbstate_t/locale_t) + <features.h>. <wchar.h> also
#     pulls FILE (alltypes __NEED_FILE → struct _IO_FILE) + struct tm (forward
#     declared). No new bits/ header required. Closure self-check below covers
#     all three.
cp "$SRC"/third_party/musl/include/wchar.h  "$DEST/wchar.h"
cp "$SRC"/third_party/musl/include/wctype.h "$DEST/wctype.h"
cp "$SRC"/third_party/musl/include/uchar.h  "$DEST/uchar.h"

# 3m. musl ctype.h. The repo's user/include/ctype.h (declaration-only shim,
#     15 LIBC_EXPORT prototypes) was deleted when narrow ctype switched to musl
#     (musl_worklist ctype module). musl's <ctype.h> is a strict superset (the
#     15 plain prototypes + inline-macro optimizations for
#     isalpha/isdigit/islower/isupper/isprint/isgraph/isspace + the 14 _l
#     locale_t variants + isascii/toascii under the _POSIX/_XOPEN gate). It
#     pulls only <features.h> (repo's own, step 2) + <bits/alltypes.h> (step 2;
#     locale_t via __NEED_locale_t). No OS-specific declaration to append, so
#     the verbatim copy is complete (same as stdio §3k). Closure self-check
#     below covers it.
cp "$SRC"/third_party/musl/include/ctype.h "$DEST/ctype.h"

# 3n. musl errno.h + bits/errno.h. The repo's user/include/errno.h was a
#     hand-written shim (#include <xos/errno.h> + a LIBC_EXPORT __errno_location
#     decl + the errno macro); the errno → musl migration reduced it to a
#     source-tree shim forwarding to musl via #include "musl/include/errno.h".
#     The published sysroot has no third_party on its search path, so publish
#     musl's real <errno.h> here. musl's <errno.h> #includes <bits/errno.h>
#     (the E*-macro table) — there is no user/include/bits/errno.h, so publish
#     musl's arch/generic copy (the same table include/uapi/xos/errno.h mirrors
#     1:1; the kernel keeps xos/errno.h as its frozen UAPI source). <errno.h>
#     also pulls <features.h> (repo's own, step 2). Under _GNU_SOURCE it
#     declares program_invocation_short_name/name — both ARE defined (musl
#     libc.c weak_alias to __progname/__progname_full, set by
#     __libc_start_main), so no undefined-reference risk. Closure self-check
#     below covers the <errno.h> → <bits/errno.h> chain.
cp "$SRC"/third_party/musl/include/errno.h           "$DEST/errno.h"
cp "$SRC"/third_party/musl/arch/generic/bits/errno.h "$DEST/bits/errno.h"

# 3o. musl setjmp.h + bits/setjmp.h. The repo's user/include/setjmp.h (custom
#     `typedef long long jmp_buf[8]` + LIBC_EXPORT prototypes) was deleted when
#     setjmp switched to musl (musl_worklist setjmp module); musl's <setjmp.h>
#     is the published replacement. musl's jmp_buf is `struct __jmp_buf_tag {
#     __jmp_buf __jb; unsigned long __fl; unsigned long __ss[128/sizeof(long)];
#     }[1]` where __jb comes from <bits/setjmp.h> (`unsigned long[8]` on x86_64,
#     from arch/x86_64/bits/setjmp.h). The setjmp/longjmp asm only touches the
#     first 64 bytes (__jb[0..7]) so this is behavior-identical to the deleted
#     repo asm (same 8 callee-saved regs). musl's <setjmp.h> additionally
#     declares sigjmp_buf/sigsetjmp/siglongjmp (gated _POSIX/_XOPEN) — sigsetjmp
#     is declare-only (no x86_64 musl impl, 0 callers; todo.md), siglongjmp is
#     built in musl_signal_objs — and marks setjmp with __returns_twice__.
#     <setjmp.h> pulls <features.h> (repo's own, step 2) + <bits/setjmp.h>
#     (published here, arch/x86_64 — self-contained). No OS-specific
#     declaration to append, so the verbatim copy is complete (same as stdio
#     §3k / ctype §3m). Closure self-check below covers it.
cp "$SRC"/third_party/musl/include/setjmp.h           "$DEST/setjmp.h"
cp "$SRC"/third_party/musl/arch/x86_64/bits/setjmp.h  "$DEST/bits/setjmp.h"

# 3p. musl locale.h. The repo had NO user/include/locale.h (the locale tier was
#     declare-only via other headers' locale_t until this migration), so publish
#     musl's <locale.h> verbatim — no shim to replace. It declares the 6 POSIX
#     locale managers (setlocale/localeconv/newlocale/duplocale/freelocale/
#     uselocale) unconditionally, plus the LC_* category constants and struct
#     lconv; under _POSIX/_XOPEN/_GNU_SOURCE it also declares duplocale/
#     freelocale/newlocale/uselocale (locale_t) + the LC_*_MASK /
#     LC_GLOBAL_LOCALE macros. locale_t itself comes from <bits/alltypes.h>
#     (step 2, __NEED_locale_t — already pulled by <ctype.h>/<wchar.h>).
#     <locale.h> pulls <features.h> (repo's own, step 2) + <bits/alltypes.h>
#     (step 2); no bits/locale.h exists (LC_* are inline #defines here, unlike
#     errno/signal whose arch bits table is a separate include). All 6 managers
#     + strcoll/strxfrm/wcscoll/wcsxfrm (+_l) are now built (musl_locale_objs)
#     and exported (libc.map), so the declarations are not declare-only. No
#     OS-specific declaration to append (unlike mman's memfd_create), so the
#     verbatim copy is complete (same as stdio §3k / ctype §3m / setjmp §3o).
#     Closure self-check below covers it.
cp "$SRC"/third_party/musl/include/locale.h "$DEST/locale.h"

# 3q. POSIX regex and fnmatch. Both implementations are built from musl's
#     src/regex module and exported by libc. No OS-specific ABI definitions are
#     involved; regoff_t comes from the already-published bits/alltypes.h.
cp "$SRC"/third_party/musl/include/regex.h   "$DEST/regex.h"
cp "$SRC"/third_party/musl/include/fnmatch.h "$DEST/fnmatch.h"

# 3r. dirent/resource now use musl headers verbatim. Their arch-independent
#     bits headers define the public dirent layout and resource constants.
cp "$SRC"/third_party/musl/include/dirent.h "$DEST/dirent.h"
cp "$SRC"/third_party/musl/arch/generic/bits/dirent.h "$DEST/bits/dirent.h"
cp "$SRC"/third_party/musl/include/sys/resource.h "$DEST/sys/resource.h"
cp "$SRC"/third_party/musl/arch/generic/bits/resource.h "$DEST/bits/resource.h"

# 4. musl freestanding std headers (stdint/stddef/stdarg/stdbool) — replace the
#    compiler's -isystem freestanding dir. The published sysroot must be usable
#    with -nostdinc and NO -isystem (the Mesa milestone consumes it via -isysroot),
#    so these resolve here rather than from the toolchain's bundled std*.h.
#    <bits/alltypes.h> + <bits/stdint.h> (published in step 2) provide the types
#    these pull via #include <bits/...>.
for h in stdint.h stddef.h stdarg.h stdbool.h; do
  cp "$SRC"/third_party/musl/include/$h "$DEST/$h"
done

# 4b. Publish the FULL musl include tree (do-not-clobber) so the sysroot is a
#     complete POSIX/C header set for cross builds (Mesa pulls <sys/shm.h>,
#     <sys/ipc.h>, <sys/stat.h>, <sys/uio.h>, <sys/utsname.h>, <netinet/*>,
#     <arpa/*>, <net/*>, <grp.h>, <pwd.h>, <glob.h>, <regex.h>, ... — far more
#     than the curated subset above). Steps 1-4 + the §3 replacements have already
#     placed every OS-specific header (features.h, errno.h, bits/*, xos/*, the
#     musl §3 replacements for unistd/stdio/stdlib/time/fcntl/socket/mman/math/...),
#     so `cp -rn` (no-clobber) fills ONLY the gaps musl provides — it never
#     overwrites an OS-owned file. This mirrors the build-time search order
#     (MUSL_INCLUDE_FLAGS: musl/include → arch/x86_64 → arch/generic, with
#     user/include first): arch/x86_64/bits wins over arch/generic/bits, and our
#     user/include/bits already on disk wins over both.
#     *.in templates (alltypes.h.in, syscall.h.in) are skipped — alltypes.h is
#     published in step 2, syscall.h in step 2; the .in forms need musl's
#     configure step and are not usable as headers.
cp -rn "$SRC"/third_party/musl/include/. "$DEST/"
cp -rn "$SRC"/third_party/musl/arch/x86_64/bits/. "$DEST/bits/"
cp -rn "$SRC"/third_party/musl/arch/generic/bits/. "$DEST/bits/"
find "$DEST" -name '*.in' -delete

# 4c. Mesa POSIX-completeness fixes for repo shims that shadow musl's fuller
#     headers. Step 2 published the repo's hand-written user/include/sys/*.h and
#     user/include/limits.h, and step 4b's `cp -rn` (no-clobber) could NOT
#     overwrite them — so several shims that are thinner than musl's POSIX
#     versions win in the sysroot, dropping types/macros Mesa needs:
#       - sys/select.h shim has no __NEED_struct_timeval → struct timeval never
#         defined. musl's <sys/time.h> (published in §3) #includes <sys/select.h>
#         expecting it to pull struct timeval via bits/alltypes.h; the shim breaks
#         that chain, so os_time.c fails "field has incomplete type 'struct timeval'".
#       - sys/stat.h shim has st_atim/st_mtim/st_ctim but NOT the st_atime/st_mtime/
#         st_ctime macro aliases (musl defines them) → disk_cache_os.c fails
#         "no member named 'st_atime'".
#       - sys/eventfd.h shim lacks eventfd_t/eventfd_read/eventfd_write →
#         os_file_notify.c fails "use of undeclared identifier 'eventfd_t'".
#       - limits.h shim (LLVM-libc-derived C23 set) lacks NAME_MAX and the
#         runtime-limits block → os_file_notify.c fails "undeclared 'NAME_MAX'".
#     The sysroot is consumed ONLY by the Mesa cross-build (the OS itself compiles
#     against the source tree via -I user/include + -I third_party/musl; the sysroot
#     is also copied verbatim into the image's /usr/include by mkdisk.sh, where
#     POSIX-complete headers are what native builds want). So these edits affect
#     Mesa/the image, not the OS's own build.
#
#     Two policies by header, matching whether the shim carries OS-specific content:
#       A) REPLACE with musl's version — for shims that are pure thin POSIX
#          wrappers with NO OS kernel ABI. sys/select.h (its `select` decl points
#          at an unexported symbol anyway — the OS implements select via a poll
#          fallback; Mesa only needs the types/macros) and sys/eventfd.h (the shim
#          is just EFD_* + eventfd decl; musl's adds eventfd_t/read/write + uses
#          O_CLOEXEC/O_NONBLOCK from <fcntl.h>, already published). musl's versions
#          declare select/eventfd the same way the OS libc expects.
#       B) APPEND musl's missing fragment — for shims that carry OS kernel ABI we
#          must keep. sys/stat.h keeps the OS struct stat (mirrors struct kstat,
#          static_assert-locked to the kernel layout) and gains the st_atime/
#          st_mtime/st_ctime aliases appended. limits.h keeps the repo's C23 macro
#          set and gains musl's runtime-limits block (NAME_MAX et al.) appended
#          under the same _POSIX_SOURCE/_GNU_SOURCE guard musl uses.

# 4c-A: replace thin POSIX-wrapper shims with musl's full versions.
cp "$SRC"/third_party/musl/include/sys/select.h "$DEST/sys/select.h"
cp "$SRC"/third_party/musl/include/sys/eventfd.h "$DEST/sys/eventfd.h"

# 4c-B1: sys/stat.h — append the st_atime/st_mtime/st_ctime macro aliases musl
#        defines (st_atim/st_mtim/st_ctim are the OS struct's timespec fields;
#        the aliases let consumers use the POSIX st_atime spelling Mesa expects).
#        Insert before the include-guard #endif so the macros sit in the public
#        scope; guarded so re-inclusion is a no-op.
if grep -q '^struct stat {' "$DEST/sys/stat.h"; then
  cat >> "$DEST/sys/stat.h" <<'__STAT_POSIX_ALIASES__'

/* POSIX st_atime/st_mtime/st_ctime aliases (appended by install-headers.sh §4c-B1).
 * musl's <sys/stat.h> defines these as st_atim.tv_sec etc.; the repo shim keeps
 * the OS struct stat (with st_atim/st_mtim/st_ctim timespec fields) but omitted
 * the second-level aliases. Mesa's disk_cache_os.c uses st_atime/st_mtime. */
#ifndef _XOS_STAT_TIME_ALIASES
#define _XOS_STAT_TIME_ALIASES
#define st_atime st_atim.tv_sec
#define st_mtime st_mtim.tv_sec
#define st_ctime st_ctim.tv_sec
#endif
__STAT_POSIX_ALIASES__
fi

# 4c-B2: limits.h — append musl's runtime-limits block (NAME_MAX, SYMLINK_MAX,
#        PATH_MAX-if-not-set, NZERO, NGROUPS_MAX, ARG_MAX, IOV_MAX, SYMLOOP_MAX,
#        TZNAME_MAX, TTY_NAME_MAX, HOST_NAME_MAX, ...). The repo shim only has
#        the C23 macro set + POSIX minimums (_POSIX_NAME_MAX etc.), missing the
#        actual NAME_MAX=255 Mesa's os_file_notify.c sizes its inotify buffer with.
#        Guarded by the same _POSIX_SOURCE/_GNU_SOURCE/_BSD_SOURCE feature macro
#        musl uses, and each #define is #ifndef-wrapped so the repo's existing
#        values (e.g. PATH_MAX=4096) win. Appended before the include-guard #endif.
if grep -q '_LIMITS_H' "$DEST/limits.h"; then
  cat >> "$DEST/limits.h" <<'__LIMITS_RUNTIME__'

/* Runtime limits (appended by install-headers.sh §4c-B2). musl's <limits.h>
 * defines NAME_MAX/SYMLINK_MAX/NZERO/NGROUPS_MAX/ARG_MAX/IOV_MAX/SYMLOOP_MAX/
 * TZNAME_MAX/TTY_NAME_MAX/HOST_NAME_MAX/... under the POSIX feature guard; the
 * repo's LLVM-libc-derived shim only carries the C23 set + _POSIX_* minimums, so
 * Mesa (os_file_notify.c uses NAME_MAX) and other POSIX consumers miss them.
 * Each macro is #ifndef-wrapped so the repo's own values (PATH_MAX etc.) win. */
#if defined(_POSIX_SOURCE) || defined(_POSIX_C_SOURCE) \
 || defined(_XOPEN_SOURCE) || defined(_GNU_SOURCE) || defined(_BSD_SOURCE)
#ifndef NAME_MAX
#define NAME_MAX 255
#endif
#ifndef SYMLINK_MAX
#define SYMLINK_MAX 255
#endif
#ifndef NZERO
#define NZERO 20
#endif
#ifndef NGROUPS_MAX
#define NGROUPS_MAX 32
#endif
#ifndef ARG_MAX
#define ARG_MAX 131072
#endif
#ifndef IOV_MAX
#define IOV_MAX 1024
#endif
#ifndef SYMLOOP_MAX
#define SYMLOOP_MAX 40
#endif
#ifndef TZNAME_MAX
#define TZNAME_MAX 6
#endif
#ifndef TTY_NAME_MAX
#define TTY_NAME_MAX 32
#endif
#ifndef HOST_NAME_MAX
#define HOST_NAME_MAX 255
#endif
#ifndef LOGIN_NAME_MAX
#define LOGIN_NAME_MAX 256
#endif
#ifndef PIPE_BUF
#define PIPE_BUF 4096
#endif
#ifndef LINE_MAX
#define LINE_MAX 4096
#endif
#ifndef RE_DUP_MAX
#define RE_DUP_MAX 255
#endif
#endif
__LIMITS_RUNTIME__
fi

# 5. Third-party library headers (libdrm / libexpat / zlib / libffi) + the linux/ &
#    asm/ UAPI shims they pull. These are Mesa cross-build deps: Mesa's meson resolves
#    libdrm/expat/zlib/ffi via pkg-config (gen-pkgconfig.sh) and compiles against the
#    sysroot, so each lib's public header must ship here. The libdrm layout mirrors the
#    upstream install (third_party/drm/meson.build:302-314): the userspace interface
#    headers (xf86drm.h/xf86drmMode.h/libsync.h) at the include TOP LEVEL, and the
#    kernel UAPI headers (drm.h/drm_mode.h/drm_fourcc.h/virtgpu_drm.h/...) under a
#    libdrm/ subdir. xf86drm.h does `#include <drm.h>` (top-level style), so
#    libdrm.pc's Cflags adds -I${includedir}/libdrm (gen-pkgconfig.sh) to resolve it.
#    libdrm's xf86drm.h also does #include <linux/types.h> + <asm/ioctl.h>; Mesa's own
#    drm-uapi/virtgpu_drm.h pulls the same. The repo's include/uapi/linux/types.h and
#    include/uapi/asm/ioctl.h are the shims that map those to freestanding stdint +
#    xos/ioctl.h (xos/ already published in step 1) — publish them at the standard
#    <linux/>/<asm/> paths so the sysroot resolves them with -isysroot alone.
mkdir -p "$DEST/drm" "$DEST/libdrm" "$DEST/linux" "$DEST/asm"
cp "$SRC"/include/uapi/linux/types.h "$DEST/linux/types.h"
cp "$SRC"/include/uapi/asm/ioctl.h   "$DEST/asm/ioctl.h"
# Kernel UAPI headers → libdrm/ (upstream layout). Also keep the drm/ copy for any
# consumer using #include <drm/...> (our own libdrm.so build uses the drm/ path).
cp "$SRC"/third_party/drm/include/drm/*.h "$DEST/libdrm/"
cp "$SRC"/third_party/drm/include/drm/*.h "$DEST/drm/"
# libdrm userspace interface headers → include top level (matches upstream install).
cp "$SRC"/third_party/drm/xf86drm.h    "$DEST/xf86drm.h"
cp "$SRC"/third_party/drm/xf86drmMode.h "$DEST/xf86drmMode.h"
cp "$SRC"/third_party/drm/libsync.h     "$DEST/libsync.h"
cp "$SRC"/third_party/libexpat/expat/lib/expat.h          "$DEST/expat.h"
cp "$SRC"/third_party/libexpat/expat/lib/expat_external.h "$DEST/expat_external.h"
cp "$SRC"/third_party/zlib/zlib.h  "$DEST/zlib.h"
cp "$SRC"/third_party/zlib/zconf.h "$DEST/zconf.h"
# libffi's public headers are generated by libffi.cmake into $BUILD (ffi.h from
# ffi.h.in @-substitution, ffitarget.h + fficonfig.h COPYONLY). Publish them only if
# the build has produced them (install-headers may run before libffi is built in a
# partial build); absent ffi.h is fine for non-ffi consumers.
BUILD_DIR="${BUILD:-$SRC/build}"
for fh in ffi.h ffitarget.h fficonfig.h; do
  if [ -f "$BUILD_DIR/$fh" ]; then
    cp "$BUILD_DIR/$fh" "$DEST/$fh"
  fi
done

echo "Installed tree:"
( cd "$DEST" && find . -type f | sort | sed 's/^\.\//  /' )

# Precheck: the static user/include/bits set (alltypes.h + stdint.h) must have
# been published in step 2 — musl's freestanding std headers #include <bits/...>
# and the closure below resolves only if those are in place.
[ -f "$DEST/bits/alltypes.h" ] || { echo "FAIL: $DEST/bits/alltypes.h missing (user/include/bits not published)." >&2; exit 1; }

# Self-test: prove the published tree is self-contained by preprocessing hello.c
# against ONLY the sysroot (-nostdinc + -I$DEST), with NO -isystem freestanding
# fallback. The sysroot ships musl's stdint/stddef/stdarg/stdbool + bits/alltypes
# + bits/stdint itself, so every header in <stdio.h>+<time.h>'s closure must
# resolve under $DEST. -H lists every header opened; a fatal "No such file" means
# a repo-owned closure broke (a real bug). CC selects the compiler (default clang).
echo
echo "Closure check: preprocessing user/hello.c against $DEST ..."
CC="${CC:-clang}"
if $CC -nostdinc -ffreestanding "-I$DEST" -E -H "$SRC/user/hello.c" >/dev/null 2>/tmp/closure.log; then
  # -H emits one line per header opened (indented by depth). Every opened header
  # is now sysroot-owned (no -isystem fallback), so the count is the full closure.
  repo_opened=$(wc -l < /tmp/closure.log || true)
  echo "OK: hello.c closure resolved ($repo_opened sysroot-owned header opens, 0 missing)."
else
  echo "FAIL: hello.c closure broken against sysroot:" >&2
  grep -E 'fatal|error' /tmp/closure.log >&2 || cat /tmp/closure.log >&2
  exit 1
fi

# Stronger regression guard: every published header's own include closure must also
# resolve under the sysroot — not just hello's path. Catches breaks like pthread.h
# → xos/signal.h that hello.c never exercises. Same pure -nostdinc + -I$DEST (no
# -isystem): the sysroot must stand alone.
#
# Scope: only the OS-owned closure (musl/repo/xos/bits/sys headers we maintain).
# The vendored third-party set published in step 5 (drm/ + libdrm/ + linux/ + asm/
# shims + xf86drm.h/xf86drmMode.h/libsync.h + expat.h/zlib.h/zconf.h/ffi*.h) is
# EXCLUDED — those are Mesa cross-build deps, not our closure to guarantee, and
# several are intentionally not self-contained (drm/via_drm.h pulls a vendor-only
# via_drmclient.h; xf86drm.h pulls <atomic_ops.h>/<sys/ioccom.h> absent from musl;
# ffitarget.h #errors when included directly — it is meant to be pulled via ffi.h
# only). Their resolution is validated by the Mesa meson build itself, not by this scan.
echo "Scanning every OS-owned published header for self-contained closure ..."
failed=0
while IFS= read -r h; do
  rel="${h#"$DEST/"}"
  printf '#include <%s>\n' "$rel" > /tmp/hdr_probe.c
  if ! $CC -nostdinc -ffreestanding "-I$DEST" -E -H /tmp/hdr_probe.c >/dev/null 2>/tmp/hdr.log; then
    echo "  FAIL: <$rel> closure broken:" >&2
    grep -E 'fatal|error' /tmp/hdr.log | head -3 | sed 's/^/      /' >&2
    failed=$((failed+1))
  fi
done < <(find "$DEST" -name '*.h' -type f \
  ! -path "$DEST/drm/*" \
  ! -path "$DEST/libdrm/*" \
  ! -path "$DEST/linux/*" \
  ! -path "$DEST/asm/*" \
  ! -name 'expat.h' ! -name 'expat_external.h' \
  ! -name 'xf86drm.h' ! -name 'xf86drmMode.h' ! -name 'libsync.h' \
  ! -name 'zlib.h' ! -name 'zconf.h' \
  ! -name 'ffi.h' ! -name 'ffitarget.h' ! -name 'fficonfig.h' \
  ! -name 'kd.h' ! -name 'soundcard.h' ! -name 'vt.h')
# sys/kd.h, sys/soundcard.h, sys/vt.h (from the full musl tree, step 4b) each
# #include <linux/{kd,soundcard,vt}.h> — kernel console/sound UAPI we don't ship
# (no kernel headers_install in this sysroot). Excluded like the third-party set
# above: not our closure to guarantee, and unused by Mesa EGL/GLES/softpipe.
if [ "$failed" -ne 0 ]; then
  echo "FAIL: $failed published header(s) had broken closure." >&2
  exit 1
fi
scanned=$(find "$DEST" -name '*.h' -type f \
  ! -path "$DEST/drm/*" \
  ! -path "$DEST/libdrm/*" \
  ! -path "$DEST/linux/*" \
  ! -path "$DEST/asm/*" \
  ! -name 'expat.h' ! -name 'expat_external.h' \
  ! -name 'xf86drm.h' ! -name 'xf86drmMode.h' ! -name 'libsync.h' \
  ! -name 'zlib.h' ! -name 'zconf.h' \
  ! -name 'ffi.h' ! -name 'ffitarget.h' ! -name 'fficonfig.h' \
  ! -name 'kd.h' ! -name 'soundcard.h' ! -name 'vt.h' | wc -l)
total=$(find "$DEST" -name '*.h' -type f | wc -l)
echo "OK: all $scanned OS-owned headers have self-contained closure ($total total published, third-party set excluded)."

# Restore the libc++ header tree moved aside before the republish (see top of
# script). Kept here so it survives even the closure-scan block above.
if [ -n "$libcxx_hdr_backup" ] && [ -d "$libcxx_hdr_backup/c++" ]; then
    mv "$libcxx_hdr_backup/c++" "$DEST/c++"
    rmdir "$libcxx_hdr_backup" 2>/dev/null || true
fi

echo "Done. Sysroot ready at $DEST"
