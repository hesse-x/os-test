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
#include "kernel/xcore/trap.h"
#include "kernel/xcore/xtask.h"
#include <stddef.h>

// === scheduling ===
void xtask_set_state(xtask *t, proc_state s);
void xtask_wake_from_wait(xtask *t);

// === address space ===
mm *mm_create(void);
void mm_put(mm *mm);
void mm_release(mm *mm, pid_t owner);
void mm_release_pages(mm *mm);
int copy_page_table(uint64_t *src, uint64_t *dst, mmap_region *regions);
mmap_region *add_mmap_region(xtask *t, uint64_t vaddr, uint64_t size,
                             uint64_t phys, struct shm *shm, uint32_t prot);

// === IPC ===
void notify_and_wake(pid_t target_pid, recv_msg *msg);
int kernel_msg_send(pid_t target_pid, const void *req, size_t req_len,
                    void *resp, size_t resp_len);
int64_t ipc_dequeue(xtask *proc, void __user *buf, void __user *data_buf,
                    size_t data_buf_len);

// === memory ===
void *kmalloc(size_t size);
void kfree(const void *ptr);
void *kcalloc(size_t n, size_t size);
void *krealloc(void *ptr, size_t size);
struct page *bfc_alloc_page(size_t n);
struct page *bfc_free_page(struct page *page, size_t n);
phys_addr_t page_to_phys(struct page *p);
kern_vaddr_t phys_to_virt(phys_addr_t phys);
bool map_user_page_direct(uint64_t *pml4, uint64_t vaddr, uint64_t phys,
                          uint64_t flags);
void unmap_user_pages(uint64_t *pml4, uint64_t vaddr_start, uint64_t vaddr_end,
                      int count);
size_t copy_from_user(void *dst, const void __user *src, size_t len);
size_t copy_to_user(void __user *dst, const void *src, size_t len);

// === interrupts ===
void irq_register(int vec, irq_handler_fn fn);
void irq_unregister(int vec);
int irq_owner_check(int irq);
void irq_owner_cleanup(pid_t pid);

// === ISR-safe eventfd signal (for driver hard-IRQ context) ===
struct file;
void eventfd_signal_isr(struct file *f);

// === file wait-queue (lazy wq for ipcfd/poll wake) ===
struct wait_queue_head;
struct wait_queue_head *file_wq_get(struct file *f);

// === SHM (kernel internal) ===
uint64_t shm_alloc_pages(uint64_t npages);
struct shm *shm_create_internal(uint64_t npages);
struct shm *shm_get(struct shm *s);
void shm_put(struct shm *s);

// === infrastructure ===
// printk/panic/spinlock declared in their own headers, not repeated here

#endif // KERNEL_XCORE_KPI_H
