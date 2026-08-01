/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef _UAPI_LINUX_KD_H
#define _UAPI_LINUX_KD_H

/* Linux virtual-console keyboard and graphics modes used by seatd. */
#define KDSETMODE 0x4B3A
#define KD_TEXT 0x00
#define KD_GRAPHICS 0x01
#define KDSKBMODE 0x4B45
#define K_UNICODE 0x03
#define K_OFF 0x04

#endif
