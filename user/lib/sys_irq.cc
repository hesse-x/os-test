/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#include <sys/irq.h>
#include <syscall.h>

int irq_bind(int irq) { return sys_irq_bind(irq); }
