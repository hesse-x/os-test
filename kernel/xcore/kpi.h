/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef KERNEL_XCORE_KPI_H
#define KERNEL_XCORE_KPI_H

#include "arch/x64/trap.h"
#include "kernel/xcore/mm_types.h"
#include "kernel/xcore/sched.h"
#include "kernel/xcore/xtask.h"
#include <stddef.h>

// IRQ handler callback type
typedef void (*irq_handler_t)(trapframe_t *);

// === scheduling ===
void xtask_set_state(xtask_t *t, proc_state_t s);
void xtask_wake_from_wait(xtask_t *t);

// === address space ===
mm_t *mm_create(void);
void mm_put(mm_t *mm);
void mm_release(mm_t *mm, pid_t owner);
void mm_release_pages(mm_t *mm);
int copy_page_table(uint64_t *src, uint64_t *dst, mmap_region_t *regions);
mmap_region_t *add_mmap_region(xtask_t *t, uint64_t vaddr, uint64_t size,
                               uint64_t phys, struct shm *shm, uint32_t prot);

// === IPC ===
void notify_and_wake(pid_t target_pid, recv_msg_t *msg);
void wake_process(pid_t pid);
int kernel_msg_send(pid_t target_pid, const void *req, size_t req_len,
                    void *resp, size_t resp_len);

// === memory ===
void *kmalloc(size_t size);
void kfree(const void *ptr);
void *kcalloc(size_t n, size_t size);
void *krealloc(void *ptr, size_t size);
Page *bfc_alloc_page(size_t n);
Page *bfc_free_page(Page *page, size_t n);
phys_addr_t page_to_phys(Page *p);
kern_vaddr_t phys_to_virt(phys_addr_t phys);
bool map_user_page_direct(uint64_t *pml4, uint64_t vaddr, uint64_t phys,
                          uint64_t flags);
void unmap_user_pages(uint64_t *pml4, uint64_t vaddr_start, uint64_t vaddr_end,
                      int count);
int copy_from_user(void *dst, const void __user *src, size_t len);
int copy_to_user(void __user *dst, const void *src, size_t len);

// === interrupts ===
void irq_register(int vec, irq_handler_t fn);
void irq_unregister(int vec);
int irq_owner_check(int irq);
void irq_owner_cleanup(pid_t pid);

// === SHM (kernel internal) ===
uint64_t shm_alloc_pages(uint64_t npages);
struct shm *shm_create_internal(uint64_t npages);
struct shm *shm_get(struct shm *s);
void shm_put(struct shm *s);

// === infrastructure ===
// printk/panic/spinlock declared in their own headers, not repeated here

#endif // KERNEL_XCORE_KPI_H
