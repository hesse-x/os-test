/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

// kernel/bsd/proc.c — BSD process lifecycle: fd/mm/fork/execve
// Extracted from kernel/proc.c (phase 4 step 4.2)

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "arch/x64/memlayout.h"
#include "arch/x64/paging.h"
#include "arch/x64/smp.h"
#include "arch/x64/trap.h"
#include "arch/x64/utils.h"
#include "kernel/bsd/devtmpfs.h"
#include "kernel/bsd/elf_loader.h"
#include "kernel/bsd/eventpoll.h"
#include "kernel/bsd/fat32.h"
#include "kernel/bsd/file_lock.h"
#include "kernel/bsd/fops.h"
#include "kernel/bsd/inode.h"
#include "kernel/bsd/ipcfd.h"
#include "kernel/bsd/netlink.h"
#include "kernel/bsd/proc.h"
#include "kernel/bsd/pty.h"
#include "kernel/bsd/signal.h"
#include "kernel/bsd/socket.h"
#include "kernel/bsd/syscall.h"
#include "kernel/bsd/timerfd.h"
#include "kernel/bsd/types.h"
#include "kernel/bsd/vfs.h"
#include "kernel/kernel.h"
#include "kernel/xcore/atomic.h"
#include "kernel/xcore/kpi.h"
#include "kernel/xcore/log.h"
#include "kernel/xcore/mem/alloc.h"
#include "kernel/xcore/mem/vma.h"
#include "kernel/xcore/mm_types.h"
#include "kernel/xcore/rcu.h"
#include "kernel/xcore/sched.h"
#include "kernel/xcore/sparse.h"
#include "kernel/xcore/spinlock.h"
#include "kernel/xcore/trap.h"
#include "kernel/xcore/wait_queue.h"
#include "kernel/xcore/xtask.h"

#include <xos/elf.h>
#include <xos/errno.h>
#include <xos/fcntl.h>
#include <xos/mman.h>
#include <xos/page.h>
#include <xos/signal.h>
#include <xos/socket.h>
#include <xos/thread.h>

struct drm_fence;
// Minimal file_io_req for FD_FILE CLOSE notification (must match fs_driver
// struct layout)
typedef struct file_io_close_req {
  uint32_t cmd;
  uint8_t _path[256];
  uint32_t _flags;
  int32_t fs_fd; // at offset 264, same as fs_driver's file_req.fs_fd
} file_io_close_req;

// Forward declaration: defined in kernel/xcore/mem/copy_user.c
long strncpy_from_user(char *dst, const char __user *src, long maxlen);

// ===================== proc lifecycle =====================

proc *proc_create(void) {
  proc *bp = (proc *)kmalloc(sizeof(proc));
  if (!bp)
    return NULL;
  __memset(bp, 0, sizeof(proc));

  // Create files
  bp->files = files_create();
  if (!bp->files) {
    kfree(bp);
    return NULL;
  }

  // Create signal_struct (independent copy on fork; CLONE_SIGHAND refs
  // separately)
  bp->signal = signal_create();
  if (!bp->signal) {
    files_put(bp->files);
    kfree(bp);
    return NULL;
  }

  // Per-task signal fields + threading fields
  list_init(&bp->futex_node);
  bp->futex_uaddr = 0;
  bp->clear_tid_addr = NULL;
  bp->sig_pending = 0;
  bp->sig_blocked = 0;

  // POSIX identity defaults: single-user system (uid/gid=0). umask follows the
  // conventional 0022 default.
  bp->uid = 0;
  bp->euid = 0;
  bp->gid = 0;
  bp->egid = 0;
  bp->umask = 0022;
  return bp;
}

void proc_free(proc *bp) {
  if (!bp)
    return;
  if (bp->files)
    files_put(bp->files);
  // xtask_free is handled by the caller (sched_task_reap resets the xtask slot)
  kfree(bp);
}

// proc_reap: POSIX cleanup part of sched_task_reap
// Called directly from sched_task_reap() for synchronous per-process cleanup.
// Owns all POSIX resource freeing: files_put (closes all fds), signal_put,
// kfree(proc). do_exit does NOT put these — it only sets ZOMBIE and
// notifies parent; sched_task_reap/proc_reap is the sole owner of proc
// lifetime.
void proc_reap(xtask *proc) {
  if (!proc->proc)
    return;
  struct proc *bp = proc->proc;

  // S09: release this process's POSIX file locks across all inodes (Linux
  // exit(2) semantics — locks are released even if the process never ran an
  // explicit F_UNLCK). Done before files_put so concurrent lockers blocked in
  // F_SETLKW get woken promptly.
  file_lock_release_pid(proc->pid);

  if (bp->files) {
    struct file *entries[MAX_FD];
    for (int fd = 0; fd < MAX_FD; fd++) {
      entries[fd] = fd_uninstall(bp->files, fd);
    }
    synchronize_rcu();
    for (int fd = 0; fd < MAX_FD; fd++) {
      if (entries[fd])
        file_put(entries[fd]);
    }
    files_put(bp->files);
    bp->files = NULL;
  }

  if (bp->signal) {
    signal_put(bp->signal);
    bp->signal = NULL;
  }

  kfree(bp);
  proc->proc = NULL;
}

// proc_reap_idle: scan for orphaned zombie processes
// Called from sched_idle_entry via reap_hook
void proc_reap_idle(void) {
  // Safety net: scan for any ZOMBIE processes whose proc
  // wasn't freed (shouldn't happen with direct proc_reap
  // in sched_task_reap, but provides defense-in-depth)
}

// ===================== files lifecycle =====================

files *files_create(void) {
  struct files *files = (struct files *)kmalloc(sizeof(struct files));
  if (!files)
    return NULL;
  __memset(files, 0, sizeof(struct files));
  files->fd_lock = SPINLOCK_INIT;
  refcount_set(&files->f_count, 1);
  for (int j = 0; j < MAX_FD; j++) {
    files->fd_table[j] = NULL;
  }
  return files;
}

// ===================== unified fd lifecycle =====================

void file_put(struct file *f) {
  if (!f)
    return;
  if (!refcount_dec_and_test(&f->f_count))
    return;

  if (f->f_op && f->f_op->close)
    f->f_op->close(current_task, f);

  switch (f->type) {
  case FD_NONE:
    break;
  case FD_PIPE: {
    pipe *p = f->pipe;
    if (p) {
      wake_pipe_peers(p, f->flags);
      if (refcount_dec_and_test(&p->p_count)) {
        kfree(p->buf);
        kfree(p->wq);
        kfree(p);
      }
    }
    break;
  }
  case FD_SHM:
    if (f->shm)
      shm_put(f->shm);
    break;
  case FD_REGULAR:
  case FD_DIR:
    if (f->inode)
      inode_put(f->inode);
    break;
  case FD_DEV: {
    struct inode *ip = f->inode;
    if (ip) {
      /* §5: 锁下读 i_priv(防 borrow-window UAF)。fd 引用由 open 时
       * dev_ops_get 取,ops 在本 fd close 前不归 0,故读出后可安全用。
       * close 回调 + 放 fd 引用(dev_ops_put)在锁外完成;put 可能触发
       * kfree(仅 user-space driver 且本 fd 是最后持有者时)。*/
      struct dev_ops *ops = dev_ops_peek_by_inode(ip);
      if (ops) {
        if (ops->driver_pid == 0 && ops->close)
          ops->close(current_task, -1);
        dev_ops_put(ops); /* 放 fd 引用 */
      }
      inode_put(ip);
    }
    break;
  }
  case FD_FILE:
    if (refcount_dec_and_test(&f->file_data.f_count) &&
        f->file_data.fs_pid >= 0) {
      file_io_close_req req;
      __memset(&req, 0, sizeof(req));
      req.cmd = 4; // FILE_CMD_CLOSE
      req.fs_fd = f->file_data.fs_fd;
      kernel_msg_send(f->file_data.fs_pid, &req, sizeof(req), NULL, 0);
    }
    break;
  case FD_SOCKET:
    if (f->sock)
      unix_sock_close(f->sock);
    break;
  case FD_EPOLL:
    if (f->epoll) {
      eventpoll_release(f->epoll);
      f->epoll = NULL;
    }
    break;
  case FD_EVENTFD:
    if (f->eventfd) {
      kfree(f->eventfd);
      f->eventfd = NULL;
    }
    break;
  case FD_TIMERFD: {
    timerfd_ctx *tfd = f->timerfd;
    if (tfd) {
      spin_lock(&timerfd_list_lock);
      list_remove(&tfd->node);
      spin_unlock(&timerfd_list_lock);
      kfree(tfd);
      f->timerfd = NULL;
    }
    break;
  }
  case FD_SIGNALFD:
    if (f->signalfd) {
      kfree(f->signalfd);
      f->signalfd = NULL;
    }
    break;
  case FD_SYNC_FILE:
    /* FD_SYNC_FILE (plan2): drop the fence ref the fd holds. Defined in the
     * driver layer; declared here locally to avoid pulling drm_internal.h
     * into the BSD layer. */
    if (f->sync_file_fence) {
      extern void drm_fence_put(struct drm_fence * fence);
      drm_fence_put(f->sync_file_fence);
      f->sync_file_fence = NULL;
    }
    break;
  case FD_IPC:
    // Clear the owner task's ipcfd_file back-link and drop the create-time
    // reference (evdev_refact.md §4.3 生命周期 / §5.6).
    ipcfd_close(f);
    break;
  case FD_NETLINK:
    if (f->nlsock)
      netlink_sock_close(f->nlsock);
    break;
  case FD_TTY:
    pty_close_file(f);
    if (f->inode) {
      /* §5: ptmx_open/pts_open 把 FD_DEV mutate 成 FD_TTY,fd 引用随之转交,
       * close 时在此放(对齐 FD_DEV 分支)。ops 为 static(ptmx_ops)/嵌入式
       * (pts priv->ops),driver_pid==0,put 永不归 0,仅还回 open 取的计数。*/
      struct dev_ops *ops = dev_ops_peek_by_inode(f->inode);
      if (ops)
        dev_ops_put(ops);
      inode_put(f->inode);
    }
    break;
  }
  if (f->wq) {
    kfree(f->wq);
    f->wq = NULL;
  }
  kfree(f);
}

