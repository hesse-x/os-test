/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

// kernel/xcore/ipc.c — Xcore IPC syscalls and primitives
// Extracted from kernel/trap.c (phase 3 step 3.1)

#include "arch/x64/apic.h"
#include "arch/x64/memlayout.h"
#include "arch/x64/paging.h"
#include "arch/x64/smp.h"
#include "arch/x64/utils.h"
#include "kernel/user_check.h"
#include "kernel/xcore/acpi.h"
#include "kernel/xcore/atomic.h"
#include "kernel/xcore/kpi.h"
#include "kernel/xcore/log.h"
#include "kernel/xcore/mem/alloc.h"
#include "kernel/xcore/mem/slab.h"
#include "kernel/xcore/mm_types.h"
#include "kernel/xcore/sched.h"
#include "kernel/xcore/sparse.h"
#include "kernel/xcore/spinlock.h"
#include "kernel/xcore/trap.h"
#include "kernel/xcore/xtask.h"
#include <stdbool.h>
#include <stdint.h>
#include <xos/errno.h>
#include <xos/syscall.h>
#include <xos/syscall_nums.h>

// ===================== IRQ owner table (shared with trap.c)
// =====================
extern pid_t irq_owner[MAX_IRQ_HANDLERS];

// ===================== IPC helper: check if kernel ISR registered
// =====================
extern int irq_has_handler(int irq); // defined in trap.c

// ===================== SHM reference counting =====================
struct shm *shm_get(struct shm *shm) {
  if (!shm)
    return NULL;
  refcount_inc(&shm->s_count);
  return shm;
}

void shm_put(struct shm *shm) {
  if (!shm)
    return;
  if (refcount_dec_and_test(&shm->s_count)) {
    // Last reference released — free all pages and struct
    if (shm->page_list) {
      for (int i = 0; i < shm->num_pages; i++) {
        struct page *p = &bfc_frames[PHY_TO_PAGE(shm->page_list[i])];
        bfc_free_page(p, 1);
      }
      kfree(shm->page_list);
    } else if (shm->phys != 0 && shm->npages > 0) {
      // Contiguous pages (legacy)
      struct page *page = &bfc_frames[PHY_TO_PAGE(shm->phys)];
      bfc_free_page(page, shm->npages);
    }
    kfree(shm);
  }
}

// ===================== Internal SHM allocation API =====================

// shm_alloc_pages(npages) — allocate contiguous physical pages, zeroed
uint64_t shm_alloc_pages(uint64_t npages) {
  if (npages == 0)
    return 0;
  struct page *pages = bfc_alloc_page(npages);
  if (!pages)
    return 0;
  uint64_t phys = (__force uint64_t)page_to_phys(pages);
  __memset((__force void *)phys_to_virt((__force phys_addr_t)phys), 0,
           npages * PAGE_SIZE);
  return phys;
}

// shm_create_internal(npages) — kernel-internal SHM creation
struct shm *shm_create_internal(uint64_t npages) {
  if (npages == 0)
    return NULL;

  uint64_t phys = shm_alloc_pages(npages);
  if (!phys)
    return NULL;

  // Allocate page_list pointing to each contiguous page
  uint64_t *page_list = (uint64_t *)kmalloc((size_t)npages * sizeof(uint64_t));
  if (!page_list) {
    struct page *p = &bfc_frames[PHY_TO_PAGE(phys)];
    bfc_free_page(p, npages);
    return NULL;
  }
  for (uint64_t i = 0; i < npages; i++)
    page_list[i] = phys + i * PAGE_SIZE;

  struct shm *shm = (struct shm *)kmalloc(sizeof(struct shm));
  if (!shm) {
    struct page *p = &bfc_frames[PHY_TO_PAGE(phys)];
    bfc_free_page(p, npages);
    kfree(page_list);
    return NULL;
  }

  shm->phys = 0; // pure page_list model
  shm->npages = 0;
  shm->file_size = npages * PAGE_SIZE;
  refcount_set(&shm->s_count, 1);
  shm->flags = 0; // no SHM_KERNEL
  shm->seals = 0;
  shm->name[0] = '\0';
  shm->page_list = page_list;
  shm->num_pages = (int)npages;
  return shm;
}

