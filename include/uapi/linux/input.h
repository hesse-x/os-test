/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef _UAPI_LINUX_INPUT_H
#define _UAPI_LINUX_INPUT_H

#include <linux/types.h>
#include <sys/ioctl.h>

/* Revoke an evdev file descriptor; used by seatd during device handoff. */
#define EVIOCREVOKE _IOW('E', 0x91, int)

#endif