int alloc_fd(files *files, int min_fd) {
  for (int i = min_fd; i < MAX_FD; i++) {
    if (fd_lookup(files, i) == NULL)
      return i;
  }
  return -EMFILE;
}

/* FD_SYNC_FILE (plan2): install a sync_file fd bound to `fence`. The caller
 * (driver EXECBUFFER out-fence path) takes a fence ref before calling; this fd
 * holds that ref, released on close by the FD_SYNC_FILE case in file_put.
 *
 * Lives in the BSD layer so the driver never touches struct file layout / fd
 * table internals directly (the driver↔bsd include boundary). drm_fence is an
 * opaque type to the BSD layer — declared forward in kernel/bsd/types.h. */
int bsd_sync_file_fd_install(xtask *proc, struct drm_fence *fence) {
  if (!fence)
    return -EINVAL;

  spin_lock(&proc->proc->files->fd_lock);
  int fd = alloc_fd(proc->proc->files, 3);
  if (fd < 0) {
    spin_unlock(&proc->proc->files->fd_lock);
    return -EMFILE;
  }
  struct file *f = (struct file *)kmalloc(sizeof(struct file));
  if (!f) {
    spin_unlock(&proc->proc->files->fd_lock);
    return -ENOMEM;
  }
  __memset(f, 0, sizeof(*f));
  refcount_set(&f->f_count, 1);
  f->type = FD_SYNC_FILE;
  f->sync_file_fence = fence;
  fd_install(proc->proc->files, fd, f);
  spin_unlock(&proc->proc->files->fd_lock);
  return fd;
}

/* FD_SYNC_FILE (plan2) lookup: return the fence bound to a sync_file fd, or
 * NULL if the fd is not a sync_file. Driver syncobj-import path uses this
 * instead of dereferencing struct file / fd table directly. The fence ref is
 * not borrowed here — the fd still owns it for its lifetime. */
struct drm_fence *bsd_sync_file_fd_fence(xtask *proc, int fd) {
  struct file *f = fd_lookup(proc->proc->files, fd);
  if (!f || f->type != FD_SYNC_FILE)
    return NULL;
  return f->sync_file_fence;
}

void pty_dup_file(struct file *f) {
  struct pty *pty = f->pty;
  if (!pty)
    return;
  if (pty_is_master_inode(f->inode))
    pty->master_refs++;
  else
    pty->slave_refs++;
}

void pty_close_file(struct file *f) {
  struct pty *pty = f->pty;
  if (!pty)
    return;
  int is_master = pty_is_master_inode(f->inode);

  if (is_master) {
    pty->master_refs--;
    if (pty->master_refs == 0) {
      if (pty->t_sid != 0) {
        for (int p = 0; p < MAX_PROC; p++) {
          if (tasks[p] && tasks[p]->pid == p && tasks[p]->proc &&
              tasks[p]->proc->pgid == pty->t_pgid &&
              tasks[p]->proc->sid == pty->t_sid) {
            __atomic_or_fetch(&tasks[p]->proc->sig_pending, SIGMASK(SIGHUP),
                              __ATOMIC_RELEASE);
            // SIGHUP must interrupt any blocking state (including
            // WAIT_FUTEX/WAIT_CHILD); wake_process_any unconditionally wakes
            // any BLOCKED target regardless of event type.
            if (tasks[p]->state == BLOCKED)
              wake_process_any(tasks[p]);
          }
        }
      }
      __wake_up(pty->wq, POLLHUP | POLLIN | POLLOUT);
    }
  } else {
    pty->slave_refs--;
    if (pty->slave_refs == 0) {
      // Clear m_to_s buffer so master read won't get stale data
      pty->m_to_s_head = 0;
      pty->m_to_s_tail = 0;
      // Wake master read — it gets EOF
      __wake_up(pty->wq, POLLHUP | POLLIN);
      pty->slave_opened = 0;

      // Remove /dev/ptsN from devtmpfs
      char name[16] = "pts";
      int pos = 3;
      int idx = pty->index;
      if (idx == 0) {
        name[pos++] = '0';
      } else {
        char tmp[8];
        int tpos = 0;
        int n = idx;
        while (n > 0) {
          tmp[tpos++] = '0' + (n % 10);
          n /= 10;
        }
        for (int i = tpos - 1; i >= 0; i--)
          name[pos++] = tmp[i];
      }
      name[pos] = '\0';
      devtmpfs_remove(name);

      if (pty->pts_priv) {
        kfree(pty->pts_priv);
        pty->pts_priv = NULL;
      }
    }
  }

  // Free pty when both sides are fully closed
  if (pty->master_refs == 0 && pty->slave_refs == 0) {
    pty_free(pty);
  }
}

void files_put(files *files) {
  if (!files)
    return;
  if (refcount_dec_and_test(&files->f_count)) {
    struct file *entries[MAX_FD];
    for (int fd = 0; fd < MAX_FD; fd++) {
      entries[fd] = fd_uninstall(files, fd);
    }
    synchronize_rcu();
    for (int fd = 0; fd < MAX_FD; fd++) {
      if (entries[fd])
        file_put(entries[fd]);
    }
    kfree(files);
  }
}

// Free a page table page by physical address
static void free_table_page(uint64_t phys) {
  struct page *p = &bfc_frames[PHY_TO_PAGE(phys)];
  // Unrecoverable: a page-table page being freed as a slab page or already
  // free page means the Page descriptor or page table has been corrupted.
  // DEBUG panics directly to locate; release builds do not check.
  ASSERT(p->status != PAGE_SLAB && p->status != PAGE_FREE);
  bfc_free_page(p, 1);
}

// ===================== mm lifecycle =====================

mm *mm_create(void) {
  struct mm *mm = (struct mm *)kmalloc(sizeof(struct mm));
  if (!mm)
    return NULL;
  __memset(mm, 0, sizeof(struct mm));

  // Allocate PML4
  struct page *pml4_page = bfc_alloc_page(1);
  if (!pml4_page) {
    kfree(mm);
    return NULL;
  }
  uint64_t pml4_phys = (__force uint64_t)page_to_phys(pml4_page);
  uint64_t pml4_virt =
      (__force uint64_t)phys_to_virt((__force phys_addr_t)pml4_phys);

  uint64_t *new_pml4 = (uint64_t *)pml4_virt;
  for (int i = 0; i < 512; i++)
    new_pml4[i] = 0;
  new_pml4[511] = pml4[511]; // kernel mapping
#ifdef SANITIZER
  new_pml4[503] =
      pml4[503]; // KASAN shadow (PML4[503], disjoint from direct map PML4[511])
#endif

  mm->cr3 = pml4_phys;
  refcount_set(&mm->m_count, 1);
  mm->parent_pid = -1;
  mm->mmap_lock = SPINLOCK_INIT;
  mm->mmap_brk = 0x800000;
  mm->mmap_phys_brk = MAP_PHYSICAL_BASE;

  // files is now created by proc_create(), not by mm_create()
  return mm;
}

