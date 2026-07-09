/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 *
 * Shim header: maps <asm/ioctl.h> (used by upstream drm.h) to our
 * xos/ioctl.h which provides Linux-compatible _IOC/_IO/_IOW/_IOR/_IOWR.
 * Placed under include/uapi/asm/ so -Iinclude/uapi resolves <asm/ioctl.h>
 * to this file.
 */

#ifndef _ASM_IOCTL_H
#define _ASM_IOCTL_H

#include "xos/ioctl.h"

#endif /* _ASM_IOCTL_H */