// Helper: allocate one page and add to shm's page_list
// Returns physical address on success, 0 on failure
uint64_t shm_add_page(struct shm *shm) {
  struct page *page = bfc_alloc_page(1);
  if (!page)
    return 0;
  uint64_t phys = (__force uint64_t)page_to_phys(page);

  // Zero the page
  __memset((__force void *)phys_to_virt((__force phys_addr_t)phys), 0,
           PAGE_SIZE);

  if (!shm->page_list) {
    // First discrete page: allocate page_list array
    int initial_cap = 16;
    shm->page_list =
        (uint64_t *)kmalloc((size_t)initial_cap * sizeof(uint64_t));
    if (!shm->page_list) {
      bfc_free_page(page, 1);
      return 0;
    }
    shm->num_pages = 0;
  }

  // Check if we need to grow the page_list array
  if (shm->num_pages > 0 && (shm->num_pages % 16 == 0)) {
    int new_cap = shm->num_pages + 16;
    uint64_t *new_list =
        (uint64_t *)kmalloc((size_t)new_cap * sizeof(uint64_t));
    if (!new_list) {
      bfc_free_page(page, 1);
      return 0;
    }
    __memcpy(new_list, shm->page_list,
             (size_t)shm->num_pages * sizeof(uint64_t));
    kfree(shm->page_list);
    shm->page_list = new_list;
  }

  return phys;
}

// ===================== Xcore IPC syscall: getpid =====================
int64_t sys_getpid(int64_t _u1, int64_t _u2, int64_t _u3, int64_t _u4,
                   int64_t _u5, int64_t _u6) {
  return (int64_t)current_task->tgid; // return tgid (process ID)
}

// ===================== Xcore IPC syscall: gettid =====================
int64_t sys_gettid(int64_t _u1, int64_t _u2, int64_t _u3, int64_t _u4,
                   int64_t _u5, int64_t _u6) {
  return (int64_t)current_task->pid; // return tid (thread ID)
}

// ===================== Xcore IPC syscall: yield =====================
int64_t sys_yield(int64_t _u1, int64_t _u2, int64_t _u3, int64_t _u4,
                  int64_t _u5, int64_t _u6) {
  schedule();
  return 0;
}

