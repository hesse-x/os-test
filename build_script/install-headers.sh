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
#   user/include/bits/*.h → $DEST/bits/         (musl-aligned arch bits: alltypes/posix/syscall.
#                                              musl's <unistd.h> does #include <bits/alltypes.h>,
#                                              <bits/posix.h>; published so the closure resolves.)
#   third_party/musl/include/unistd.h → $DEST/unistd.h   (musl's real <unistd.h> replaces the
#   third_party/musl/include/sys/time.h → $DEST/sys/time.h  source-tree shim at publish time —
#                                              the shim forwards via "musl/include/..." which
#                                              only resolves with -I third_party at build time.)
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

echo "Installed tree:"
( cd "$DEST" && find . -type f | sort | sed 's/^\.\//  /' )

# Self-test: prove the published tree is self-contained by preprocessing hello.c
# against ONLY the sysroot (-nostdinc) plus the toolchain's freestanding headers
# (-isystem, which supplies stdint.h/stddef.h/stdarg.h — the same headers a real
# --target=x86_64-xos cross gcc ships). -I$DEST makes the sysroot the include root,
# exactly as -isysroot would expose /usr/include. -H lists every header opened; a
# fatal "No such file" means the repo-owned closure broke (a real bug).
#
# CC selects the compiler (default clang, matching build.sh's default). The
# freestanding dir differs by compiler (gcc → …/gcc/<v>/include, clang →
# …/clang/<v>/include), so $FREESTANDING is queried per-compiler and the
# closure filter excludes that exact path instead of a hardcoded "/gcc/".
echo
echo "Closure check: preprocessing user/hello.c against $DEST ..."
CC="${CC:-clang}"
FREESTANDING="$($CC -print-file-name=include)"
if $CC -nostdinc -ffreestanding -isystem "$FREESTANDING" "-I$DEST" -E -H "$SRC/user/hello.c" >/dev/null 2>/tmp/closure.log; then
  # -H emits one line per header opened (indented by depth). Lines from the toolchain's
  # freestanding dir are excluded by exact path match; everything else is a sysroot-owned header.
  repo_opened=$(grep -vcF "$FREESTANDING" /tmp/closure.log || true)
  echo "OK: hello.c closure resolved ($repo_opened sysroot-owned header opens, 0 missing)."
else
  echo "FAIL: hello.c closure broken against sysroot:" >&2
  grep -E 'fatal|error' /tmp/closure.log >&2 || cat /tmp/closure.log >&2
  exit 1
fi

# Stronger regression guard: every published header's own include closure must also
# resolve under the sysroot — not just hello's path. Catches breaks like pthread.h
# → xos/signal.h that hello.c never exercises.
echo "Scanning every published header for self-contained closure ..."
failed=0
while IFS= read -r h; do
  rel="${h#"$DEST/"}"
  printf '#include <%s>\n' "$rel" > /tmp/hdr_probe.c
  if ! $CC -nostdinc -ffreestanding -isystem "$FREESTANDING" "-I$DEST" -E -H /tmp/hdr_probe.c >/dev/null 2>/tmp/hdr.log; then
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