void mm_release(mm *mm, pid_t owner_pid) {
  if (!mm)
    return;
  uint64_t *pml4_virt =
      (__force uint64_t *)phys_to_virt((__force phys_addr_t)mm->cr3);

  // 1. Walk user PML4 entries (0-255, canonical low half), free leaf pages +
  // page table pages
  for (int pml4_idx = 0; pml4_idx < 256; pml4_idx++) {
    uint64_t pdpt_entry = pml4_virt[pml4_idx];
    if (!(pdpt_entry & PTE_PRESENT))
      continue;

    uint64_t pdpt_phys = pdpt_entry & 0x000FFFFFFFFFF000ULL;
    uint64_t *pdpt_virt =
        (__force uint64_t *)phys_to_virt((__force phys_addr_t)pdpt_phys);

    for (int pdpt_idx = 0; pdpt_idx < 512; pdpt_idx++) {
      uint64_t pd_entry = pdpt_virt[pdpt_idx];
      if (!(pd_entry & PTE_PRESENT))
        continue;
      if (pd_entry & PTE_PS)
        continue;

      uint64_t pd_phys = pd_entry & 0x000FFFFFFFFFF000ULL;
      uint64_t *pd_virt =
          (__force uint64_t *)phys_to_virt((__force phys_addr_t)pd_phys);

      for (int pd_idx = 0; pd_idx < 512; pd_idx++) {
        uint64_t pt_entry = pd_virt[pd_idx];
        if (!(pt_entry & PTE_PRESENT))
          continue;
        if (pt_entry & PTE_PS)
          continue;

        uint64_t pt_phys = pt_entry & 0x000FFFFFFFFFF000ULL;
        uint64_t *pt_virt =
            (__force uint64_t *)phys_to_virt((__force phys_addr_t)pt_phys);

        for (int pt_idx = 0; pt_idx < 512; pt_idx++) {
          uint64_t pte = pt_virt[pt_idx];
          if (pte_present(pte)) {
            uint64_t leaf_phys = pte & 0x000FFFFFFFFFF000ULL;
            // Check mmap_regions: skip SHM fd mappings and MAP_PHYSICAL
            bool is_shared = false;
            for (mmap_region *mr = mm->mmap_regions; mr; mr = mr->next) {
              if (mr->shm_obj != NULL) {
                shm *s = mr->shm_obj;
                if (s->page_list) {
                  for (int pi = 0; pi < s->num_pages; pi++) {
                    if (leaf_phys == s->page_list[pi]) {
                      is_shared = true;
                      break;
                    }
                  }
                  if (is_shared)
                    break;
                } else if (s->phys != 0 && s->npages > 0) {
                  if (leaf_phys >= s->phys &&
                      leaf_phys < s->phys + s->npages * PAGE_SIZE) {
                    is_shared = true;
                    break;
                  }
                }
              }
              if (mr->phys != 0 && leaf_phys >= mr->phys &&
                  leaf_phys < mr->phys + mr->size) {
                is_shared = true;
                break;
              }
            }
            // Skip shared sig trampoline page
            if (sig_trampoline_phys != 0 && leaf_phys == sig_trampoline_phys) {
              pt_virt[pt_idx] = 0;
              continue;
            }
            if (!is_shared) {
              struct page *leaf_page = &bfc_frames[PHY_TO_PAGE(leaf_phys)];
              // Unrecoverable: a user page-table leaf pointing to a slab page
              // means the page table or Page descriptor has been corrupted.
              // DEBUG panics directly to locate; release builds do not check.
              ASSERT(leaf_page->status != PAGE_SLAB);
              if (refcount_dec_and_test(&leaf_page->p_refcount)) {
                bfc_free_page(leaf_page, 1);
              }
            }
            pt_virt[pt_idx] = 0;
          }
        }
        free_table_page(pt_phys);
        pd_virt[pd_idx] = 0;
      }
      free_table_page(pd_phys);
      pdpt_virt[pdpt_idx] = 0;
    }
    free_table_page(pdpt_phys);
    pml4_virt[pml4_idx] = 0;
  }

  // 2. Free PML4 page itself
  free_table_page(mm->cr3);

  // 3. Free mmap region metadata + release SHM references
  mmap_region *region = mm->mmap_regions;
  while (region) {
    mmap_region *next = region->next;
    if (region->shm_obj) {
      shm_put(region->shm_obj);
    }
    kfree(region);
    region = next;
  }

  // 4. Release files (closes all fds) — now done via proc_free, not mm_release

  // 5. Clear devtmpfs entries and ISR driver PID for this PID
  if (owner_pid >= 0) {
    devtmpfs_cleanup_pid(owner_pid);
    irq_owner_cleanup(owner_pid);

    // Wake any processes waiting for REQ reply from this process
    for (int i = 0; i < MAX_PROC; i++) {
      if (tasks[i] && tasks[i]->pid >= 0 && tasks[i]->state == BLOCKED &&
          tasks[i]->wait_event == WAIT_REQ_REPLY &&
          tasks[i]->req_target_pid == owner_pid) {
        int wcpu = tasks[i]->assigned_cpu;
        uint64_t wflags;
        spin_lock_irqsave(&cpu_locals[wcpu].scheduler_lock, &wflags);
        if (tasks[i]->state == BLOCKED &&
            tasks[i]->wait_event == WAIT_REQ_REPLY) {
          tasks[i]->req_result = ESRCH;
          wake_from_wait(tasks[i]);
        }
        spin_unlock_irqrestore(&cpu_locals[wcpu].scheduler_lock, wflags);
      }
    }

    // Wake any processes waiting for MSG reply from this process
    for (int i = 0; i < MAX_PROC; i++) {
      if (tasks[i] && tasks[i]->pid >= 0 && tasks[i]->state == BLOCKED &&
          tasks[i]->wait_event == WAIT_MSG_REPLY &&
          tasks[i]->msg_target_pid == owner_pid) {
        int wcpu = tasks[i]->assigned_cpu;
        uint64_t wflags;
        spin_lock_irqsave(&cpu_locals[wcpu].scheduler_lock, &wflags);
        if (tasks[i]->state == BLOCKED &&
            tasks[i]->wait_event == WAIT_MSG_REPLY) {
          tasks[i]->msg_result = -ESRCH;
          wake_from_wait(tasks[i]);
        }
        spin_unlock_irqrestore(&cpu_locals[wcpu].scheduler_lock, wflags);
      }
    }
  }

  kfree(mm);
}

void mm_put(mm *mm) {
  if (!mm)
    return;
  if (refcount_dec_and_test(&mm->m_count)) {
    mm_release(mm, -1);
  }
}

void mm_release_pages(mm *mm) {
  if (!mm)
    return;
  uint64_t *pml4_virt =
      (__force uint64_t *)phys_to_virt((__force phys_addr_t)mm->cr3);

  for (int pml4_idx = 0; pml4_idx < 256; pml4_idx++) {
    uint64_t pdpt_entry = pml4_virt[pml4_idx];
    if (!(pdpt_entry & PTE_PRESENT))
      continue;

    uint64_t pdpt_phys = pdpt_entry & 0x000FFFFFFFFFF000ULL;
    uint64_t *pdpt_virt =
        (__force uint64_t *)phys_to_virt((__force phys_addr_t)pdpt_phys);

    for (int pdpt_idx = 0; pdpt_idx < 512; pdpt_idx++) {
      uint64_t pd_entry = pdpt_virt[pdpt_idx];
      if (!(pd_entry & PTE_PRESENT))
        continue;
      if (pd_entry & PTE_PS)
        continue;

      uint64_t pd_phys = pd_entry & 0x000FFFFFFFFFF000ULL;
      uint64_t *pd_virt =
          (__force uint64_t *)phys_to_virt((__force phys_addr_t)pd_phys);

      for (int pd_idx = 0; pd_idx < 512; pd_idx++) {
        uint64_t pt_entry = pd_virt[pd_idx];
        if (!(pt_entry & PTE_PRESENT))
          continue;
        if (pt_entry & PTE_PS)
          continue;

        uint64_t pt_phys = pt_entry & 0x000FFFFFFFFFF000ULL;
        uint64_t *pt_virt =
            (__force uint64_t *)phys_to_virt((__force phys_addr_t)pt_phys);

        for (int pt_idx = 0; pt_idx < 512; pt_idx++) {
          uint64_t pte = pt_virt[pt_idx];
          if (pte_present(pte)) {
            uint64_t leaf_phys = pte & 0x000FFFFFFFFFF000ULL;
            bool skip = false;
            for (mmap_region *mr = mm->mmap_regions; mr; mr = mr->next) {
              if (mr->shm_obj) {
                shm *s = mr->shm_obj;
                if (s->page_list) {
                  for (int pi = 0; pi < s->num_pages; pi++)
                    if (leaf_phys == s->page_list[pi]) {
                      skip = true;
                      break;
                    }
                } else if (s->phys && s->npages) {
                  if (leaf_phys >= s->phys &&
                      leaf_phys < s->phys + s->npages * PAGE_SIZE) {
                    skip = true;
                    break;
                  }
                }
              }
              if (mr->phys && leaf_phys >= mr->phys &&
                  leaf_phys < mr->phys + mr->size) {
                skip = true;
                break;
              }
            }
            if (sig_trampoline_phys && leaf_phys == sig_trampoline_phys)
              skip = true;
            if (!skip) {
              struct page *leaf_page = &bfc_frames[PHY_TO_PAGE(leaf_phys)];
              if (refcount_dec_and_test(&leaf_page->p_refcount)) {
                bfc_free_page(leaf_page, 1);
              }
            }
            pt_virt[pt_idx] = 0;
          }
        }
        free_table_page(pt_phys);
        pd_virt[pd_idx] = 0;
      }
      free_table_page(pd_phys);
      pdpt_virt[pdpt_idx] = 0;
    }
    free_table_page(pdpt_phys);
    pml4_virt[pml4_idx] = 0;
  }

  // Free PML4
  free_table_page(mm->cr3);
}

// ===================== fork helpers =====================

// Build child kernel stack from parent trapframe: trapframe (rax=new_rax) +
// switch_frame. Returns k_rsp. Used by sys_fork and sys_clone.
uint64_t build_kstack_from_tf(uint64_t k_stack_top, trapframe *parent_tf,
                              uint64_t new_rax) {
  trapframe tf;
  __memcpy(&tf, parent_tf, sizeof(trapframe));
  tf.rax = new_rax;

  typedef struct {
    uint64_t rbx;
    uint64_t rbp;
    uint64_t r12;
    uint64_t r13;
    uint64_t r14;
    uint64_t r15;
    uint64_t ret_addr;
  } local_switch_frame;

  local_switch_frame sf = {0};
  sf.ret_addr = (uint64_t)process_entry;

  uint8_t *sp = (uint8_t *)k_stack_top;
  sp -= sizeof(trapframe);
  __memcpy(sp, &tf, sizeof(trapframe));
  sp -= sizeof(local_switch_frame);
  __memcpy(sp, &sf, sizeof(local_switch_frame));
  return (uint64_t)sp;
}

// Deep-copy fd_table from parent_files to child_files.
// Bumps ref counts for pipe, SHM, file, inode, socket, TTY.
// S06: also copies the per-fd close_on_exec bitmap so the child inherits the
// parent's cloexec settings (Linux: fork/clone-without-CLONE_FILES copies).
static void __attribute__((unused)) copy_fd_table(files *parent_files,
                                                  files *child_files) {
  for (int fd = 0; fd < MAX_FD; fd++) {
    struct file *f = parent_files->fd_table[fd];
    child_files->fd_table[fd] = f;
    if (f) {
      file_get(f);
      if (f->type == FD_TTY)
        pty_dup_file(f);
    }
  }
  __memcpy(child_files->close_on_exec, parent_files->close_on_exec,
           sizeof(child_files->close_on_exec));
}

