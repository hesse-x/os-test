/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef XOS_LWIP_ARCH_CC_H
#define XOS_LWIP_ARCH_CC_H

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

#define LWIP_ERRNO_INCLUDE <errno.h>
#define LWIP_ERRNO_STDINCLUDE 1
#define LWIP_RAND() ((unsigned int)rand())
#define LWIP_PLATFORM_DIAG(x)                                                  \
  do {                                                                         \
    printf x;                                                                  \
  } while (0)
#define LWIP_PLATFORM_ASSERT(x)                                                \
  do {                                                                         \
    fprintf(stderr, "lwip assertion: %s\n", (x));                              \
    abort();                                                                   \
  } while (0)

typedef unsigned int sys_prot_t;

#endif