// ===================== Xcore IPC syscall: recv =====================
int64_t sys_recv(int64_t arg1, int64_t arg2, int64_t arg3, int64_t arg4,
                 int64_t _u1, int64_t _u2) {
  void __user *buf = (void __user *__force)arg1;
  void __user *data_buf = (void __user *__force)arg2;
  size_t data_buf_len = (size_t)arg3;
  uint32_t timeout_ms = (uint32_t)arg4;

  // Validate user pointer
  uint64_t ptr = (__force uint64_t)buf;
  if (!ptr || ptr >= 0xFFFFFFFF80000000ULL ||
      ptr + RECV_MSG_SIZE > 0xFFFFFFFF80000000ULL)
    return (int64_t)-EFAULT;

  // Validate data_buf if provided
  if (data_buf &&
      (data_buf_len == 0 ||
       (__force uint64_t)data_buf >= 0xFFFFFFFF80000000ULL ||
       (__force uint64_t)data_buf + data_buf_len > 0xFFFFFFFF80000000ULL))
    return (int64_t)-EFAULT;

  xtask *proc = current_task;
  int cpu = proc->assigned_cpu;
  while (1) {
    // Try to dequeue a message from recv queue
    spin_lock(&proc->recv_lock);
    if (proc->recv_head != proc->recv_tail) {
      // Message available: copy to user buffer
      copy_to_user(buf, proc->recv_buf[proc->recv_tail], RECV_MSG_SIZE);
      // If this is an REQ request, record the caller PID for sys_resp
      recv_msg *msg = (recv_msg *)proc->recv_buf[proc->recv_tail];
      if (msg->type == RECV_REQ) {
        proc->req_caller_pid = (pid_t)msg->src;
      }
      // If this is RECV_MSG, copy data to user data_buf and free kernel buffer
      if (msg->type == RECV_MSG) {
        void *kmaddr = msg->msg.kmaddr;
        size_t len = msg->msg.len;
        proc->msg_caller_pid = (pid_t)msg->src;

        if (!data_buf || data_buf_len < len) {
          kfree(kmaddr);
          recv_msg *umsg = (recv_msg __force *)buf;
          umsg->msg.kmaddr = NULL;
          umsg->msg.len = len;
          proc->recv_tail = (proc->recv_tail + 1) % RECV_QUEUE_SIZE;
          spin_unlock(&proc->recv_lock);
          return (int64_t)-EINVAL;
        }

        copy_to_user(data_buf, kmaddr, len);
        kfree(kmaddr);

        recv_msg *umsg = (recv_msg __force *)buf;
        umsg->msg.kmaddr = NULL;
        umsg->msg.len = len;
      }
      proc->recv_tail = (proc->recv_tail + 1) % RECV_QUEUE_SIZE;
      spin_unlock(&proc->recv_lock);
      return 0;
    }
    spin_unlock(&proc->recv_lock);

    // Queue empty: block on WAIT_RECV
    proc->state = BLOCKED;
    proc->wait_event = WAIT_RECV;
    proc->wait_timed_out = 0;

    if (timeout_ms > 0) {
      proc->wait_deadline = sched_clock() + (int64_t)timeout_ms * 1000000ULL;
      uint64_t flags;
      spin_lock_irqsave(&cpu_locals[cpu].scheduler_lock, &flags);
      sched_timer_queue_insert(cpu, proc);
      spin_unlock_irqrestore(&cpu_locals[cpu].scheduler_lock, flags);
    } else {
      proc->wait_deadline = 0;
    }

    schedule();

    // EINTR check: ISR notification (recv_intr set by wake_process)
    if (proc->recv_intr) {
      proc->recv_intr = 0;
      return (int64_t)-EINTR;
    }

    // EINTR check: signal pending and deliverable
    if (signal_pending_hook && signal_pending_hook(proc))
      return (int64_t)-EINTR;

    // Woken up: check if timed out
    if (proc->wait_timed_out) {
      // Re-check queue before returning timeout
      spin_lock(&proc->recv_lock);
      if (proc->recv_head != proc->recv_tail) {
        copy_to_user(buf, proc->recv_buf[proc->recv_tail], RECV_MSG_SIZE);
        recv_msg *msg = (recv_msg *)proc->recv_buf[proc->recv_tail];
        if (msg->type == RECV_REQ) {
          proc->req_caller_pid = (pid_t)msg->src;
        }
        if (msg->type == RECV_MSG) {
          void *kmaddr = msg->msg.kmaddr;
          size_t len = msg->msg.len;
          proc->msg_caller_pid = (pid_t)msg->src;

          if (!data_buf || data_buf_len < len) {
            kfree(kmaddr);
            recv_msg *umsg = (recv_msg __force *)buf;
            umsg->msg.kmaddr = NULL;
            umsg->msg.len = len;
            proc->recv_tail = (proc->recv_tail + 1) % RECV_QUEUE_SIZE;
            spin_unlock(&proc->recv_lock);
            return (int64_t)-EINVAL;
          }
          copy_to_user(data_buf, kmaddr, len);
          kfree(kmaddr);
          recv_msg *umsg = (recv_msg __force *)buf;
          umsg->msg.kmaddr = NULL;
          umsg->msg.len = len;
        }
        proc->recv_tail = (proc->recv_tail + 1) % RECV_QUEUE_SIZE;
        spin_unlock(&proc->recv_lock);
        return 0;
      }
      spin_unlock(&proc->recv_lock);
      return (int64_t)-ETIMEDOUT;
    }
    // Non-timeout wakeup: a message was enqueued, loop back to dequeue it
  }
}