// Deep-copy mmap_regions linked list from parent to child.
// SHM refs are bumped.
__attribute__((unused)) static mmap_region *
copy_mmap_regions(mmap_region *src) {
  mmap_region *head = NULL, *tail = NULL;
  for (mmap_region *mr = src; mr; mr = mr->next) {
    mmap_region *new_mr = (mmap_region *)kmalloc(sizeof(mmap_region));
    if (!new_mr)
      return head; // partial copy, caller handles
    *new_mr = *mr;
    new_mr->next = NULL;
    if (mr->shm_obj)
      shm_get(mr->shm_obj);
    if (!head)
      head = new_mr;
    else
      tail->next = new_mr;
    tail = new_mr;
  }
  return head;
}

// ===================== sys_fork =====================

int64_t sys_fork(int64_t a1, int64_t a2, int64_t a3, int64_t a4, int64_t a5,
                 int64_t a6) {
  (void)a1;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  (void)a6;
  xtask *parent = current_task;

  // 1. Allocate new xtask slot
  spin_lock(&tasks_lock);
  int alloc_idx = -1;
  xtask *child = xtask_alloc(&alloc_idx);
  if (!child) {
    spin_unlock(&tasks_lock);
    return (int64_t)-ENOMEM;
  }

  // 2. Allocate new mm
  mm *child_mm = mm_create();
  if (!child_mm) {
    spin_unlock(&tasks_lock);
    return (int64_t)-ENOMEM;
  }

  // 3. Deep-copy parent user page tables
  uint64_t *src_pml4 =
      (uint64_t *)phys_to_virt((__force phys_addr_t)parent->mm->cr3);
  uint64_t *dst_pml4 =
      (uint64_t *)phys_to_virt((__force phys_addr_t)child_mm->cr3);
  int ret = copy_page_table(src_pml4, dst_pml4, parent->mm->mmap_regions);
  if (ret < 0) {
    mm_put(child_mm);
    spin_unlock(&tasks_lock);
    return (int64_t)ret;
  }

  // Flush parent's TLB — copy_page_table modified parent's RW PTEs to COW,
  // stale TLB entries would still show the old writable mappings
  load_cr3(parent->mm->cr3);

  // 4. Create child proc + copy fd_table
  proc *child_bp = proc_create();
  if (!child_bp) {
    mm_put(child_mm);
    spin_unlock(&tasks_lock);
    return (int64_t)-ENOMEM;
  }
  child->proc = child_bp;
  child_bp->xtask = child;
  copy_fd_table(parent->proc->files, child_bp->files);

  // 5. Copy mmap_regions (including user stack region)
  child_mm->mmap_regions = copy_mmap_regions(parent->mm->mmap_regions);
  child_mm->mmap_brk = parent->mm->mmap_brk;
  child_mm->mmap_phys_brk = parent->mm->mmap_phys_brk;
  child_mm->parent_pid = parent->pid;

  // 6. Allocate new kernel stack, copy parent trapframe (rax=0 for child
  // return)
  struct page *stack_pages = bfc_alloc_page(KERNEL_STACK_PAGES);
  if (!stack_pages) {
    mm_put(child_mm);
    spin_unlock(&tasks_lock);
    return (int64_t)-ENOMEM;
  }
  uint64_t k_stack_phys = (__force uint64_t)page_to_phys(stack_pages);
  uint64_t k_stack_top =
      (__force uint64_t)phys_to_virt((__force phys_addr_t)k_stack_phys) +
      KERNEL_STACK_SIZE;

  // 6b. FPU inheritance: child pre-allocates fpu_page + copies parent's
  // current FPU snapshot (POSIX fork semantics)
  if (!xcore_fpu_alloc(child)) {
    bfc_free_page(stack_pages, KERNEL_STACK_PAGES);
    proc_free(child_bp);
    child->proc = NULL;
    mm_put(child_mm);
    spin_unlock(&tasks_lock);
    return (int64_t)-ENOMEM;
  }
  ASSERT(parent->fpu_page != NULL);
  void *parent_fpu =
      (void *)(__force uintptr_t)phys_to_virt(page_to_phys(parent->fpu_page));
  void *child_fpu =
      (void *)(__force uintptr_t)phys_to_virt(page_to_phys(child->fpu_page));
  kernel_fpu_save(parent_fpu); // save parent's current live xmm (fxsave does
                               // not modify registers, parent keeps running)
  __memcpy(child_fpu, parent_fpu, PAGE_SIZE);

  // 6. Build child kernel stack (reuse build_kstack_from_tf helper)
  trapframe *parent_tf = get_cpu_local()->cur_tf;
  uint64_t k_rsp = build_kstack_from_tf(k_stack_top, parent_tf, 0);

  // 7. Fill child xtask
  // Ordering constraint: assigned_cpu must be set before pid (the wake path
  // reads assigned_cpu locklessly before taking the lock, prevent OOB if pid
  // takes effect while assigned_cpu is still -1); affinity to parent CPU
  child->assigned_cpu = sched_pick_cpu_pref(parent->assigned_cpu);
  child->pid = alloc_idx;
  child->state = READY;
  child->k_rsp = k_rsp;
  child->k_stack_top = k_stack_top;
  child->cr3 = child_mm->cr3; // cached
  child->entry = parent->entry;
  child->wait_event = WAIT_NONE;
  child->tgid = child->pid;
  child->mm = child_mm;
  // fs_base inheritance: fork child shares parent's address space (COW), and
  // the parent TCB page mapping remains in the child's space, so the child
  // must inherit parent's fs_base to access errno/TLS. Missing it leaves child
  // FS_BASE=0, and the first errno read (mov %fs:0) triggers #PF and gets
  // killed (e.g. libc wrapper writing errno after execve failure).
  // sys_clone's CLONE_SETTLS branch handles TLS separately; the fork path here
  // always inherits the parent's value.
  child->fs_base = parent->fs_base;
  child->iopm = NULL; // fork does not inherit IOPM
  child->recv_head = 0;
  child->recv_tail = 0;
  child->recv_lock = SPINLOCK_INIT;
  child->req_caller_pid = -1;
  child->req_reply_buf = NULL;
  child->req_replied = 0;
  child->msg_caller_pid = -1;
  child->msg_reply_buf = NULL;
  child->msg_replied = 0;
  child->cpu_time_ns = 0;
  child->last_sched = 0;
  child->exit_code = 0;
  // POSIX fields are in proc
  child_bp->exit_code = 0;
  // signal_struct: fork independent copy (no CLONE_SIGHAND)
  // proc_create already allocated a fresh signal_struct; deep-copy action[]
  __memcpy(child_bp->signal->action, parent->proc->signal->action,
           sizeof(parent->proc->signal->action));
  child_bp->signal->shared_pending = 0;
  child_bp->signal->group_exit = 0;
  child_bp->signal->group_exit_code = 0;
  child_bp->signal->parent_pid = parent->pid;
  atomic_set(&child_bp->signal->thread_count, 1);
  atomic_set(&child_bp->signal->live_count, 1);
  // per-task signal fields
  child_bp->sig_pending = 0;
  child_bp->sig_blocked = parent->proc->sig_blocked;
  // threading fields
  child_bp->clear_tid_addr = NULL;
  list_init(&child_bp->futex_node);
  child_bp->futex_uaddr = 0;
  child_bp->sid = parent->proc->sid;
  child_bp->pgid = parent->proc->pgid;
  child_bp->ctty = parent->proc->ctty;
  // Inherit POSIX identity & permissions. alarm_deadline is NOT inherited —
  // a fresh child gets a new signal_struct (zero-initialized) with no alarm.
  child_bp->uid = parent->proc->uid;
  child_bp->euid = parent->proc->euid;
  child_bp->gid = parent->proc->gid;
  child_bp->egid = parent->proc->egid;
  child_bp->umask = parent->proc->umask;
  list_init(&child->run_node);
  list_init(&child->wait_node);

  spin_unlock(&tasks_lock);

  // 8. Enqueue to scheduler
  int cpu = child->assigned_cpu;
  uint64_t rflags;
  spin_lock_irqsave(&cpu_locals[cpu].scheduler_lock, &rflags);
  // TOCTOU recheck: if the target CPU has been filled past the threshold
  // during enqueue, re-pick a CPU
  if (__atomic_load_n(&cpu_locals[cpu].run_count, __ATOMIC_RELAXED) >
      RECHECK_THRESHOLD) {
    int new_cpu = sched_pick_cpu_pref(parent->assigned_cpu);
    if (new_cpu != cpu) {
      spin_unlock_irqrestore(&cpu_locals[cpu].scheduler_lock, rflags);
      child->assigned_cpu = new_cpu; // not yet enqueued, safe to mutate field
      cpu = new_cpu;
      spin_lock_irqsave(&cpu_locals[cpu].scheduler_lock, &rflags);
    }
  }
  run_queue_push(cpu, child);
  spin_unlock_irqrestore(&cpu_locals[cpu].scheduler_lock, rflags);

  // 9. Parent returns child PID
  printk(LOG_INFO, "sys_fork: parent=%d → child=%d\n", parent->pid, child->pid);
  return (int64_t)child->pid;
}

// ===================== sys_clone =====================

// clone flag definitions (matches Linux)
#define CLONE_VM 0x00000100
#define CLONE_FILES 0x00000400
#define CLONE_SIGHAND 0x00000800
#define CLONE_THREAD 0x00010000
#define CLONE_PARENT_SETTID 0x00100000
#define CLONE_CHILD_CLEARTID 0x00200000
#define CLONE_CHILD_SETTID 0x01000000
#define CLONE_SETTLS 0x00080000

