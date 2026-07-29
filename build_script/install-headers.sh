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
#   third_party/musl/include/dlfcn.h  → $DEST/dlfcn.h        (dlopen/dlsym/dlclose/dlerror/dladdr/Dl_info;
#                                              user/include has no dlfcn.h, so musl's ships verbatim)
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

# 3. Replace the shim <unistd.h> / <sys/time.h> / <fcntl.h> with musl's real headers.
#    The repo's user/include/unistd.h, sys/time.h, and fcntl.h are source-tree shims
#    that forward to musl via #include "musl/include/..." (resolved at build time by
#    -I third_party). The published sysroot has no third_party on its search path,
#    so it ships musl's headers directly at the standard paths instead. musl's
#    <unistd.h> pulls <features.h>, <bits/alltypes.h>, <bits/posix.h>; <sys/time.h>
#    pulls <sys/select.h>; <fcntl.h> pulls <bits/fcntl.h> (published above) — all
#    already published. Note: <xos/fcntl.h> is NO LONGER published (moved to
#    the kernel-private kernel/bsd/kfcntl.h during the fcntl header split;
#    fcntl needs no OS-specific extension header — musl's <fcntl.h> plus the
#    kernel M0.4 resolve_dirfd_start fix cover openat fully).
cp "$SRC"/third_party/musl/include/unistd.h     "$DEST/unistd.h"
cp "$SRC"/third_party/musl/include/sys/time.h   "$DEST/sys/time.h"
cp "$SRC"/third_party/musl/include/fcntl.h      "$DEST/fcntl.h"

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
