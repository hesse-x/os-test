/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef KERNEL_XHCI_H
#define KERNEL_XHCI_H

#include "kernel/driver/driver.h"

void xhci_init();
void xhci_poll();

extern struct dev_driver xhci_driver;

#endif // KERNEL_XHCI_H