int64_t sys_clone(int64_t arg1, int64_t arg2, int64_t arg3, int64_t arg4,
                  int64_t arg5) {
  uint64_t flags = (uint64_t)arg1;
  uint64_t stack = (uint64_t)arg2;
  uint64_t parent_tid = (uint64_t)arg3;
  uint64_t child_tid = (uint64_t)arg4;
  uint64_t tls = (uint64_t)arg5;
  xtask *parent = current_task;

  // flag combination constraint validation
  if ((flags & CLONE_SIGHAND) && !(flags & CLONE_VM))
    return (int64_t)-EINVAL;
  if ((flags & CLONE_THREAD) && !(flags & CLONE_SIGHAND))
    return (int64_t)-EINVAL;
  if ((flags & CLONE_VM) && stack == 0)
    return (int64_t)-EINVAL;

  // 1. Allocate an xtask slot
  spin_lock(&tasks_lock);
  int alloc_idx = -1;
  xtask *child = xtask_alloc(&alloc_idx);
  if (!child) {
    spin_unlock(&tasks_lock);
    return (int64_t)-ENOMEM;
  }

  // 2. Allocate kernel stack
  struct page *stack_pages = bfc_alloc_page(KERNEL_STACK_PAGES);
  if (!stack_pages) {
    spin_unlock(&tasks_lock);
    return (int64_t)-ENOMEM;
  }
  uint64_t k_stack_phys = (__force uint64_t)page_to_phys(stack_pages);
  uint64_t k_stack_top =
      (__force uint64_t)phys_to_virt((__force phys_addr_t)k_stack_phys) +
      KERNEL_STACK_SIZE;

  // 2b. FPU state: child thread pre-allocates fpu_page + copies parent
  // thread's current FPU snapshot
  if (!xcore_fpu_alloc(child)) {
    bfc_free_page(stack_pages, KERNEL_STACK_PAGES);
    spin_unlock(&tasks_lock);
    return (int64_t)-ENOMEM;
  }
  ASSERT(parent->fpu_page != NULL);
  void *parent_fpu =
      (void *)(__force uintptr_t)phys_to_virt(page_to_phys(parent->fpu_page));
  void *child_fpu =
      (void *)(__force uintptr_t)phys_to_virt(page_to_phys(child->fpu_page));
  kernel_fpu_save(parent_fpu);
  __memcpy(child_fpu, parent_fpu, PAGE_SIZE);

  // 3. mm
  mm *child_mm;
  if (flags & CLONE_VM) {
    child_mm = parent->mm;
    refcount_inc(&child_mm->m_count);
    child->cr3 = parent->cr3;
  } else {
    child_mm = mm_create();
    if (!child_mm) {
      bfc_free_page(child->fpu_page, 1);
      child->fpu_page = NULL;
      bfc_free_page(stack_pages, KERNEL_STACK_PAGES);
      spin_unlock(&tasks_lock);
      return (int64_t)-ENOMEM;
    }
    uint64_t *src_pml4 =
        (uint64_t *)phys_to_virt((__force phys_addr_t)parent->mm->cr3);
    uint64_t *dst_pml4 =
        (uint64_t *)phys_to_virt((__force phys_addr_t)child_mm->cr3);
    int ret = copy_page_table(src_pml4, dst_pml4, parent->mm->mmap_regions);
    if (ret < 0) {
      mm_put(child_mm);
      bfc_free_page(child->fpu_page, 1);
      child->fpu_page = NULL;
      bfc_free_page(stack_pages, KERNEL_STACK_PAGES);
      spin_unlock(&tasks_lock);
      return (int64_t)ret;
    }
    load_cr3(parent->mm->cr3);
    child_mm->mmap_regions = copy_mmap_regions(parent->mm->mmap_regions);
    child_mm->mmap_brk = parent->mm->mmap_brk;
    child_mm->mmap_phys_brk = parent->mm->mmap_phys_brk;
    child_mm->parent_pid = parent->pid;
    child->cr3 = child_mm->cr3;
  }

  // 4. files
  proc *child_bp = proc_create();
  if (!child_bp) {
    if (!(flags & CLONE_VM))
      mm_put(child_mm);
    bfc_free_page(child->fpu_page, 1);
    child->fpu_page = NULL;
    bfc_free_page(stack_pages, KERNEL_STACK_PAGES);
    spin_unlock(&tasks_lock);
    return (int64_t)-ENOMEM;
  }
  child->proc = child_bp;
  child_bp->xtask = child;
  struct files *deferred_files = NULL; // CLONE_FILES: defer files_put outside
                                       // tasks_lock (files_put→synchronize_rcu
                                       // under tasks_lock deadlocks with
                                       // do_exit spinning on tasks_lock)
  if (flags & CLONE_FILES) {
    deferred_files = child_bp->files;
    child_bp->files = parent->proc->files;
    refcount_inc(&child_bp->files->f_count);
  } else {
    copy_fd_table(parent->proc->files, child_bp->files);
  }

  // 5. signal_struct
  if (flags & CLONE_SIGHAND) {
    signal_put(
        child_bp->signal); // release the default one created by proc_create
    child_bp->signal = parent->proc->signal;
    refcount_inc(&child_bp->signal->sig_count);
  } else {
    __memcpy(child_bp->signal->action, parent->proc->signal->action,
             sizeof(parent->proc->signal->action));
    child_bp->signal->shared_pending = 0;
    child_bp->signal->group_exit = 0;
    child_bp->signal->group_exit_code = 0;
    child_bp->signal->parent_pid = parent->pid;
    atomic_set(&child_bp->signal->thread_count, 1);
    atomic_set(&child_bp->signal->live_count, 1);
  }

  // 6. tgid + thread-group count
  if (flags & CLONE_THREAD) {
    child->tgid = parent->tgid;
    atomic_inc(&child_bp->signal->thread_count);
    atomic_inc(&child_bp->signal->live_count);
  } else {
    child->tgid = child->pid;
  }

  // 7. trapframe: rax=0, rsp=(CLONE_VM? stack : parent_tf->rsp)
  trapframe *parent_tf = get_cpu_local()->cur_tf;
  uint64_t new_rsp = (flags & CLONE_VM) ? stack : parent_tf->rsp;
  // Temporarily modify parent_tf->rsp to reuse build_kstack_from_tf
  uint64_t saved_rsp = parent_tf->rsp;
  parent_tf->rsp = new_rsp;
  uint64_t k_rsp = build_kstack_from_tf(k_stack_top, parent_tf, 0);
  parent_tf->rsp = saved_rsp;

  // 8. fs_base + proc thread-level fields
  child->fs_base = (flags & CLONE_SETTLS) ? tls : parent->fs_base;
  if (flags & CLONE_THREAD) {
    // §4.5:TLS/栈信息由 sys_pthread_setup 预置到 current_task,此处消费即清;
    // 未预置则为零值(非 pthread 的 CLONE_THREAD 不存在)。
    struct thread_clone_info ci = current_task->pending_pthread_setup;
    __memset(&current_task->pending_pthread_setup, 0,
             sizeof(struct thread_clone_info));
    child->detached = ci.detached;
    child->tls_page = ci.tls_page;
    child->tls_total = ci.tls_total;
    child->user_stack_base = ci.user_stack_base;
    child->user_stack_size = ci.user_stack_size;
  }
  child_bp->sig_pending = 0;
  child_bp->sig_blocked = parent->proc->sig_blocked;
  child_bp->exit_code = 0;
  child_bp->futex_uaddr = 0;
  list_init(&child_bp->futex_node);
  child_bp->clear_tid_addr =
      (flags & CLONE_CHILD_CLEARTID) ? (void *)(uintptr_t)child_tid : NULL;

  // 9. CLONE_PARENT_SETTID / CLONE_CHILD_SETTID
  //    CHILD_SETTID must be written before the child thread is scheduled,
  //    otherwise the child's clear_tid_addr write of 0 + futex_wake on exit
  //    would be lost (the parent only writes tid after clone returns,
  //    overwriting 0 -> lost wake-up).
  //    S03: the tid address is a full 64-bit user int* (set_tid_address /
  //    clone child_tid are user pointers, never truncated).
  if (flags & CLONE_PARENT_SETTID) {
    *((int *)(uintptr_t)parent_tid) = (int)alloc_idx;
  }
  if (flags & CLONE_CHILD_SETTID) {
    *((int *)(uintptr_t)child_tid) = (int)alloc_idx;
  }

  // 10. Fill child xtask
  // Ordering constraint: assigned_cpu must be set before pid; CLONE_THREAD
  // has affinity to the parent CPU, otherwise no affinity
  child->assigned_cpu = (flags & CLONE_THREAD)
                            ? sched_pick_cpu_pref(parent->assigned_cpu)
                            : sched_pick_cpu();
  child->pid = alloc_idx;
  child->state = READY;
  child->k_rsp = k_rsp;
  child->k_stack_top = k_stack_top;
  child->entry = parent->entry;
  child->wait_event = WAIT_NONE;
  child->mm = child_mm;
  child->iopm = NULL;
  child->recv_head = 0;
  child->recv_tail = 0;
  child->recv_lock = SPINLOCK_INIT;
  child->req_caller_pid = -1;
  child->req_reply_buf = NULL;
  child->req_replied = 0;
  child->msg_caller_pid = -1;
  child->msg_reply_buf = NULL;
  child->msg_replied = 0;
  child->cpu_time_ns = 0;
  child->last_sched = 0;
  child->exit_code = 0;
  list_init(&child->run_node);
  list_init(&child->wait_node);

  // Inherit session/job control when not CLONE_THREAD
  if (!(flags & CLONE_THREAD)) {
    child_bp->sid = parent->proc->sid;
    child_bp->pgid = parent->proc->pgid;
    child_bp->ctty = parent->proc->ctty;
    // Inherit POSIX identity & permissions. alarm_deadline is NOT inherited
    // (a fresh child gets a new signal_struct, zero-initialized).
    child_bp->uid = parent->proc->uid;
    child_bp->euid = parent->proc->euid;
    child_bp->gid = parent->proc->gid;
    child_bp->egid = parent->proc->egid;
    child_bp->umask = parent->proc->umask;
  }

  spin_unlock(&tasks_lock);

  // Deferred CLONE_FILES cleanup: release the default files created by
  // proc_create. Must be outside tasks_lock because files_put calls
  // synchronize_rcu, which waits for all CPUs to reach quiescence. Under
  // tasks_lock, a thread on another CPU spinning on tasks_lock (e.g. do_exit)
  // cannot reach quiescence → RCU stall deadlock.
  if (deferred_files)
    files_put(deferred_files);

  // 11. Enqueue to scheduler
  int cpu = child->assigned_cpu;
  uint64_t rflags;
  spin_lock_irqsave(&cpu_locals[cpu].scheduler_lock, &rflags);
  // TOCTOU recheck: if the target CPU has been filled past the threshold
  // during enqueue, re-pick a CPU
  if (__atomic_load_n(&cpu_locals[cpu].run_count, __ATOMIC_RELAXED) >
      RECHECK_THRESHOLD) {
    int new_cpu = (flags & CLONE_THREAD)
                      ? sched_pick_cpu_pref(parent->assigned_cpu)
                      : sched_pick_cpu();
    if (new_cpu != cpu) {
      spin_unlock_irqrestore(&cpu_locals[cpu].scheduler_lock, rflags);
      child->assigned_cpu = new_cpu; // not yet enqueued, safe to mutate field
      cpu = new_cpu;
      spin_lock_irqsave(&cpu_locals[cpu].scheduler_lock, &rflags);
    }
  }
  run_queue_push(cpu, child);
  spin_unlock_irqrestore(&cpu_locals[cpu].scheduler_lock, rflags);

  return (int64_t)child->pid;
}