// ===================== Xcore IPC syscall: req =====================
int64_t sys_req(int64_t arg1, int64_t arg2, int64_t arg3, int64_t _u1,
                int64_t _u2, int64_t _u3) {
  pid_t target_pid = (pid_t)arg1;
  void __user *request = (void __user *__force)arg2;
  void __user *reply = (void __user *__force)arg3;
  printk(LOG_DEBUG, "sys_req: pid=%d -> target_pid=%d\n", current_task->pid,
         target_pid);

  if (target_pid < 0 || target_pid >= MAX_PROC)
    return (int64_t)-ESRCH;

  uint64_t req_ptr = (__force uint64_t)request;
  uint64_t rep_ptr = (__force uint64_t)reply;
  if (!req_ptr || req_ptr >= 0xFFFFFFFF80000000ULL ||
      req_ptr + RECV_MSG_SIZE > 0xFFFFFFFF80000000ULL)
    return (int64_t)-EFAULT;
  if (!rep_ptr || rep_ptr >= 0xFFFFFFFF80000000ULL ||
      rep_ptr + RECV_MSG_SIZE > 0xFFFFFFFF80000000ULL)
    return (int64_t)-EFAULT;

  xtask *target = task_get(target_pid);
  if (target->pid != target_pid)
    return (int64_t)-ESRCH;
  WARN_ON(target->state == UNUSED);

  // Build RECV_REQ message
  uint8_t msg[RECV_MSG_SIZE];
  recv_msg *hdr = (recv_msg *)msg;
  hdr->type = RECV_REQ;
  hdr->src = (uint32_t)current_task->pid;
  copy_from_user(hdr->data, request, 56);

  // Enqueue to target's recv queue
  spin_lock(&target->recv_lock);
  uint32_t next = (target->recv_head + 1) % RECV_QUEUE_SIZE;
  if (next == target->recv_tail) {
    spin_unlock(&target->recv_lock);
    printk(LOG_WARN, "sys_req: target_pid=%d recv queue full!\n", target_pid);
    return (int64_t)-EBUSY;
  }
  __memcpy(target->recv_buf[target->recv_head], msg, RECV_MSG_SIZE);
  target->recv_head = next;
  spin_unlock(&target->recv_lock);

  // Wake target if in WAIT_RECV
  int target_cpu = target->assigned_cpu;
  uint64_t flags;
  spin_lock_irqsave(&cpu_locals[target_cpu].scheduler_lock, &flags);
  if (target->state == BLOCKED && target->wait_event == WAIT_RECV) {
    wake_from_wait(target);
  }
  spin_unlock_irqrestore(&cpu_locals[target_cpu].scheduler_lock, flags);

  // Block caller on WAIT_REQ_REPLY
  xtask *proc = current_task;
  proc->state = BLOCKED;
  proc->wait_event = WAIT_REQ_REPLY;
  proc->wait_timed_out = 0;
  proc->wait_deadline = sched_clock() + 5000000000ULL; // 5 second timeout
  proc->req_target_pid = target_pid;
  proc->req_reply_buf = reply;
  proc->req_reply_len = RECV_MSG_SIZE;
  proc->req_result = 0;

  int cpu = proc->assigned_cpu;
  uint64_t flags2;
  spin_lock_irqsave(&cpu_locals[cpu].scheduler_lock, &flags2);
  sched_timer_queue_insert(cpu, proc);
  spin_unlock_irqrestore(&cpu_locals[cpu].scheduler_lock, flags2);

  schedule();

  // Timeout check
  if (proc->wait_timed_out) {
    return (int64_t)-ETIMEDOUT;
  }

  // EINTR check
  if (signal_pending_hook && signal_pending_hook(proc))
    return (int64_t)-EINTR;

  // Woken up by sys_resp or proc_reap
  if (proc->req_result != 0)
    return (int64_t)proc->req_result;
  return 0;
}

