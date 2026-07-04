#!/bin/bash
# install-headers.sh — publish UAPI headers to a sysroot (Linux `make headers_install` equivalent).
#
# Source form == publish form (zero rewrite): the repo's headers already carry the
# include paths the published sysroot must satisfy, e.g. user/include/time.h does
#   #include "xos/time.h"
# and this script copies include/uapi/xos/ verbatim to $DEST/xos/, so that path
# resolves in the sysroot exactly as it does in the source tree (where -Iinclude/uapi
# maps "xos/..." to include/uapi/xos/...). No sed, no path munging.
#
# What gets published:
#   include/uapi/xos/*.h → $DEST/xos/          (UAPI contract headers — shared kernel/user ABI)
#   user/include/*.h     → $DEST/              (POSIX/C standard headers — the libc side)
#   user/include/sys/*.h → $DEST/sys/
#
# What is NOT published (deliberately):
#   common/              — non-UAPI shared implementation:
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
#   gcc -nostdinc -ffreestanding -I<dest> -E -H user/hello.c
#   → every header in <stdio.h>+<time.h>'s closure resolves under <dest> with no
#     "No such file" lines, proving the published tree is self-contained.
set -euo pipefail

SRC="$(cd "$(dirname "$0")" && pwd)"
DEST="${1:-$SRC/build/sysroot/usr/include}"

echo "Installing UAPI headers → $DEST"
rm -rf "$DEST"
mkdir -p "$DEST/sys"

# 1. UAPI contract headers (include/uapi/xos/) — the OS's include/uapi/.
#    Only *.h: the dir also holds CMakeLists.txt (a build file, not an installed header).
mkdir -p "$DEST/xos"
cp "$SRC"/include/uapi/xos/*.h "$DEST/xos/"

# 2. Standard / POSIX headers (user/include/) — the libc side.
#    Top-level *.h → $DEST/; sys/*.h → $DEST/sys/.
cp    "$SRC"/user/include/*.h  "$DEST/"
cp -r "$SRC"/user/include/sys/. "$DEST/sys/"

echo "Installed tree:"
( cd "$DEST" && find . -type f | sort | sed 's/^\.\//  /' )

# Self-test: prove the published tree is self-contained by preprocessing hello.c
# against ONLY the sysroot (-nostdinc) plus the toolchain's freestanding headers
# (-isystem, which supplies stdint.h/stddef.h/stdarg.h — the same headers a real
# --target=x86_64-xos cross gcc ships). -I$DEST makes the sysroot the include root,
# exactly as -isysroot would expose /usr/include. -H lists every header opened; a
# fatal "No such file" means the repo-owned closure broke (a real bug).
echo
echo "Closure check: preprocessing user/hello.c against $DEST ..."
FREESTANDING="$(gcc -print-file-name=include)"
if gcc -nostdinc -ffreestanding -isystem "$FREESTANDING" "-I$DEST" -E -H "$SRC/user/hello.c" >/dev/null 2>/tmp/closure.log; then
  # -H emits one line per header opened (indented by depth). Lines from the toolchain's
  # freestanding dir contain "/gcc/"; everything else is a sysroot-owned header.
  repo_opened=$(grep -vc '/gcc/' /tmp/closure.log || true)
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
  if ! gcc -nostdinc -ffreestanding -isystem "$FREESTANDING" "-I$DEST" -E -H /tmp/hdr_probe.c >/dev/null 2>/tmp/hdr.log; then
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
