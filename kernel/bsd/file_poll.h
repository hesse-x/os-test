/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef KERNEL_FILE_POLL_H
#define KERNEL_FILE_POLL_H

#include "kernel/bsd/devtmpfs.h" // __poll
#include "kernel/xcore/wait_queue.h"

struct file;
__poll file_poll(struct file *f, __poll events);
wait_queue_head *file_wq_get(struct file *f);

#endif // KERNEL_FILE_POLL_H