// ===================== Xcore IPC syscall: resp =====================
int64_t sys_resp(int64_t arg1, int64_t _u1, int64_t _u2, int64_t _u3,
                 int64_t _u4, int64_t _u5) {
  void __user *reply = (void __user *__force)arg1;

  uint64_t ptr = (__force uint64_t)reply;
  if (!ptr || ptr >= 0xFFFFFFFF80000000ULL ||
      ptr + RECV_MSG_SIZE > 0xFFFFFFFF80000000ULL)
    return (int64_t)-EFAULT;

  xtask *proc = current_task;
  pid_t caller_pid = proc->req_caller_pid;
  if (caller_pid < 0 || caller_pid >= MAX_PROC)
    return (int64_t)-EINVAL;

  xtask *caller = task_get(caller_pid);
  if (caller->pid != caller_pid)
    return (int64_t)-ESRCH;

  size_t reply_len = caller->req_reply_len;
  if (reply_len == 0)
    reply_len = RECV_MSG_SIZE;
  if (reply_len > RECV_MSG_SIZE)
    reply_len = RECV_MSG_SIZE;
  uint8_t kbuf[RECV_MSG_SIZE];
  copy_from_user(kbuf, reply, RECV_MSG_SIZE);

  uint64_t saved_cr3;
  __asm__ volatile("movq %%cr3, %0" : "=r"(saved_cr3));
  __asm__ volatile("movq %0, %%cr3" ::"r"((int64_t)caller->cr3) : "memory");
  copy_to_user(caller->req_reply_buf, kbuf, reply_len);
  __asm__ volatile("movq %0, %%cr3" ::"r"(saved_cr3) : "memory");

  // Wake caller
  int caller_cpu = caller->assigned_cpu;
  uint64_t flags;
  spin_lock_irqsave(&cpu_locals[caller_cpu].scheduler_lock, &flags);
  if (caller->state == BLOCKED && caller->wait_event == WAIT_REQ_REPLY) {
    caller->req_result = 0;
    wake_from_wait(caller);
  }
  spin_unlock_irqrestore(&cpu_locals[caller_cpu].scheduler_lock, flags);

  proc->req_caller_pid = -1;
  return 0;
}

// ===================== Xcore IPC syscall: irq_bind =====================
int64_t sys_irq_bind(int64_t arg1, int64_t _u1, int64_t _u2, int64_t _u3,
                     int64_t _u4, int64_t _u5) {
  int irq = (int)arg1;
  if (irq < 0 || irq >= MAX_IRQ_HANDLERS)
    return (int64_t)-EINVAL;
  if (irq_has_handler(irq))
    return (int64_t)-EBUSY;
  __atomic_store_n(&irq_owner[irq], current_task->pid, __ATOMIC_RELEASE);

  // Auto-unmask I/O APIC for this IRQ (GSI = vector - 32)
  int gsi = irq - 32;
  if (gsi >= 0 && gsi < 24) {
    uint32_t bsp_apic_id = (uint32_t)(lapic_read(LAPIC_ID) >> 24);
    const acpi_iso_override *iso = acpi_find_iso((uint8_t)gsi);
    bool level = iso ? iso->level_triggered : false;
    bool low = iso ? iso->active_low : false;
    ioapic_set_irq(gsi, irq, bsp_apic_id, false, level, low);
  }

  return 0;
}

// ===================== notify_and_wake =====================
void notify_and_wake(pid_t target_pid, recv_msg *msg) {
  if (target_pid < 0 || target_pid >= MAX_PROC)
    return;
  xtask *target = task_get(target_pid);
  if (target->pid != target_pid)
    return;

  spin_lock(&target->recv_lock);
  uint32_t next = (target->recv_head + 1) % RECV_QUEUE_SIZE;
  if (next == target->recv_tail) {
    spin_unlock(&target->recv_lock);
    return;
  }
  __memcpy(target->recv_buf[target->recv_head], msg, RECV_MSG_SIZE);
  target->recv_head = next;
  spin_unlock(&target->recv_lock);

  int target_cpu = target->assigned_cpu;
  uint64_t flags;
  spin_lock_irqsave(&cpu_locals[target_cpu].scheduler_lock, &flags);
  if (target->pid == target_pid && target->state == BLOCKED &&
      target->wait_event == WAIT_RECV) {
    wake_from_wait(target);
  }
  spin_unlock_irqrestore(&cpu_locals[target_cpu].scheduler_lock, flags);
}

