/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef KERNEL_XCORE_KTHREAD_H
#define KERNEL_XCORE_KTHREAD_H

#include <stdbool.h>
#include <stdint.h>

struct kthread;

struct xtask *kthread_task(struct kthread *thread);
struct kthread *kthread_create(int (*fn)(void *), void *arg, const char *name);
int kthread_run_detached(int (*fn)(void *), void *arg, const char *name);
int kthread_wait_timeout(struct kthread *thread, uint64_t deadline_ns);
void kthread_wake(struct kthread *thread);
bool kthread_should_stop(struct kthread *thread);
int kthread_stop(struct kthread *thread);

#endif
