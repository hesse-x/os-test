/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef _SYS_IOCCOM_H
#define _SYS_IOCCOM_H

#include <sys/ioctl.h>

#define IOC_OUT _IOC_READ
#define IOC_IN _IOC_WRITE
#define IOC_INOUT (_IOC_READ | _IOC_WRITE)
#define IOC_VOID _IOC_NONE
#define _IOWINT(type, nr) _IOW(type, nr, int)

#endif