// ===================== kernel_msg_send =====================
int kernel_msg_send(pid_t target_pid, const void *req, size_t req_len,
                    void *resp, size_t resp_len) {
  uint8_t dummy[64];
  if (!resp) {
    resp = dummy;
    resp_len = sizeof(dummy);
  }
  return (int)sys_msg_to(target_pid, (void *)req, req_len, resp, resp_len);
}

// ===================== wake_process =====================
// Narrow semantics: only handles IPC-class waits
// (WAIT_PIPE/WAIT_POLL/WAIT_RECV). Hitting other events
// (WAIT_FUTEX/WAIT_CHILD/WAIT_REQ_REPLY/WAIT_MSG_REPLY) indicates a caller
// semantic error -- use wake_with_event (precise event match) or
// wake_process_any (signal path, must interrupt any blocking state). Encode
// this constraint in code: debug build triggers ASSERT panic, preventing future
// wait_event additions from repeating Bug 1 (memory:
// feedback_constraint_in_code).
void wake_process(pid_t pid) {
  if (pid < 0 || pid >= MAX_PROC)
    return;
  xtask *target = task_get(pid);

  int target_cpu = target->assigned_cpu;
  uint64_t flags;
  spin_lock_irqsave(&cpu_locals[target_cpu].scheduler_lock, &flags);
  if (target->pid == pid && target->state == BLOCKED) {
    wait_event ev = target->wait_event;
    ASSERT(ev == WAIT_PIPE || ev == WAIT_POLL || ev == WAIT_RECV);
    if (ev == WAIT_RECV)
      target->recv_intr = 1;
    wake_from_wait(target);
  }
  spin_unlock_irqrestore(&cpu_locals[target_cpu].scheduler_lock, flags);
}

// ===================== Xcore IPC syscall: notify =====================
int64_t sys_notify(int64_t arg1, int64_t _u1, int64_t _u2, int64_t _u3,
                   int64_t _u4, int64_t _u5) {
  pid_t target_pid = (pid_t)arg1;
  if (target_pid < 0 || target_pid >= MAX_PROC)
    return (int64_t)-EINVAL;

  xtask *target = task_get(target_pid);
  if (target->pid != target_pid)
    return (int64_t)-ESRCH;

  spin_lock(&target->recv_lock);
  uint32_t next = (target->recv_head + 1) % RECV_QUEUE_SIZE;
  if (next == target->recv_tail) {
    spin_unlock(&target->recv_lock);
    return (int64_t)-EBUSY;
  }
  recv_msg *slot = (recv_msg *)target->recv_buf[target->recv_head];
  slot->type = RECV_NOTIFY;
  slot->src = (uint32_t)current_task->pid;
  target->recv_head = next;
  spin_unlock(&target->recv_lock);

  int target_cpu = target->assigned_cpu;
  uint64_t flags;
  spin_lock_irqsave(&cpu_locals[target_cpu].scheduler_lock, &flags);
  if (target->state == BLOCKED && target->wait_event == WAIT_RECV) {
    wake_from_wait(target);
  }
  spin_unlock_irqrestore(&cpu_locals[target_cpu].scheduler_lock, flags);
  return 0;
}

// ===================== Xcore IPC syscall: gettime / clock
// =====================
int64_t sys_gettime(int64_t _u1, int64_t _u2, int64_t _u3, int64_t _u4,
                    int64_t _u5, int64_t _u6) {
  return sched_clock();
}

int64_t sys_clock(int64_t _u1, int64_t _u2, int64_t _u3, int64_t _u4,
                  int64_t _u5, int64_t _u6) {
  return current_task->cpu_time_ns;
}

