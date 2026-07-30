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
#     self-check stays green. dlinfo/dl_iterate_phdr (struct link_map /
#     dl_phdr_info in <link.h>) are deliberately NOT published: link.h pulls
#     musl's elf.h (3121 lines) + arch/generic bits/link.h into the public ABI,
#     a larger commitment left for when dlinfo/dl_iterate_phdr are exercised
#     userspace-side. The dlinfo symbol is still compiled into libc (see
#     musl_dl_objs); only the <link.h> header is absent.
cp "$SRC"/third_party/musl/include/dlfcn.h      "$DEST/dlfcn.h"

# 3c. musl pthread/signal/sched headers. The repo's user/include/pthread.h,
#     signal.h, sched.h were deleted when pthread switched to musl (pthread.md
#     §八) — these three now come from musl. <signal.h> pulls <bits/signal.h>
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

# 4. musl freestanding std headers (stdint/stddef/stdarg/stdbool) — replace the
#    compiler's -isystem freestanding dir. The published sysroot must be usable
#    with -nostdinc and NO -isystem (the Mesa milestone consumes it via -isysroot),
#    so these resolve here rather than from the toolchain's bundled std*.h.
#    <bits/alltypes.h> + <bits/stdint.h> (published in step 2) provide the types
#    these pull via #include <bits/...>.
for h in stdint.h stddef.h stdarg.h stdbool.h; do
  cp "$SRC"/third_party/musl/include/$h "$DEST/$h"
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
echo "Scanning every published header for self-contained closure ..."
failed=0
while IFS= read -r h; do
  rel="${h#"$DEST/"}"
  printf '#include <%s>\n' "$rel" > /tmp/hdr_probe.c
  if ! $CC -nostdinc -ffreestanding "-I$DEST" -E -H /tmp/hdr_probe.c >/dev/null 2>/tmp/hdr.log; then
    echo "  FAIL: <$rel> closure broken:" >&2
    grep -E 'fatal|error' /tmp/hdr.log | head -3 | sed 's/^/      /' >&2
    failed=$((failed+1))
  fi
done < <(find "$DEST" -name '*.h' -type f)
if [ "$failed" -ne 0 ]; then
  echo "FAIL: $failed published header(s) had broken closure." >&2
  exit 1
fi
total=$(find "$DEST" -name '*.h' -type f | wc -l)
echo "OK: all $total published headers have self-contained closure."
echo "Done. Sysroot ready at $DEST"
