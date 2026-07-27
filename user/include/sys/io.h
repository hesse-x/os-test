/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 *
 * I/O port access permission control. Matches musl's <sys/io.h> surface for
 * ioperm (plan §2.F3); the legacy <unistd.h> declared ioperm directly, but musl
 * places it here. Port-I/O macros (inb/outb & friends) are not provided — this
 * OS uses ioperm(2) to grant access and inline asm to perform the access, and
 * no current consumer needs the macro set.
 */
#ifndef _SYS_IO_H
#define _SYS_IO_H

#include <features.h>

#ifdef __cplusplus
extern "C" {
#endif

int iopl(int);
int ioperm(unsigned long, unsigned long, int);

#ifdef __cplusplus
}
#endif

#endif /* _SYS_IO_H */