// ===================== sys_msg_to (inner implementation) =====================
int64_t sys_msg_to(pid_t target_pid, void *msg_buf, size_t msg_len,
                   void *reply_buf, size_t reply_len) {
  xtask *target = task_get(target_pid);
  if (target->pid != target_pid)
    return (int64_t)-ESRCH;

  void *kbuf = kmalloc(msg_len);
  if (!kbuf)
    return (int64_t)-ENOMEM;
  __memcpy(kbuf, msg_buf, msg_len);

  uint8_t msg[RECV_MSG_SIZE];
  recv_msg *hdr = (recv_msg *)msg;
  hdr->type = RECV_MSG;
  hdr->src = (uint32_t)current_task->pid;
  hdr->msg.kmaddr = kbuf;
  hdr->msg.len = msg_len;

  spin_lock(&target->recv_lock);
  uint32_t next = (target->recv_head + 1) % RECV_QUEUE_SIZE;
  if (next == target->recv_tail) {
    spin_unlock(&target->recv_lock);
    kfree(kbuf);
    return (int64_t)-EBUSY;
  }
  __memcpy(target->recv_buf[target->recv_head], msg, RECV_MSG_SIZE);
  target->recv_head = next;
  spin_unlock(&target->recv_lock);

  int target_cpu = target->assigned_cpu;
  uint64_t flags;
  spin_lock_irqsave(&cpu_locals[target_cpu].scheduler_lock, &flags);
  if (target->state == BLOCKED && target->wait_event == WAIT_RECV) {
    wake_from_wait(target);
  }
  spin_unlock_irqrestore(&cpu_locals[target_cpu].scheduler_lock, flags);

  // Block caller on WAIT_MSG_REPLY
  xtask *proc = current_task;
  proc->state = BLOCKED;
  proc->wait_event = WAIT_MSG_REPLY;
  proc->wait_timed_out = 0;
  proc->wait_deadline = sched_clock() + 5000000000ULL;
  proc->msg_target_pid = target_pid;
  proc->msg_reply_buf = (void __user *__force)reply_buf;
  proc->msg_reply_len = reply_len;
  proc->msg_result = 0;

  int cpu = proc->assigned_cpu;
  uint64_t flags2;
  spin_lock_irqsave(&cpu_locals[cpu].scheduler_lock, &flags2);
  sched_timer_queue_insert(cpu, proc);
  spin_unlock_irqrestore(&cpu_locals[cpu].scheduler_lock, flags2);

  schedule();

  if (proc->wait_timed_out) {
    return (int64_t)-ETIMEDOUT;
  }

  // EINTR check
  if (signal_pending_hook && signal_pending_hook(proc))
    return (int64_t)-EINTR;

  if (proc->msg_result != 0)
    return (int64_t)proc->msg_result;
  return 0;
}

// ===================== Xcore IPC syscall: msg =====================
int64_t sys_msg(int64_t arg1, int64_t arg2, int64_t arg3, int64_t arg4,
                int64_t arg5, int64_t _u1) {
  pid_t target_pid = (pid_t)arg1;
  void __user *msg_buf = (void __user *__force)arg2;
  size_t msg_len = (size_t)arg3;
  void __user *reply_buf = (void __user *__force)arg4;
  size_t reply_len = (size_t)arg5;

  if (target_pid < 0 || target_pid >= MAX_PROC)
    return (int64_t)-ESRCH;

  if (msg_len == 0 || msg_len > 65536)
    return (int64_t)-EINVAL;

  uint64_t msg_ptr = (__force uint64_t)msg_buf;
  if (!msg_ptr || msg_ptr >= 0xFFFFFFFF80000000ULL ||
      msg_ptr + msg_len > 0xFFFFFFFF80000000ULL)
    return (int64_t)-EFAULT;

  uint64_t rep_ptr = (__force uint64_t)reply_buf;
  if (!rep_ptr || rep_ptr >= 0xFFFFFFFF80000000ULL ||
      rep_ptr + reply_len > 0xFFFFFFFF80000000ULL)
    return (int64_t)-EFAULT;

  return sys_msg_to(target_pid, (void __force *)msg_buf, msg_len,
                    (void __force *)reply_buf, reply_len);
}

