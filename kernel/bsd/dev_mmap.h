/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef KERNEL_BSD_DEV_MMAP_H
#define KERNEL_BSD_DEV_MMAP_H

#include "kernel/bsd/devtmpfs.h"

// prepare owns one owner reference in backing. Commit consumes it on success;
// callers use abort after every failed prepare/commit path.
void dev_mmap_abort(struct dev_mmap_backing *backing);
int64_t dev_mmap_commit(mm *address_space, uint64_t *pml4,
                        const struct dev_mmap_request *request,
                        struct dev_mmap_backing *backing);

#endif