// ===================== sys_execve =====================

int64_t sys_execve(int64_t a1, int64_t a2, int64_t a3, int64_t a4, int64_t a5,
                   int64_t a6) {
  (void)a4;
  (void)a5;
  (void)a6;
  const char *pathname = (const char *)a1;
  char **argv_ptr = (char **)a2; // user-space argv[]
  char **envp_ptr = (char **)a3; // user-space envp[]
  xtask *proc = current_task;
  if (!proc->mm)
    return (int64_t)-EINVAL;
  printk(LOG_INFO, "execve: pid=%d path=%s\n", proc->pid, pathname);

  // 1. Open pathname via VFS
  int64_t open_result =
      sys_open((int64_t)(uintptr_t)pathname, O_RDONLY, 0, 0, 0, 0);
  int32_t fd = (int32_t)(open_result & 0xFFFFFFFFULL);
  if (fd < 0) {
    printk(LOG_ERROR, "execve: open failed pid=%d path=%s err=%d\n", proc->pid,
           pathname, fd);
    return (int64_t)fd;
  }

  // 2. Get file size from inode directly
  if (!fd_lookup(proc->proc->files, fd) ||
      fd_lookup(proc->proc->files, fd)->type != FD_REGULAR) {
    sys_close((int64_t)fd, 0, 0, 0, 0, 0);
    return (int64_t)-EIO;
  }
  struct inode *ip = fd_lookup(proc->proc->files, fd)->inode;
  if (!ip) {
    sys_close((int64_t)fd, 0, 0, 0, 0, 0);
    return (int64_t)-EBADF;
  }
  uint32_t saved_ino = ip->ino;
  uint64_t file_size = ip->size;

  // 3. kmalloc buffer, read entire ELF into kernel
  uint8_t *elf_buf = (uint8_t *)kmalloc(file_size);
  if (!elf_buf) {
    sys_close((int64_t)fd, 0, 0, 0, 0, 0);
    return (int64_t)-ENOMEM;
  }

  // Use fat32_read directly (sys_read rejects kernel-space buffers)
  int nread = fat32_read(ip, 0, (void *)elf_buf, file_size);
  sys_close((int64_t)fd, 0, 0, 0, 0, 0);
  ip = NULL; /* ip is now dangling after sys_close — do not dereference */

  if (nread < 0 || (uint64_t)nread < file_size) {
    kfree(elf_buf);
    return (int64_t)-EIO;
  }

  // 4. Validate ELF magic
  if (elf_buf[0] != 0x7F || elf_buf[1] != 'E' || elf_buf[2] != 'L' ||
      elf_buf[3] != 'F') {
    printk(LOG_ERROR,
           "execve: pid=%d path=%s ino=%lu size=%lu bad magic: %02x %02x %02x "
           "%02x\n",
           proc->pid, pathname, (unsigned long)saved_ino,
           (unsigned long)file_size, elf_buf[0], elf_buf[1], elf_buf[2],
           elf_buf[3]);
    kfree(elf_buf);
    return (int64_t)-ENOEXEC;
  }
  printk(LOG_INFO, "execve: pid=%d path=%s size=%lu magic OK\n", proc->pid,
         pathname, (unsigned long)file_size);

  // 5. Allocate new PML4, copy kernel entries (before releasing old space)
  struct page *pml4_page = bfc_alloc_page(1);
  if (!pml4_page) {
    kfree(elf_buf);
    return (int64_t)-ENOMEM;
  }
  uint64_t pml4_phys = (__force uint64_t)page_to_phys(pml4_page);
  uint64_t pml4_virt =
      (__force uint64_t)phys_to_virt((__force phys_addr_t)pml4_phys);
  uint64_t *new_pml4 = (uint64_t *)pml4_virt;
  for (int i = 0; i < 512; i++)
    new_pml4[i] = 0;
  new_pml4[511] = pml4[511];
#ifdef SANITIZER
  new_pml4[503] =
      pml4[503]; // KASAN shadow (PML4[503], disjoint from direct map PML4[511])
#endif

  // 6. elf_load into new PML4
  elf_load_result lr = elf_load(elf_buf, file_size, new_pml4);
  if (!lr.success) {
    kfree(elf_buf);
    free_table_page(pml4_phys);
    printk(LOG_ERROR, "execve: elf_load failed pid=%d\n", proc->pid);
    return (int64_t)-ENOEXEC;
  }

  // 6b. Parse PT_INTERP — detect dynamic executable
  char interp_path[256] = {0};
  bool is_dynamic = false;
  {
    Elf64_Ehdr *eh = (Elf64_Ehdr *)elf_buf;
    for (int i = 0; i < eh->e_phnum; i++) {
      Elf64_Phdr *ph =
          (Elf64_Phdr *)(elf_buf + eh->e_phoff + i * eh->e_phentsize);
      if (ph->p_type == PT_INTERP) {
        if (ph->p_filesz >= sizeof(interp_path)) {
          kfree(elf_buf);
          free_table_page(pml4_phys);
          return (int64_t)-ENOENT;
        }
        __memcpy(interp_path, elf_buf + ph->p_offset, ph->p_filesz);
        interp_path[ph->p_filesz] = '\0';
        is_dynamic = true;
        break;
      }
    }
  }

  // 6c. Dynamic path: load ld.so at LD_SO_BASE
  // interp_path is a kernel stack variable, so use vfs_open_kern (kernel-path
  // open) instead of sys_open (which validates the path as a user pointer).
  elf_load_result ld_lr = {0};
  if (is_dynamic) {
    struct inode *ld_ip = vfs_open_kern(interp_path);
    if (!ld_ip) {
      kfree(elf_buf);
      free_table_page(pml4_phys);
      printk(LOG_ERROR, "execve: ld.so open failed pid=%d path=%s err=%d\n",
             proc->pid, interp_path, ENOENT);
      return (int64_t)-ENOENT;
    }
    uint64_t ld_size = ld_ip->size;
    uint8_t *ld_buf = (uint8_t *)kmalloc(ld_size);
    if (!ld_buf) {
      inode_put(ld_ip);
      kfree(elf_buf);
      free_table_page(pml4_phys);
      return (int64_t)-ENOMEM;
    }
    int ld_nread = fat32_read(ld_ip, 0, (void *)ld_buf, ld_size);
    inode_put(ld_ip);
    if (ld_nread < 0 || (uint64_t)ld_nread < ld_size) {
      kfree(ld_buf);
      kfree(elf_buf);
      free_table_page(pml4_phys);
      return (int64_t)-EIO;
    }
    ld_lr = elf_load_at(ld_buf, ld_size, new_pml4, LD_SO_BASE);
    kfree(ld_buf);
    if (!ld_lr.success) {
      kfree(elf_buf);
      free_table_page(pml4_phys);
      printk(LOG_ERROR, "execve: ld.so load failed pid=%d\n", proc->pid);
      return (int64_t)-ENOEXEC;
    }
  }

  // 7. Allocate new user stack
  int user_stack_pages = 2048;
  struct page *user_stack_page = bfc_alloc_page(user_stack_pages);
  if (!user_stack_page) {
    kfree(elf_buf);
    sys_exit((int64_t)-ENOMEM, 0, 0, 0, 0, 0);
    __builtin_unreachable();
  }
  uint64_t user_stack_phys = (__force uint64_t)page_to_phys(user_stack_page);
  uint64_t stack_base = USER_STACK_TOP - (uint64_t)user_stack_pages * PAGE_SIZE;
  for (int i = 0; i < user_stack_pages; i++) {
    if (!map_user_page_direct(new_pml4, stack_base + i * PAGE_SIZE,
                              user_stack_phys + i * PAGE_SIZE,
                              PTE_PRESENT | PTE_RW | PTE_USER | PTE_NX)) {
      kfree(elf_buf);
      sys_exit((int64_t)-ENOMEM, 0, 0, 0, 0, 0);
      __builtin_unreachable();
    }
  }

  // Map signal trampoline page
  if (sig_trampoline_phys != 0) {
    map_user_page_direct(new_pml4, SIG_TRAMPOLINE_ADDR, sig_trampoline_phys,
                         PTE_PRESENT | PTE_USER);
  }

// === argc/argv/envp/auxv stack construction (standard SysV ABI) ===
// The stack is contiguous physical memory (user_stack_phys, user_stack_pages
// pages); user-space addresses [stack_base, USER_STACK_TOP] are contiguously
// mapped. Note: the argv/envp string buffer is large (ARG_MAX*256*2 ~ 64KB),
// cannot use the kernel stack (only 8KB), so kmalloc is used instead.
#define ARG_MAX 128
  char(*argv_strings)[256] = (char(*)[256])kmalloc(sizeof(char[ARG_MAX][256]));
  char(*envp_strings)[256] = (char(*)[256])kmalloc(sizeof(char[ARG_MAX][256]));
  uint64_t *argv_str_vaddrs = (uint64_t *)kmalloc(sizeof(uint64_t) * ARG_MAX);
  uint64_t *envp_str_vaddrs = (uint64_t *)kmalloc(sizeof(uint64_t) * ARG_MAX);
  if (!argv_strings || !envp_strings || !argv_str_vaddrs || !envp_str_vaddrs) {
    if (argv_strings)
      kfree(argv_strings);
    if (envp_strings)
      kfree(envp_strings);
    if (argv_str_vaddrs)
      kfree(argv_str_vaddrs);
    if (envp_str_vaddrs)
      kfree(envp_str_vaddrs);
    kfree(elf_buf);
    free_table_page(pml4_phys);
    return (int64_t)-ENOMEM;
  }
  int argc = 0;
  if (argv_ptr) {
    while (argc < ARG_MAX) {
      char *p;
      if (copy_from_user(&p, argv_ptr + argc, sizeof(p)))
        break;
      if (!p)
        break;
      if (strncpy_from_user(argv_strings[argc], p, 255) < 0)
        break;
      argv_strings[argc][255] = '\0';
      argc++;
    }
  }
  // If argv is empty, inject pathname as argv[0] per POSIX convention
  if (argc == 0 && pathname) {
    int plen = 0;
    while (pathname[plen] && plen < 255) {
      argv_strings[0][plen] = pathname[plen];
      plen++;
    }
    argv_strings[0][plen] = '\0';
    argc = 1;
  }

  int envc = 0;
  if (envp_ptr) {
    while (envc < ARG_MAX) {
      char *p;
      if (copy_from_user(&p, envp_ptr + envc, sizeof(p)))
        break;
      if (!p)
        break;
      if (strncpy_from_user(envp_strings[envc], p, 255) < 0)
        break;
      envp_strings[envc][255] = '\0';
      envc++;
    }
  }

// STACK_KV: sp_user user-space address -> kernel virtual address (supports
// cross-page)
#define STACK_KV(sp_user)                                                      \
  ((void *)((__force uint64_t)phys_to_virt((__force phys_addr_t)(              \
      user_stack_phys + (uint64_t)user_stack_pages * PAGE_SIZE -               \
      (USER_STACK_TOP - (sp_user))))))

  uint64_t sp_user = USER_STACK_TOP;

  // 1. AT_RANDOM 16 bytes (first zero-fill)
  sp_user -= 16;
  __memset(STACK_KV(sp_user), 0, 16);
  uint64_t at_random_vaddr = sp_user;

  // 2. AT_EXECFN: execve path string copy
  int execfn_len = 0;
  while (pathname[execfn_len])
    execfn_len++;
  sp_user -= (execfn_len + 1);
  __memcpy(STACK_KV(sp_user), pathname, execfn_len + 1);
  uint64_t at_execfn_vaddr = sp_user;

  // 3. envp strings (high address -> low address)
  for (int i = envc - 1; i >= 0; i--) {
    int len = 0;
    while (envp_strings[i][len])
      len++;
    sp_user -= (len + 1);
    __memcpy(STACK_KV(sp_user), envp_strings[i], len + 1);
    envp_str_vaddrs[i] = sp_user;
  }

  // 4. argv strings
  for (int i = argc - 1; i >= 0; i--) {
    int len = 0;
    while (argv_strings[i][len])
      len++;
    sp_user -= (len + 1);
    __memcpy(STACK_KV(sp_user), argv_strings[i], len + 1);
    argv_str_vaddrs[i] = sp_user;
  }

  // 5. 16-byte align sp_user
  while ((sp_user % 16) != 0)
    sp_user--;

  // 6. Write auxv (8 pairs + AT_NULL)
  int auxc = 8;
  sp_user -= (auxc + 1) * 16;
  uint64_t *auxv = (uint64_t *)STACK_KV(sp_user);
  int ai = 0;
  auxv[ai++] = AT_PHDR;
  auxv[ai++] = lr.phdr_vaddr;
  auxv[ai++] = AT_PHENT;
  auxv[ai++] = lr.phent;
  auxv[ai++] = AT_PHNUM;
  auxv[ai++] = lr.phnum;
  auxv[ai++] = AT_ENTRY;
  auxv[ai++] = lr.entry;
  auxv[ai++] = AT_BASE;
  auxv[ai++] = is_dynamic ? ld_lr.load_base : 0;
  auxv[ai++] = AT_PAGESZ;
  auxv[ai++] = PAGE_SIZE;
  auxv[ai++] = AT_RANDOM;
  auxv[ai++] = at_random_vaddr;
  auxv[ai++] = AT_EXECFN;
  auxv[ai++] = at_execfn_vaddr;
  auxv[ai++] = AT_NULL;
  auxv[ai++] = 0;

  // 7. envp pointer array + NULL
  sp_user -= (envc + 1) * 8;
  uint64_t *envp_arr = (uint64_t *)STACK_KV(sp_user);
  envp_arr[envc] = 0;
  for (int i = 0; i < envc; i++)
    envp_arr[i] = envp_str_vaddrs[i];

  // 8. argv pointer array + NULL
  sp_user -= (argc + 1) * 8;
  uint64_t *argv_arr = (uint64_t *)STACK_KV(sp_user);
  argv_arr[argc] = 0;
  for (int i = 0; i < argc; i++)
    argv_arr[i] = argv_str_vaddrs[i];

  // 9. argc
  sp_user -= 8;
  *(uint64_t *)STACK_KV(sp_user) = (uint64_t)argc;

  uint64_t user_sp = sp_user;
#undef STACK_KV
#undef ARG_MAX

  kfree(argv_strings);
  kfree(envp_strings);
  kfree(argv_str_vaddrs);
  kfree(envp_str_vaddrs);

  // === Point of no return: old address space will be replaced ===

  // 8. Close FD_CLOEXEC fds
  // S06: cloexec now lives in the per-fd bitmap (not f->flags), so execve
  // closes exactly the fds whose bitmap bit is set.
  spinlock *fdlk = &proc->proc->files->fd_lock;
  struct file *cloexec_entries[MAX_FD];
  int cloexec_count = 0;
  spin_lock(fdlk);
  for (int i = 0; i < MAX_FD; i++) {
    struct file *f = fd_lookup(proc->proc->files, i);
    if (f && fd_get_cloexec(proc->proc->files, i)) {
      fd_uninstall(proc->proc->files, i);
      fd_set_cloexec(proc->proc->files, i, 0);
      cloexec_entries[cloexec_count++] = f;
    }
  }
  spin_unlock(fdlk);
  if (cloexec_count > 0) {
    synchronize_rcu();
    for (int i = 0; i < cloexec_count; i++)
      file_put(cloexec_entries[i]);
  }

  // 9. Switch to new address space

  // 9a. Modify current trapframe: rip=entry, rsp=user_sp
  trapframe *tf = get_cpu_local()->cur_tf;
  if (is_dynamic) {
    tf->rip = ld_lr.entry; // ld.so entry
    tf->rsp = user_sp;     // points to argc
    printk(LOG_INFO, "exec: ld.so @ 0x%lx, entry 0x%lx, main @ 0x%lx\n",
           ld_lr.load_base, ld_lr.entry, lr.entry);
  } else {
    tf->rip = lr.entry; // main ELF entry (static path)
    tf->rsp = user_sp;  // points to argc
  }
  // User stack top must be 16-byte aligned (see same ASSERT in
  // sched_build_kstack)
  ASSERT(tf->rsp % 16 == 0);
  tf->rax = 0;

  // 9b. Update mm fields
  uint64_t old_cr3 = proc->mm->cr3;
  mmap_region *old_regions = proc->mm->mmap_regions;
  proc->mm->mmap_regions = NULL;
  proc->mm->cr3 = pml4_phys;
  proc->cr3 = pml4_phys; // cached
  proc->mm->mmap_brk = 0x800000;
  proc->mm->mmap_phys_brk = MAP_PHYSICAL_BASE;
  proc->entry = lr.entry;

  // 9c. Create user stack mmap_region
  mmap_region *stack_region = (mmap_region *)kmalloc(sizeof(mmap_region));
  if (stack_region) {
    __memset(stack_region, 0, sizeof(mmap_region));
    stack_region->vaddr = stack_base;
    stack_region->size = (uint64_t)user_stack_pages * PAGE_SIZE;
    stack_region->phys = 0; // not MAP_PHYSICAL — anonymous stack
    stack_region->prot = PROT_READ | PROT_WRITE;
    stack_region->fd = -1; // anonymous
    stack_region->offset = 0;
    stack_region->flags = MAP_ANONYMOUS;
    stack_region->next = NULL;
    proc->mm->mmap_regions = stack_region;
  }

  // 9d. Flush CR3
  __asm__ volatile("movq %0, %%cr3" ::"r"(pml4_phys) : "memory");

  // 10. Release old address space
  {
    uint64_t *old_pml4_virt =
        (uint64_t *)phys_to_virt((__force phys_addr_t)old_cr3);
    for (int pml4_idx = 0; pml4_idx < 256; pml4_idx++) {
      uint64_t pdpt_entry = old_pml4_virt[pml4_idx];
      if (!(pdpt_entry & PTE_PRESENT))
        continue;

      uint64_t pdpt_phys = pdpt_entry & 0x000FFFFFFFFFF000ULL;
      uint64_t *pdpt_virt =
          (uint64_t *)phys_to_virt((__force phys_addr_t)pdpt_phys);

      for (int pdpt_idx = 0; pdpt_idx < 512; pdpt_idx++) {
        uint64_t pd_entry = pdpt_virt[pdpt_idx];
        if (!(pd_entry & PTE_PRESENT))
          continue;
        if (pd_entry & PTE_PS)
          continue;

        uint64_t pd_phys = pd_entry & 0x000FFFFFFFFFF000ULL;
        uint64_t *pd_virt =
            (uint64_t *)phys_to_virt((__force phys_addr_t)pd_phys);

        for (int pd_idx = 0; pd_idx < 512; pd_idx++) {
          uint64_t pt_entry = pd_virt[pd_idx];
          if (!(pt_entry & PTE_PRESENT))
            continue;
          if (pt_entry & PTE_PS)
            continue;

          uint64_t pt_phys = pt_entry & 0x000FFFFFFFFFF000ULL;
          uint64_t *pt_virt =
              (uint64_t *)phys_to_virt((__force phys_addr_t)pt_phys);

          for (int pt_idx = 0; pt_idx < 512; pt_idx++) {
            uint64_t pte = pt_virt[pt_idx];
            if (pte_present(pte)) {
              uint64_t leaf_phys = pte & 0x000FFFFFFFFFF000ULL;
              bool skip = false;
              for (mmap_region *mr = old_regions; mr; mr = mr->next) {
                if (mr->shm_obj) {
                  shm *s = mr->shm_obj;
                  if (s->page_list) {
                    for (int pi = 0; pi < s->num_pages; pi++)
                      if (leaf_phys == s->page_list[pi]) {
                        skip = true;
                        break;
                      }
                  } else if (s->phys && s->npages) {
                    if (leaf_phys >= s->phys &&
                        leaf_phys < s->phys + s->npages * PAGE_SIZE) {
                      skip = true;
                      break;
                    }
                  }
                }
                if (mr->phys && leaf_phys >= mr->phys &&
                    leaf_phys < mr->phys + mr->size) {
                  skip = true;
                  break;
                }
              }
              if (sig_trampoline_phys && leaf_phys == sig_trampoline_phys)
                skip = true;
              if (!skip) {
                struct page *leaf_page = &bfc_frames[PHY_TO_PAGE(leaf_phys)];
                if (refcount_dec_and_test(&leaf_page->p_refcount)) {
                  bfc_free_page(leaf_page, 1);
                }
              }
              pt_virt[pt_idx] = 0;
            }
          }
          free_table_page(pt_phys);
          pd_virt[pd_idx] = 0;
        }
        free_table_page(pd_phys);
        pdpt_virt[pdpt_idx] = 0;
      }
      free_table_page(pdpt_phys);
      old_pml4_virt[pml4_idx] = 0;
    }
    free_table_page(old_cr3);
  }

  // 11. Free old mmap_regions + release SHM references
  {
    mmap_region *region = old_regions;
    while (region) {
      mmap_region *next = region->next;
      if (region->shm_obj)
        shm_put(region->shm_obj);
      kfree(region);
      region = next;
    }
  }

  // 12. kfree ELF buffer
  kfree(elf_buf);

  return 0;
}