// ===================== Xcore IPC syscall: msg_resp =====================
int64_t sys_msg_resp(int64_t arg1, int64_t arg2, int64_t _u1, int64_t _u2,
                     int64_t _u3, int64_t _u4) {
  void __user *resp_buf = (void __user *__force)arg1;
  size_t resp_len = (size_t)arg2;

  uint64_t ptr = (__force uint64_t)resp_buf;
  if (!ptr || ptr >= 0xFFFFFFFF80000000ULL || resp_len == 0 ||
      ptr + resp_len > 0xFFFFFFFF80000000ULL)
    return (int64_t)-EFAULT;

  xtask *proc = current_task;
  pid_t caller_pid = proc->msg_caller_pid;
  if (caller_pid < 0 || caller_pid >= MAX_PROC) {
    return (int64_t)-EINVAL;
  }

  xtask *caller = task_get(caller_pid);
  if (caller->pid != caller_pid)
    return (int64_t)-ESRCH;

  void *kbuf = kmalloc(resp_len);
  if (!kbuf)
    return (int64_t)-ENOMEM;
  copy_from_user(kbuf, resp_buf, resp_len);

  size_t copy_len =
      resp_len < caller->msg_reply_len ? resp_len : caller->msg_reply_len;

  uint64_t saved_cr3;
  __asm__ volatile("movq %%cr3, %0" : "=r"(saved_cr3));
  __asm__ volatile("movq %0, %%cr3" ::"r"((int64_t)caller->cr3) : "memory");
  copy_to_user(caller->msg_reply_buf, kbuf, copy_len);
  __asm__ volatile("movq %0, %%cr3" ::"r"(saved_cr3) : "memory");

  kfree(kbuf);

  int caller_cpu = caller->assigned_cpu;
  uint64_t flags;
  spin_lock_irqsave(&cpu_locals[caller_cpu].scheduler_lock, &flags);
  if (caller->state == BLOCKED && caller->wait_event == WAIT_MSG_REPLY) {
    caller->msg_result = 0;
    wake_from_wait(caller);
  }
  spin_unlock_irqrestore(&cpu_locals[caller_cpu].scheduler_lock, flags);

  proc->msg_caller_pid = -1;
  return 0;
}

// ===================== Xcore IPC syscall: ioperm =====================
int64_t sys_ioperm(int64_t arg1, int64_t arg2, int64_t arg3, int64_t _u1,
                   int64_t _u2, int64_t _u3) {
  unsigned long from = (unsigned long)arg1;
  unsigned long num = (unsigned long)arg2;
  int turn_on = (int)arg3;

  if (from + num > 65536)
    return (int64_t)-EINVAL;

  xtask *proc = current_task;

  if (!proc->iopm) {
    uint8_t *iopm = (uint8_t *)kmalloc(IOPM_SIZE);
    if (!iopm)
      return (int64_t)-ENOMEM;
    for (int i = 0; i < IOPM_SIZE; i++)
      iopm[i] = 0xFF;
    proc->iopm = iopm;
  }

  for (unsigned long port = from; port < from + num; port++) {
    int byte_idx = port / 8;
    int bit_idx = port % 8;
    if (turn_on) {
      proc->iopm[byte_idx] &= ~(1 << bit_idx);
    } else {
      proc->iopm[byte_idx] |= (1 << bit_idx);
    }
  }

  int cpu = proc->assigned_cpu;
  __memcpy(per_cpu_tss[cpu].iopm, proc->iopm, IOPM_SIZE);

  return 0;
}

// ===================== IRQ owner management =====================
void irq_owner_cleanup(pid_t pid) {
  for (int i = 0; i < MAX_IRQ_HANDLERS; i++) {
    if (irq_owner[i] == pid) {
      __atomic_store_n(&irq_owner[i], -1, __ATOMIC_RELEASE);
    }
  }
}

int irq_owner_check(int irq) {
  if (irq < 0 || irq >= MAX_IRQ_HANDLERS)
    return -1;
  return __atomic_load_n(&irq_owner[irq], __ATOMIC_ACQUIRE);
}
