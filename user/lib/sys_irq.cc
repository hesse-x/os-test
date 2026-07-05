/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#include "syscall.h"
#include <sys/irq.h>

int irq_bind(int irq) { return sys_irq_bind(irq); }