// ===================== mmap region allocation =====================

mmap_region *add_mmap_region(xtask *proc, uint64_t vaddr, uint64_t size,
                             uint64_t phys, struct shm *shm_obj, uint32_t prot,
                             int fd, uint64_t offset, uint32_t flags) {
  if (!proc->mm)
    return NULL;
  mmap_region *region = (mmap_region *)kmalloc(sizeof(mmap_region));
  if (!region)
    return NULL;
  region->vaddr = vaddr;
  region->size = size;
  region->phys = phys;
  region->shm_obj = shm_obj;
  region->prot = prot;
  region->fd = fd;
  region->offset = offset;
  region->flags = flags;
  region->next = NULL;
  if (vma_insert_sorted(proc->mm, region) != 0) {
    kfree(region);
    return NULL;
  }
  return region;
}

// ===================== POSIX identity & permissions (group 1)
// ===================== Single-user system: uid/gid default to 0. setuid/setgid
// are relaxed (no setuid-bit privilege rules yet) — they just set
// real+effective, matching the "root can become anyone" model. umask gates
// file-creation mode (applied via i_op->create + the inode cache mode).

int64_t sys_getuid(int64_t unused1, int64_t unused2, int64_t unused3,
                   int64_t unused4, int64_t unused5, int64_t unused6) {
  (void)unused1;
  (void)unused2;
  (void)unused3;
  (void)unused4;
  (void)unused5;
  (void)unused6;
  return (int64_t)current_proc->uid;
}

