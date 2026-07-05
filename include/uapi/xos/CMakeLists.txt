# Copyright (c) 2026 hesse
#
# SPDX-License-Identifier: MIT

# include/uapi/xos/ — UAPI contract headers (kernel/userspace shared ABI).
# This is the OS's equivalent of Linux's include/uapi/ — the publish source for
# `make headers_install` (see install-headers.sh): this directory is copied verbatim
# to /usr/include/xos/ in the sysroot, matching the include paths written in source
# (e.g. #include "xos/errno.h", resolved via -Iinclude/uapi), so the source form
# == publish form (zero rewrite).
#
# Self-containment rule (same as Linux uapi/): every header here must depend only
# on freestanding <stdint.h>/<stddef.h>/... or on other xos/ headers — never on
# kernel-internal headers. This is what lets them be installed to /usr/include
# and included by host-built user programs without the kernel source tree.
#
# Non-UAPI shared implementation (macro.h, kvformat.*) stays in utils/ — those
# are NOT published to the sysroot.
#
# No compilation targets — all headers are header-only.