int64_t sys_geteuid(int64_t unused1, int64_t unused2, int64_t unused3,
                    int64_t unused4, int64_t unused5, int64_t unused6) {
  (void)unused1;
  (void)unused2;
  (void)unused3;
  (void)unused4;
  (void)unused5;
  (void)unused6;
  return (int64_t)current_proc->euid;
}

int64_t sys_getgid(int64_t unused1, int64_t unused2, int64_t unused3,
                   int64_t unused4, int64_t unused5, int64_t unused6) {
  (void)unused1;
  (void)unused2;
  (void)unused3;
  (void)unused4;
  (void)unused5;
  (void)unused6;
  return (int64_t)current_proc->gid;
}

int64_t sys_getegid(int64_t unused1, int64_t unused2, int64_t unused3,
                    int64_t unused4, int64_t unused5, int64_t unused6) {
  (void)unused1;
  (void)unused2;
  (void)unused3;
  (void)unused4;
  (void)unused5;
  (void)unused6;
  return (int64_t)current_proc->egid;
}

int64_t sys_setuid(int64_t arg1, int64_t unused2, int64_t unused3,
                   int64_t unused4, int64_t unused5, int64_t unused6) {
  (void)unused2;
  (void)unused3;
  (void)unused4;
  (void)unused5;
  (void)unused6;
  uint32_t uid = (uint32_t)arg1;
  // Relaxed: a single-user root system has no setuid-bit privilege ladder.
  // euid=uid lets root drop/raise identity freely.
  current_proc->uid = uid;
  current_proc->euid = uid;
  return 0;
}

int64_t sys_setgid(int64_t arg1, int64_t unused2, int64_t unused3,
                   int64_t unused4, int64_t unused5, int64_t unused6) {
  (void)unused2;
  (void)unused3;
  (void)unused4;
  (void)unused5;
  (void)unused6;
  uint32_t gid = (uint32_t)arg1;
  current_proc->gid = gid;
  current_proc->egid = gid;
  return 0;
}

int64_t sys_getppid(int64_t unused1, int64_t unused2, int64_t unused3,
                    int64_t unused4, int64_t unused5, int64_t unused6) {
  (void)unused1;
  (void)unused2;
  (void)unused3;
  (void)unused4;
  (void)unused5;
  (void)unused6;
  pid_t ppid = current_proc->signal->parent_pid;
  // init (and orphans adopted by init) reports itself/1 as parent.
  if (ppid <= 0)
    ppid = current_task->pid;
  return (int64_t)ppid;
}

int64_t sys_getpgrp(int64_t unused1, int64_t unused2, int64_t unused3,
                    int64_t unused4, int64_t unused5, int64_t unused6) {
  (void)unused1;
  (void)unused2;
  (void)unused3;
  (void)unused4;
  (void)unused5;
  (void)unused6;
  return (int64_t)current_proc->pgid;
}

int64_t sys_umask(int64_t arg1, int64_t unused2, int64_t unused3,
                  int64_t unused4, int64_t unused5, int64_t unused6) {
  (void)unused2;
  (void)unused3;
  (void)unused4;
  (void)unused5;
  (void)unused6;
  uint32_t old = current_proc->umask;
  current_proc->umask = (uint32_t)arg1 & 0777;
  return (int64_t)old;
}

// gethostname(buf, len) — copy out the kernel hostname string.
int64_t sys_gethostname(int64_t arg1, int64_t arg2, int64_t unused3,
                        int64_t unused4, int64_t unused5, int64_t unused6) {
  (void)unused3;
  (void)unused4;
  (void)unused5;
  (void)unused6;
  char __user *ubuf = (char __user *__force)arg1;
  size_t len = (size_t)arg2;
  if (!ubuf || len == 0)
    return (int64_t)-EFAULT;
  char kbuf[HOSTNAME_MAX];
  size_t n = hostname_get(kbuf, sizeof(kbuf));
  if (n >= len)
    return (int64_t)-EINVAL;           // buffer too small (would not fit NUL)
  if (copy_to_user(ubuf, kbuf, n + 1)) // include terminator
    return (int64_t)-EFAULT;
  return 0;
}

// sethostname(name, len) — replace the kernel hostname.
int64_t sys_sethostname(int64_t arg1, int64_t arg2, int64_t unused3,
                        int64_t unused4, int64_t unused5, int64_t unused6) {
  (void)unused3;
  (void)unused4;
  (void)unused5;
  (void)unused6;
  const char __user *uname = (const char __user *__force)arg1;
  size_t len = (size_t)arg2;
  if (!uname)
    return (int64_t)-EFAULT;
  if (len == 0 || len >= HOSTNAME_MAX)
    return (int64_t)-EINVAL;
  char kbuf[HOSTNAME_MAX];
  if (copy_from_user(kbuf, uname, len))
    return (int64_t)-EFAULT;
  // Reject embedded NUL so the hostname stays a clean C string.
  for (size_t i = 0; i < len; i++)
    if (kbuf[i] == '\0')
      return (int64_t)-EINVAL;
  hostname_set(kbuf, len);
  return 0;
}
