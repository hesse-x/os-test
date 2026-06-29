#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "kernel/proc.h"

// Validate assembly offset assumptions in trapentry.S (switch_to uses hardcoded offsets)
_Static_assert(offsetof(task_t, k_rsp) == 8,  "switch_to asm: k_rsp offset mismatch");
_Static_assert(offsetof(task_t, cr3)   == 24, "switch_to asm: cr3 offset mismatch");
_Static_assert(sizeof(trapframe_t)   == 176, "trapframe size must be 176 (22 × uint64_t)");
_Static_assert(offsetof(cpu_local_t, tss_rsp0) == 48,
               "syscall_fast_entry asm: tss_rsp0 offset mismatch (expected 48)");

#include "kernel/pty.h"
#include "kernel/inode.h"
#include "kernel/devtmpfs.h"
#include "kernel/log.h"
#include "kernel/trap.h"
#include "kernel/vfs.h"
#include "arch/x64/smp.h"
#include "kernel/mem/alloc.h"
#include "kernel/mem/slab.h"
#include "kernel/elf_loader.h"
#include "common/shm.h"
#include "common/macro.h"
#include "common/errno.h"
#include "common/stat.h"
#include "common/fcntl.h"
#include "kernel/display.h"
#include "kernel/fat32.h"
#include "arch/x64/paging.h"
#include "arch/x64/trap.h"
#include "arch/x64/utils.h"
#include "arch/x64/apic.h"
#include "common/dev.h"
#include "kernel/socket.h"

// Minimal file_io_req for FD_FILE CLOSE notification (must match fs_driver struct layout)
typedef struct file_io_close_req {
    uint32_t cmd;
    uint8_t  _path[256];
    uint32_t _flags;
    int32_t  fs_fd;          // at offset 264, same as fs_driver's file_req.fs_fd
} file_io_close_req;

task_t tasks[MAX_PROC];
// current_task is per-CPU (in cpu_local_t), accessed via macro

spinlock_t tasks_lock = SPINLOCK_INIT;
pid_t init_pid = -1;

// ===================== files_t lifecycle =====================

files_t *files_create(void) {
    files_t *files = (files_t *)kmalloc(sizeof(files_t));
    if (!files) return NULL;
    __memset(files, 0, sizeof(files_t));
    files->fd_lock = SPINLOCK_INIT;
    refcount_set(&files->f_count, 1);
    for (int j = 0; j < MAX_FD; j++) {
        files->fd_table[j] = NULL;
    }
    return files;
}

// ===================== unified fd lifecycle =====================

void file_put(struct file *f) {
    if (!f) return;
    if (!refcount_dec_and_test(&f->f_count)) return;

    switch (f->type) {
    case FD_NONE:
        break;
    case FD_PIPE: {
        pipe_t *p = f->pipe;
        if (p) {
            wake_pipe_peers(p, f->flags);
            if (refcount_dec_and_test(&p->p_count)) {
                kfree(p->buf);
                kfree(p);
            }
        }
        break;
    }
    case FD_SHM:
        if (f->shm) shm_put(f->shm);
        break;
    case FD_REGULAR:
    case FD_DIR:
        if (f->inode) inode_put(f->inode);
        break;
    case FD_DEV: {
        struct inode *ip = f->inode;
        if (ip) {
            if (ip->i_priv) {
                struct dev_ops *ops = (struct dev_ops *)ip->i_priv;
                if (ops->driver_pid == 0 && ops->close)
                    ops->close(current_task, -1);
            }
            inode_put(ip);
        }
        break;
    }
    case FD_FILE:
        if (refcount_dec_and_test(&f->file_data.f_count) && f->file_data.fs_pid >= 0) {
            file_io_close_req req;
            __memset(&req, 0, sizeof(req));
            req.cmd = 4;  // FILE_CMD_CLOSE
            req.fs_fd = f->file_data.fs_fd;
            kernel_msg_send(f->file_data.fs_pid, &req, sizeof(req), NULL, 0);
        }
        break;
    case FD_SOCKET:
        if (f->sock) sock_close(f->sock);
        break;
    case FD_TTY:
        pty_close_file(f);
        if (f->inode) inode_put(f->inode);
        break;
    }
    kfree(f);
}

int alloc_fd(files_t *files, int min_fd) {
    for (int i = min_fd; i < MAX_FD; i++) {
        if (files->fd_table[i] == NULL)
            return i;
    }
    return -EMFILE;
}

void pty_dup_file(struct file *f) {
    struct pty *pty = f->pty;
    if (!pty) return;
    if (pty_is_master_inode(f->inode))
        pty->master_refs++;
    else
        pty->slave_refs++;
}

void pty_close_file(struct file *f) {
    struct pty *pty = f->pty;
    if (!pty) return;
    int is_master = pty_is_master_inode(f->inode);

    if (is_master) {
        pty->master_refs--;
        if (pty->master_refs == 0) {
            if (pty->t_sid != 0) {
                for (int p = 0; p < MAX_PROC; p++) {
                    if (tasks[p].pid == p && tasks[p].pgid == pty->t_pgid
                        && tasks[p].sid == pty->t_sid) {
                        __atomic_or_fetch(&tasks[p].sig.pending, 1ULL << SIGHUP, __ATOMIC_RELEASE);
                        if (tasks[p].state == BLOCKED) wake_process(p);
                    }
                }
            }
            if (pty->s_read_pid >= 0) wake_process(pty->s_read_pid);
            if (pty->s_write_pid >= 0) wake_process(pty->s_write_pid);
        }
    } else {
        pty->slave_refs--;
        if (pty->slave_refs == 0) {
            // Clear m_to_s buffer so master read won't get stale data
            pty->m_to_s_head = 0;
            pty->m_to_s_tail = 0;
            // Wake master read — it gets EOF
            if (pty->m_read_pid >= 0) wake_process(pty->m_read_pid);
            pty->slave_opened = 0;

            // Remove /dev/ptsN from devtmpfs
            char name[16] = "pts";
            int pos = 3;
            int idx = pty->index;
            if (idx == 0) {
                name[pos++] = '0';
            } else {
                char tmp[8]; int tpos = 0; int n = idx;
                while (n > 0) { tmp[tpos++] = '0' + (n % 10); n /= 10; }
                for (int i = tpos - 1; i >= 0; i--) name[pos++] = tmp[i];
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

void files_put(files_t *files) {
    if (!files) return;
    if (refcount_dec_and_test(&files->f_count)) {
        for (int fd = 0; fd < MAX_FD; fd++) {
            struct file *f = fd_uninstall(files, fd);
            if (f) file_put(f);
        }
        kfree(files);
    }
}

// Free a page table page by physical address
static void free_table_page(uint64_t phys) {
    Page *p = &bfc_frames[PHY_TO_PAGE(phys)];
    bfc_free_page(p, 1);
}

// ===================== mm_t lifecycle =====================

mm_t *mm_create(void) {
    mm_t *mm = (mm_t *)kmalloc(sizeof(mm_t));
    if (!mm) return NULL;
    __memset(mm, 0, sizeof(mm_t));

    // Allocate PML4
    Page *pml4_page = bfc_alloc_page(1);
    if (!pml4_page) { kfree(mm); return NULL; }
    uint64_t pml4_phys = (__force uint64_t)page_to_phys(pml4_page);
    uint64_t pml4_virt = (__force uint64_t)phys_to_virt((__force phys_addr_t)pml4_phys);

    uint64_t *new_pml4 = (uint64_t *)pml4_virt;
    for (int i = 0; i < 512; i++) new_pml4[i] = 0;
    new_pml4[511] = pml4[511]; // kernel mapping
    #ifdef SANITIZER
    new_pml4[503] = pml4[503]; // KASAN shadow
    #endif

    mm->cr3 = pml4_phys;
    refcount_set(&mm->m_count, 1);
    mm->parent_pid = -1;
    mm->mmap_brk = 0x800000;
    mm->mmap_phys_brk = MAP_PHYSICAL_BASE;

    // Create independent files_t
    mm->files = files_create();
    if (!mm->files) {
        free_table_page(mm->cr3);
        kfree(mm);
        return NULL;
    }

    return mm;
}

void mm_release(mm_t *mm, pid_t owner_pid) {
    if (!mm) return;
    uint64_t *pml4_virt = (__force uint64_t *)phys_to_virt((__force phys_addr_t)mm->cr3);

    // 1. Walk user PML4 entries (0-255, canonical low half), free leaf pages + page table pages
    for (int pml4_idx = 0; pml4_idx < 256; pml4_idx++) {
        uint64_t pdpt_entry = pml4_virt[pml4_idx];
        if (!(pdpt_entry & PTE_PRESENT)) continue;

        uint64_t pdpt_phys = pdpt_entry & 0x000FFFFFFFFFF000ULL;
        uint64_t *pdpt_virt = (__force uint64_t *)phys_to_virt((__force phys_addr_t)pdpt_phys);

        for (int pdpt_idx = 0; pdpt_idx < 512; pdpt_idx++) {
            uint64_t pd_entry = pdpt_virt[pdpt_idx];
            if (!(pd_entry & PTE_PRESENT)) continue;
            if (pd_entry & PTE_PS) continue;

            uint64_t pd_phys = pd_entry & 0x000FFFFFFFFFF000ULL;
            uint64_t *pd_virt = (__force uint64_t *)phys_to_virt((__force phys_addr_t)pd_phys);

            for (int pd_idx = 0; pd_idx < 512; pd_idx++) {
                uint64_t pt_entry = pd_virt[pd_idx];
                if (!(pt_entry & PTE_PRESENT)) continue;
                if (pt_entry & PTE_PS) continue;

                uint64_t pt_phys = pt_entry & 0x000FFFFFFFFFF000ULL;
                uint64_t *pt_virt = (__force uint64_t *)phys_to_virt((__force phys_addr_t)pt_phys);

                for (int pt_idx = 0; pt_idx < 512; pt_idx++) {
                    uint64_t pte = pt_virt[pt_idx];
                    if (pte & PTE_PRESENT) {
                        uint64_t leaf_phys = pte & 0x000FFFFFFFFFF000ULL;
                        // Check mmap_regions: skip SHM fd mappings and MAP_PHYSICAL
                        bool is_shared = false;
                        for (mmap_region_t *mr = mm->mmap_regions; mr; mr = mr->next) {
                            if (mr->shm_obj != NULL) {
                                shm_t *s = mr->shm_obj;
                                if (s->page_list) {
                                    for (int pi = 0; pi < s->num_pages; pi++) {
                                        if (leaf_phys == s->page_list[pi]) {
                                            is_shared = true;
                                            break;
                                        }
                                    }
                                    if (is_shared) break;
                                } else if (s->phys != 0 && s->npages > 0) {
                                    if (leaf_phys >= s->phys &&
                                        leaf_phys < s->phys + s->npages * PAGE_SIZE) {
                                        is_shared = true;
                                        break;
                                    }
                                }
                            }
                            if (mr->phys != 0 &&
                                leaf_phys >= mr->phys &&
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
                            Page *leaf_page = &bfc_frames[PHY_TO_PAGE(leaf_phys)];
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
    mmap_region_t *region = mm->mmap_regions;
    while (region) {
        mmap_region_t *next = region->next;
        if (region->shm_obj) {
            shm_put(region->shm_obj);
        }
        kfree(region);
        region = next;
    }

    // 4. Release files (closes all fds)
    files_put(mm->files);
    mm->files = NULL;

    // 5. Clear devtmpfs entries and ISR driver PID for this PID
    if (owner_pid >= 0) {
        devtmpfs_cleanup_pid(owner_pid);
        irq_owner_cleanup(owner_pid);

        // Wake any processes waiting for REQ reply from this process
        for (int i = 0; i < MAX_PROC; i++) {
            if (tasks[i].pid >= 0 &&
                tasks[i].state == BLOCKED &&
                tasks[i].wait_event == WAIT_REQ_REPLY &&
                tasks[i].req_target_pid == owner_pid) {
                int wcpu = tasks[i].assigned_cpu;
                spin_lock(&cpu_locals[wcpu].scheduler_lock);
                if (tasks[i].state == BLOCKED && tasks[i].wait_event == WAIT_REQ_REPLY) {
                    tasks[i].state = READY;
                    tasks[i].wait_event = WAIT_NONE;
                    tasks[i].req_result = ESRCH;
                    list_push_back(&cpu_locals[wcpu].run_queue, &tasks[i].run_node);
                    cpu_locals[wcpu].run_count++;
                }
                spin_unlock(&cpu_locals[wcpu].scheduler_lock);
            }
        }

        // Wake any processes waiting for MSG reply from this process
        for (int i = 0; i < MAX_PROC; i++) {
            if (tasks[i].pid >= 0 &&
                tasks[i].state == BLOCKED &&
                tasks[i].wait_event == WAIT_MSG_REPLY &&
                tasks[i].msg_target_pid == owner_pid) {
                int wcpu = tasks[i].assigned_cpu;
                spin_lock(&cpu_locals[wcpu].scheduler_lock);
                if (tasks[i].state == BLOCKED && tasks[i].wait_event == WAIT_MSG_REPLY) {
                    tasks[i].state = READY;
                    tasks[i].wait_event = WAIT_NONE;
                    tasks[i].msg_result = -ESRCH;
                    list_push_back(&cpu_locals[wcpu].run_queue, &tasks[i].run_node);
                    cpu_locals[wcpu].run_count++;
                }
                spin_unlock(&cpu_locals[wcpu].scheduler_lock);
            }
        }
    }

    kfree(mm);
}

void mm_put(mm_t *mm) {
    if (!mm) return;
    if (refcount_dec_and_test(&mm->m_count)) {
        mm_release(mm, -1);
    }
}

void mm_release_pages(mm_t *mm) {
    if (!mm) return;
    uint64_t *pml4_virt = (__force uint64_t *)phys_to_virt((__force phys_addr_t)mm->cr3);

    for (int pml4_idx = 0; pml4_idx < 256; pml4_idx++) {
        uint64_t pdpt_entry = pml4_virt[pml4_idx];
        if (!(pdpt_entry & PTE_PRESENT)) continue;

        uint64_t pdpt_phys = pdpt_entry & 0x000FFFFFFFFFF000ULL;
        uint64_t *pdpt_virt = (__force uint64_t *)phys_to_virt((__force phys_addr_t)pdpt_phys);

        for (int pdpt_idx = 0; pdpt_idx < 512; pdpt_idx++) {
            uint64_t pd_entry = pdpt_virt[pdpt_idx];
            if (!(pd_entry & PTE_PRESENT)) continue;
            if (pd_entry & PTE_PS) continue;

            uint64_t pd_phys = pd_entry & 0x000FFFFFFFFFF000ULL;
            uint64_t *pd_virt = (__force uint64_t *)phys_to_virt((__force phys_addr_t)pd_phys);

            for (int pd_idx = 0; pd_idx < 512; pd_idx++) {
                uint64_t pt_entry = pd_virt[pd_idx];
                if (!(pt_entry & PTE_PRESENT)) continue;
                if (pt_entry & PTE_PS) continue;

                uint64_t pt_phys = pt_entry & 0x000FFFFFFFFFF000ULL;
                uint64_t *pt_virt = (__force uint64_t *)phys_to_virt((__force phys_addr_t)pt_phys);

                for (int pt_idx = 0; pt_idx < 512; pt_idx++) {
                    uint64_t pte = pt_virt[pt_idx];
                    if (pte & PTE_PRESENT) {
                        uint64_t leaf_phys = pte & 0x000FFFFFFFFFF000ULL;
                        bool skip = false;
                        for (mmap_region_t *mr = mm->mmap_regions; mr; mr = mr->next) {
                            if (mr->shm_obj) {
                                shm_t *s = mr->shm_obj;
                                if (s->page_list) {
                                    for (int pi = 0; pi < s->num_pages; pi++)
                                        if (leaf_phys == s->page_list[pi]) { skip = true; break; }
                                } else if (s->phys && s->npages) {
                                    if (leaf_phys >= s->phys && leaf_phys < s->phys + s->npages * PAGE_SIZE) { skip = true; break; }
                                }
                            }
                            if (mr->phys && leaf_phys >= mr->phys && leaf_phys < mr->phys + mr->size) { skip = true; break; }
                        }
                        if (sig_trampoline_phys && leaf_phys == sig_trampoline_phys) skip = true;
                        if (!skip) {
                            Page *leaf_page = &bfc_frames[PHY_TO_PAGE(leaf_phys)];
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

// Deep-copy fd_table from parent_files to child_files.
// Bumps ref counts for pipe, SHM, file, inode, socket, TTY.
static void __attribute__((unused)) copy_fd_table(files_t *parent_files, files_t *child_files) {
    for (int fd = 0; fd < MAX_FD; fd++) {
        struct file *f = parent_files->fd_table[fd];
        child_files->fd_table[fd] = f;
        if (f) {
            file_get(f);
            if (f->type == FD_TTY) pty_dup_file(f);
        }
    }
}

// Deep-copy mmap_regions linked list from parent to child.
// SHM refs are bumped.
__attribute__((unused))
static mmap_region_t *copy_mmap_regions(mmap_region_t *src) {
    mmap_region_t *head = NULL, *tail = NULL;
    for (mmap_region_t *mr = src; mr; mr = mr->next) {
        mmap_region_t *new_mr = (mmap_region_t *)kmalloc(sizeof(mmap_region_t));
        if (!new_mr) return head; // partial copy, caller handles
        *new_mr = *mr;
        new_mr->next = NULL;
        if (mr->shm_obj) shm_get(mr->shm_obj);
        if (!head) head = new_mr;
        else tail->next = new_mr;
        tail = new_mr;
    }
    return head;
}

// switch_to restore frame: callee-saved registers + return address
typedef struct switch_frame_t {
    uint64_t rbx;
    uint64_t rbp;
    uint64_t r12;
    uint64_t r13;
    uint64_t r14;
    uint64_t r15;
    uint64_t ret_addr;
} switch_frame_t;

_Static_assert(sizeof(switch_frame_t) == 56,  "switch_frame size must be 56 (7 × uint64_t)");

static int pick_cpu(void);

// ===================== sys_fork =====================

int64_t sys_fork(int64_t a1, int64_t a2, int64_t a3,
                   int64_t a4, int64_t a5, int64_t a6) {
    (void)a1;(void)a2;(void)a3;(void)a4;(void)a5;(void)a6;
    task_t *parent = current_task;

    // 1. Allocate new task_t slot
    spin_lock(&tasks_lock);
    task_t *child = NULL;
    int alloc_idx = -1;
    for (int i = 0; i < MAX_PROC; i++) {
        if (tasks[i].pid < 0) { child = &tasks[i]; alloc_idx = i; break; }
    }
    if (!child) { spin_unlock(&tasks_lock); return (int64_t)-ENOMEM; }

    // 2. Allocate new mm_t
    mm_t *child_mm = mm_create();
    if (!child_mm) { spin_unlock(&tasks_lock); return (int64_t)-ENOMEM; }

    // 3. Deep-copy parent user page tables
    uint64_t *src_pml4 = (uint64_t *)phys_to_virt((__force phys_addr_t)parent->mm->cr3);
    uint64_t *dst_pml4 = (uint64_t *)phys_to_virt((__force phys_addr_t)child_mm->cr3);
    int ret = copy_page_table(src_pml4, dst_pml4, parent->mm->mmap_regions);
    if (ret < 0) { mm_put(child_mm); spin_unlock(&tasks_lock); return (int64_t)ret; }

    // Flush parent's TLB — copy_page_table modified parent's RW PTEs to COW,
    // stale TLB entries would still show the old writable mappings
    load_cr3(parent->mm->cr3);

    // 4. Copy fd_table (through files_t)
    copy_fd_table(parent->mm->files, child_mm->files);

    // 5. Copy mmap_regions (including user stack region)
    child_mm->mmap_regions = copy_mmap_regions(parent->mm->mmap_regions);
    child_mm->mmap_brk = parent->mm->mmap_brk;
    child_mm->mmap_phys_brk = parent->mm->mmap_phys_brk;
    child_mm->parent_pid = parent->pid;

    // 6. Allocate new kernel stack, copy parent trapframe (rax=0 for child return)
    Page *stack_pages = bfc_alloc_page(2);
    if (!stack_pages) { mm_put(child_mm); spin_unlock(&tasks_lock); return (int64_t)-ENOMEM; }
    uint64_t k_stack_phys = (__force uint64_t)page_to_phys(stack_pages);
    uint64_t k_stack_top = (__force uint64_t)phys_to_virt((__force phys_addr_t)k_stack_phys) + 2 * PAGE_SIZE;

    // Copy current trapframe (from per-CPU cur_tf set by syscall/irq entry)
    trapframe_t tf;
    trapframe_t *parent_tf = get_cpu_local()->cur_tf;
    __memcpy(&tf, parent_tf, sizeof(trapframe_t));
    tf.rax = 0; // child returns 0

    switch_frame_t sf = {0};
    sf.ret_addr = (uint64_t)process_entry;

    uint8_t *sp = (uint8_t *)k_stack_top;
    sp -= sizeof(trapframe_t);
    __memcpy(sp, &tf, sizeof(trapframe_t));
    sp -= sizeof(switch_frame_t);
    __memcpy(sp, &sf, sizeof(switch_frame_t));
    uint64_t k_rsp = (uint64_t)sp;

    // 7. Fill child task_t
    child->pid = alloc_idx;
    child->state = READY;
    child->k_rsp = k_rsp;
    child->k_stack_top = k_stack_top;
    child->cr3 = child_mm->cr3; // cached
    child->entry = parent->entry;
    child->wait_event = WAIT_NONE;
    child->tgid = child->pid;
    child->mm = child_mm;
    child->assigned_cpu = pick_cpu();
    child->iopm = NULL; // fork does not inherit IOPM
    child->exit_code = 0;
    child->recv_head = 0; child->recv_tail = 0; child->recv_lock = SPINLOCK_INIT;
    child->req_caller_pid = -1; child->req_reply_buf = NULL;
    child->msg_caller_pid = -1; child->msg_reply_buf = NULL;
    child->cpu_time_ns = 0; child->last_sched = 0;
    child->sig.pending = 0; child->sig.blocked = parent->sig.blocked;
    __memcpy(child->sig.action, parent->sig.action, sizeof(parent->sig.action));
    child->sid = parent->sid; child->pgid = parent->pgid; child->ctty = parent->ctty;
    list_init(&child->run_node); list_init(&child->wait_node);

    spin_unlock(&tasks_lock);

    // 8. Enqueue to scheduler
    int cpu = child->assigned_cpu;
    spin_lock(&cpu_locals[cpu].scheduler_lock);
    list_push_back(&cpu_locals[cpu].run_queue, &child->run_node);
    cpu_locals[cpu].run_count++;
    spin_unlock(&cpu_locals[cpu].scheduler_lock);

    // 9. Parent returns child PID
    return (int64_t)child->pid;
}

// ===================== sys_execve =====================

int64_t sys_execve(int64_t a1, int64_t a2, int64_t a3,
                     int64_t a4, int64_t a5, int64_t a6) {
    (void)a2;(void)a3;(void)a4;(void)a5;(void)a6;
    const char *pathname = (const char *)a1;
    task_t *proc = current_task;
    if (!proc->mm) return (int64_t)-EINVAL;
    printk(LOG_INFO, "execve: pid=%d path=%s\n", proc->pid, pathname);

    // 1. Open pathname via VFS
    int64_t open_result = sys_open((int64_t)(uintptr_t)pathname, O_RDONLY, 0, 0, 0, 0);
    int32_t fd = (int32_t)(open_result & 0xFFFFFFFFULL);
    if (fd < 0) {
        printk(LOG_ERROR, "execve: open failed pid=%d path=%s err=%d\n", proc->pid, pathname, fd);
        return (int64_t)fd;
    }

    // 2. Get file size from inode directly (sys_fstat uses copy_to_user which
    //    would just memcpy, but we avoid the round-trip)
    if (!proc->mm->files->fd_table[fd] || proc->mm->files->fd_table[fd]->type != FD_REGULAR) {
        sys_close((int64_t)fd, 0, 0, 0, 0, 0);
        return (int64_t)-EIO;
    }
    struct inode *ip = proc->mm->files->fd_table[fd]->inode;
    if (!ip) { sys_close((int64_t)fd, 0, 0, 0, 0, 0); return (int64_t)-EBADF; }
    uint32_t saved_ino = ip->ino;
    uint64_t file_size = ip->size;

    // 3. kmalloc buffer, read entire ELF into kernel
    uint8_t *elf_buf = (uint8_t *)kmalloc(file_size);
    if (!elf_buf) { sys_close((int64_t)fd, 0, 0, 0, 0, 0); return (int64_t)-ENOMEM; }

    // Use fat32_read directly (sys_read rejects kernel-space buffers)
    int nread = fat32_read(ip, 0, (void *)elf_buf, file_size);
    sys_close((int64_t)fd, 0, 0, 0, 0, 0);
    ip = NULL;  /* ip is now dangling after sys_close — do not dereference */

    if (nread < 0 || (uint64_t)nread < file_size) { kfree(elf_buf); return (int64_t)-EIO; }

    // 4. Validate ELF magic
    if (elf_buf[0] != 0x7F || elf_buf[1] != 'E' || elf_buf[2] != 'L' || elf_buf[3] != 'F') {
        printk(LOG_ERROR, "execve: pid=%d path=%s ino=%lu size=%lu bad magic: %02x %02x %02x %02x\n",
            proc->pid, pathname, (unsigned long)saved_ino, (unsigned long)file_size,
            elf_buf[0], elf_buf[1], elf_buf[2], elf_buf[3]);
        kfree(elf_buf);
        return (int64_t)-ENOEXEC;
    }

    // 5. Allocate new PML4, copy kernel entries (before releasing old space)
    Page *pml4_page = bfc_alloc_page(1);
    if (!pml4_page) { kfree(elf_buf); return (int64_t)-ENOMEM; }
    uint64_t pml4_phys = (__force uint64_t)page_to_phys(pml4_page);
    uint64_t pml4_virt = (__force uint64_t)phys_to_virt((__force phys_addr_t)pml4_phys);
    uint64_t *new_pml4 = (uint64_t *)pml4_virt;
    for (int i = 0; i < 512; i++) new_pml4[i] = 0;
    new_pml4[511] = pml4[511];

    // 6. elf_load into new PML4 (old address space still intact, failure can roll back)
    elf_load_result_t lr = elf_load(elf_buf, file_size, new_pml4);
    if (!lr.success) {
        kfree(elf_buf);
        free_table_page(pml4_phys);
        printk(LOG_ERROR, "execve: elf_load failed pid=%d\n", proc->pid);
        return (int64_t)-ENOEXEC;
    }

    // 7. Allocate new user stack
    int user_stack_pages = 2048;
    Page *user_stack_page = bfc_alloc_page(user_stack_pages);
    if (!user_stack_page) {
        kfree(elf_buf);
        sys_exit((int64_t)-ENOMEM, 0, 0, 0, 0, 0);
        __builtin_unreachable();
    }
    uint64_t user_stack_phys = (__force uint64_t)page_to_phys(user_stack_page);
    uint64_t stack_base = 0x00007FFFFFFFE000 - (uint64_t)user_stack_pages * PAGE_SIZE;
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

    // === Point of no return: old address space will be replaced ===

    // 8. Close FD_CLOEXEC fds (before releasing old space)
    spinlock_t *fdlk = &proc->mm->files->fd_lock;
    spin_lock(fdlk);
    for (int i = 0; i < MAX_FD; i++) {
        struct file *f = proc->mm->files->fd_table[i];
        if (f && (f->flags & FD_CLOEXEC)) {
            fd_uninstall(proc->mm->files, i);
            file_put(f);
        }
    }
    spin_unlock(fdlk);

    // 9. Switch to new address space FIRST, then free old pages.
    //    If we free before switching, subsequent kmalloc/kfree can reuse
    //    the old PML4 page and corrupt page tables still loaded in CR3.

    // 9a. Modify current trapframe: rip=entry, rsp=stack_top
    trapframe_t *tf = get_cpu_local()->cur_tf;
    tf->rip = lr.entry;
    tf->rsp = 0x00007FFFFFFFE000;
    tf->rax = 0;

    // 9b. Update mm_t fields
    uint64_t old_cr3 = proc->mm->cr3;
    mmap_region_t *old_regions = proc->mm->mmap_regions;
    proc->mm->mmap_regions = NULL;
    proc->mm->cr3 = pml4_phys;
    proc->cr3 = pml4_phys; // cached
    proc->mm->mmap_brk = 0x800000;
    proc->mm->mmap_phys_brk = MAP_PHYSICAL_BASE;
    proc->entry = lr.entry;

    // 9c. Create user stack mmap_region (must be before CR3 flush — kmalloc
    //     may trigger page table walks on old CR3 that are still valid)
    mmap_region_t *stack_region = (mmap_region_t *)kmalloc(sizeof(mmap_region_t));
    if (stack_region) {
        __memset(stack_region, 0, sizeof(mmap_region_t));
        stack_region->vaddr = stack_base;
        stack_region->size = (uint64_t)user_stack_pages * PAGE_SIZE;
        stack_region->phys = 0; // not MAP_PHYSICAL — anonymous stack
        stack_region->prot = PROT_READ | PROT_WRITE;
        stack_region->next = NULL;
        proc->mm->mmap_regions = stack_region;
    }

    // 9d. Flush CR3 — now we are on the new address space
    __asm__ volatile("movq %0, %%cr3" :: "r"(pml4_phys) : "memory");

    // 10. Release old address space (safe now: CR3 points to new PML4)
    //     We inline mm_release_pages using old_cr3/old_regions since
    //     proc->mm->cr3 already points to the new PML4.
    {
        uint64_t *old_pml4_virt = (uint64_t *)phys_to_virt((__force phys_addr_t)old_cr3);
        for (int pml4_idx = 0; pml4_idx < 256; pml4_idx++) {
            uint64_t pdpt_entry = old_pml4_virt[pml4_idx];
            if (!(pdpt_entry & PTE_PRESENT)) continue;

            uint64_t pdpt_phys = pdpt_entry & 0x000FFFFFFFFFF000ULL;
            uint64_t *pdpt_virt = (uint64_t *)phys_to_virt((__force phys_addr_t)pdpt_phys);

            for (int pdpt_idx = 0; pdpt_idx < 512; pdpt_idx++) {
                uint64_t pd_entry = pdpt_virt[pdpt_idx];
                if (!(pd_entry & PTE_PRESENT)) continue;
                if (pd_entry & PTE_PS) continue;

                uint64_t pd_phys = pd_entry & 0x000FFFFFFFFFF000ULL;
                uint64_t *pd_virt = (uint64_t *)phys_to_virt((__force phys_addr_t)pd_phys);

                for (int pd_idx = 0; pd_idx < 512; pd_idx++) {
                    uint64_t pt_entry = pd_virt[pd_idx];
                    if (!(pt_entry & PTE_PRESENT)) continue;
                    if (pt_entry & PTE_PS) continue;

                    uint64_t pt_phys = pt_entry & 0x000FFFFFFFFFF000ULL;
                    uint64_t *pt_virt = (uint64_t *)phys_to_virt((__force phys_addr_t)pt_phys);

                    for (int pt_idx = 0; pt_idx < 512; pt_idx++) {
                        uint64_t pte = pt_virt[pt_idx];
                        if (pte & PTE_PRESENT) {
                            uint64_t leaf_phys = pte & 0x000FFFFFFFFFF000ULL;
                            bool skip = false;
                            for (mmap_region_t *mr = old_regions; mr; mr = mr->next) {
                                if (mr->shm_obj) {
                                    shm_t *s = mr->shm_obj;
                                    if (s->page_list) {
                                        for (int pi = 0; pi < s->num_pages; pi++)
                                            if (leaf_phys == s->page_list[pi]) { skip = true; break; }
                                    } else if (s->phys && s->npages) {
                                        if (leaf_phys >= s->phys && leaf_phys < s->phys + s->npages * PAGE_SIZE) { skip = true; break; }
                                    }
                                }
                                if (mr->phys && leaf_phys >= mr->phys && leaf_phys < mr->phys + mr->size) { skip = true; break; }
                            }
                            if (sig_trampoline_phys && leaf_phys == sig_trampoline_phys) skip = true;
                            if (!skip) {
                                Page *leaf_page = &bfc_frames[PHY_TO_PAGE(leaf_phys)];
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
        mmap_region_t *region = old_regions;
        while (region) {
            mmap_region_t *next = region->next;
            if (region->shm_obj) shm_put(region->shm_obj);
            kfree(region);
            region = next;
        }
    }

    // 12. kfree ELF buffer
    kfree(elf_buf);

    return 0;
}

// ===================== Timer queue operations =====================
// Must be called under scheduler_lock of the target CPU

void timer_queue_insert(int cpu, task_t *proc) {
    list_node_t *head = &cpu_locals[cpu].timer_queue;
    list_node_t *node = head->next;
    while (node != head) {
        task_t *p = LIST_ENTRY(node, task_t, wait_node);
        if (p->wait_deadline > proc->wait_deadline) break;
        node = node->next;
    }
    // Insert before node
    proc->wait_node.prev = node->prev;
    proc->wait_node.next = node;
    node->prev->next = &proc->wait_node;
    node->prev = &proc->wait_node;
}

void timer_queue_remove(task_t *proc) {
    list_remove(&proc->wait_node);
}

// ===================== mmap region allocation =====================

mmap_region_t *add_mmap_region(task_t *proc, uint64_t vaddr, uint64_t size,
                                uint64_t phys, struct shm *shm_obj, uint32_t prot) {
    if (!proc->mm) return NULL;
    mmap_region_t *region = (mmap_region_t *)kmalloc(sizeof(mmap_region_t));
    if (!region) return NULL;
    region->vaddr = vaddr;
    region->size = size;
    region->phys = phys;
    region->shm_obj = shm_obj;
    region->prot = prot;
    region->next = proc->mm->mmap_regions;
    proc->mm->mmap_regions = region;
    return region;
}

// ===================== Process table =====================

void proc_init() {
    for (int i = 0; i < MAX_PROC; i++) {
        tasks[i].pid = -1;
        tasks[i].state = UNUSED;
        tasks[i].k_rsp = 0;
        tasks[i].k_stack_top = 0;
        tasks[i].cr3 = 0;
        tasks[i].entry = 0;
        tasks[i].wait_event = WAIT_NONE;
        tasks[i].tgid = -1;
        tasks[i].mm = NULL;
        tasks[i].assigned_cpu = -1;
        tasks[i].iopm = NULL;
        tasks[i].exit_code = 0;
        tasks[i].wait_deadline = 0;
        tasks[i].wait_timed_out = 0;
        tasks[i].recv_intr = 0;
        list_init(&tasks[i].run_node);
        list_init(&tasks[i].wait_node);
        // recv queue
        tasks[i].recv_head = 0;
        tasks[i].recv_tail = 0;
        tasks[i].recv_lock = SPINLOCK_INIT;
        // REQ state
        tasks[i].req_caller_pid = -1;
        tasks[i].req_reply_buf = NULL;
        tasks[i].req_result = 0;
        tasks[i].req_target_pid = -1;
        // MSG state
        tasks[i].msg_reply_buf = NULL;
        tasks[i].msg_reply_len = 0;
        tasks[i].msg_caller_pid = -1;
        tasks[i].msg_result = 0;
        tasks[i].msg_target_pid = -1;
        tasks[i].cpu_time_ns = 0;
        tasks[i].last_sched = 0;
        // Signal state
        tasks[i].sig.pending = 0;
        tasks[i].sig.blocked = 0;
        __memset(&tasks[i].sig_force_info, 0, sizeof(siginfo_t));
        __memset(tasks[i].sig.action, 0, sizeof(tasks[i].sig.action));
        // Session / controlling terminal
        tasks[i].sid = 0;
        tasks[i].pgid = 0;
        tasks[i].ctty = NULL;
    }
    cpu_locals[0]._cur_proc = NULL;
    cpu_locals[0].run_count = 0;
    cpu_locals[0].idle_proc = NULL;
    for (int c = 0; c < NUM_KMALLOC_CLASSES; c++) {
        cpu_locals[0].active_slab[c] = NULL;
    }
}

// Build trapframe + switch_frame on kernel stack, return k_rsp
static uint64_t build_kstack(uint64_t k_stack_top, uint64_t entry_rip) {
    trapframe_t tf = {0};
    tf.ss      = 0x23;                   // USER_DS
    tf.rsp     = 0x00007FFFFFFFE000;      // user stack top
    tf.rflags  = 0x202;                  // IF=1, IOPL=0
    tf.cs      = 0x2B;                   // USER_CS
    tf.rip     = entry_rip;
    tf.err_code = 0;
    tf.trapno  = 0;

    switch_frame_t sf = {0};
    sf.ret_addr = (uint64_t)process_entry;

    uint8_t *sp = (uint8_t *)k_stack_top;
    sp -= sizeof(trapframe_t);
    __memcpy(sp, &tf, sizeof(trapframe_t));

    sp -= sizeof(switch_frame_t);
    __memcpy(sp, &sf, sizeof(switch_frame_t));

    return (uint64_t)sp;
}

// Build idle kernel stack: only switch_frame (no trapframe), ret_addr = idle_entry
static uint64_t build_idle_kstack(uint64_t k_stack_top) {
    switch_frame_t sf = {0};
    sf.ret_addr = (uint64_t)idle_entry;

    uint8_t *sp = (uint8_t *)k_stack_top;
    sp -= sizeof(switch_frame_t);
    __memcpy(sp, &sf, sizeof(switch_frame_t));

    return (uint64_t)sp;
}

// Create idle process for the specified CPU
task_t *create_idle_process(int cpu_id) {
    spin_lock(&tasks_lock);
    task_t *proc = NULL;
    int alloc_idx = -1;
    for (int i = 0; i < MAX_PROC; i++) {
        if (tasks[i].pid < 0) {
            proc = &tasks[i];
            alloc_idx = i;
            break;
        }
    }
    if (!proc) { spin_unlock(&tasks_lock); printk(LOG_ERROR, "create_idle_process: no free slot\n"); return NULL; }

    // Allocate kernel stack (8KB = 2 pages)
    Page *stack_pages = bfc_alloc_page(2);
    if (!stack_pages) { spin_unlock(&tasks_lock); printk(LOG_ERROR, "create_idle_process: alloc stack failed\n"); return NULL; }
    uint64_t k_stack_phys = (__force uint64_t)page_to_phys(stack_pages);
    uint64_t k_stack_top = (__force uint64_t)phys_to_virt((__force phys_addr_t)k_stack_phys) + 2 * PAGE_SIZE;

    // Build idle switch_frame on kernel stack (no trapframe, no user mode)
    uint64_t k_rsp = build_idle_kstack(k_stack_top);

    // Fill PCB: idle uses kernel PML4, no user address space
    proc->pid = alloc_idx;
    proc->state = RUNNING;  // idle starts as RUNNING on its CPU
    proc->k_rsp = k_rsp;
    proc->k_stack_top = k_stack_top;
    proc->cr3 = (__force uint64_t)PHY_ADDR((uintptr_t)pml4); // kernel PML4 physical address (cached)
    proc->entry = (uint64_t)idle_entry;
    proc->wait_event = WAIT_NONE;
    proc->tgid = proc->pid;
    proc->mm = NULL;  // idle has no address space
    proc->assigned_cpu = cpu_id;
    proc->iopm = NULL;
    proc->exit_code = 0;
    proc->cpu_time_ns = 0;
    proc->last_sched = 0;
    // Signal state
    proc->sig.pending = 0;
    proc->sig.blocked = 0;
    __memset(&proc->sig_force_info, 0, sizeof(siginfo_t));
    __memset(proc->sig.action, 0, sizeof(proc->sig.action));
    // Session / controlling terminal
    proc->sid = 0;
    proc->pgid = 0;
    proc->ctty = NULL;
    list_init(&proc->run_node);
    list_init(&proc->wait_node);
    spin_unlock(&tasks_lock);

    cpu_locals[cpu_id].idle_proc = proc;

    return proc;
}

__attribute__((no_sanitize("kernel-address")))
void idle_entry() {
    sti();
    while (1) {
        schedule();
        sti();
        __asm__ volatile("hlt");
    }
}

// Pick the CPU with the fewest runnable processes
static int pick_cpu() {
    int best = 0;
    int min = __atomic_load_n(&cpu_locals[0].run_count, __ATOMIC_RELAXED);
    for (int i = 1; i < ncpu; i++) {
        int r = __atomic_load_n(&cpu_locals[i].run_count, __ATOMIC_RELAXED);
        if (r < min) {
            min = r;
            best = i;
        }
    }
    return best;
}

task_t *process_create_elf(const uint8_t *elf_data, uint64_t elf_size) {
    // 1. Find free slot under tasks_lock
    spin_lock(&tasks_lock);
    task_t *proc = NULL;
    int alloc_idx = -1;
    for (int i = 0; i < MAX_PROC; i++) {
        if (tasks[i].pid < 0) {
            proc = &tasks[i];
            alloc_idx = i;
            break;
        }
    }
    if (!proc) { spin_unlock(&tasks_lock); printk(LOG_ERROR, "process_create_elf: no free slot\n"); return NULL; }
    // 2. Allocate kernel stack (8KB = 2 pages)
    Page *stack_pages = bfc_alloc_page(2);
    if (!stack_pages) { spin_unlock(&tasks_lock); return NULL; }
    uint64_t k_stack_phys = (__force uint64_t)page_to_phys(stack_pages);
    uint64_t k_stack_top = (__force uint64_t)phys_to_virt((__force phys_addr_t)k_stack_phys) + 2 * PAGE_SIZE;

    // 3. Create mm_t (allocates PML4 + files_t)
    mm_t *mm = mm_create();
    if (!mm) { spin_unlock(&tasks_lock); return NULL; }
    uint64_t pml4_phys = mm->cr3;
    uint64_t pml4_virt = (__force uint64_t)phys_to_virt((__force phys_addr_t)pml4_phys);
    uint64_t *new_pml4 = (uint64_t *)pml4_virt;

    // 4. Load ELF segments into user address space
    elf_load_result_t lr = elf_load(elf_data, elf_size, new_pml4);
    if (!lr.success) { mm_put(mm); spin_unlock(&tasks_lock); printk(LOG_ERROR, "process_create_elf: elf_load failed\n"); return NULL; }

    // 5. Map user stack: 2048 pages (8MB) at 0x7FFFFFFF0000-0x7FFFFFFFE000
    int user_stack_pages = 2048;
    Page *user_stack_page = bfc_alloc_page(user_stack_pages);
    if (!user_stack_page) { mm_put(mm); spin_unlock(&tasks_lock); return NULL; }
    uint64_t user_stack_phys = (__force uint64_t)page_to_phys(user_stack_page);
    uint64_t stack_base = 0x00007FFFFFFFE000 - (uint64_t)user_stack_pages * PAGE_SIZE;

    for (int i = 0; i < user_stack_pages; i++) {
        if (!map_user_page_direct(new_pml4, stack_base + i * PAGE_SIZE,
                                 user_stack_phys + i * PAGE_SIZE,
                                 PTE_PRESENT | PTE_RW | PTE_USER | PTE_NX)) {
            mm_put(mm);
            spin_unlock(&tasks_lock);
            return NULL;
        }
    }

    // Map shared trampoline page at fixed user address
    if (sig_trampoline_phys != 0) {
        if (!map_user_page_direct(new_pml4, SIG_TRAMPOLINE_ADDR, sig_trampoline_phys,
                                 PTE_PRESENT | PTE_USER)) {
            printk(LOG_ERROR, "process_create_elf: failed to map trampoline page\n");
        }
    }

    // 6. Build trapframe + switch_to frame on kernel stack
    uint64_t k_rsp = build_kstack(k_stack_top, lr.entry);

    // 7. Fill PCB (still under tasks_lock)
    int assigned_cpu = pick_cpu();
    proc->pid = alloc_idx;
    proc->state = READY;
    proc->k_rsp = k_rsp;
    proc->k_stack_top = k_stack_top;
    proc->cr3 = pml4_phys;  // cached
    proc->entry = lr.entry;
    proc->wait_event = WAIT_NONE;
    proc->tgid = proc->pid;
    proc->mm = mm;
    proc->assigned_cpu = assigned_cpu;
    proc->iopm = NULL;
    proc->exit_code = 0;
    proc->cpu_time_ns = 0;
    proc->last_sched = 0;
    // Signal state
    proc->sig.pending = 0;
    proc->sig.blocked = 0;
    __memset(&proc->sig_force_info, 0, sizeof(siginfo_t));
    __memset(proc->sig.action, 0, sizeof(proc->sig.action));
    // Session / controlling terminal
    proc->sid = 0;
    proc->pgid = 0;
    proc->ctty = NULL;
    list_init(&proc->run_node);
    list_init(&proc->wait_node);

    // Create stack mmap_region
    mmap_region_t *stack_region = (mmap_region_t *)kmalloc(sizeof(mmap_region_t));
    if (stack_region) {
        __memset(stack_region, 0, sizeof(mmap_region_t));
        stack_region->vaddr = stack_base;
        stack_region->size = (uint64_t)user_stack_pages * PAGE_SIZE;
        stack_region->phys = 0; // not MAP_PHYSICAL — anonymous stack
        stack_region->prot = PROT_READ | PROT_WRITE;
        stack_region->next = NULL;
        mm->mmap_regions = stack_region;
    }

    printk(LOG_DEBUG, "process_create_elf: pid=%d kstack_phys=0x%lx kstack_top=0x%lx\n",
        proc->pid, k_stack_phys, k_stack_top);

    spin_unlock(&tasks_lock);

    // Enqueue to target CPU's run_queue under scheduler_lock
    spin_lock(&cpu_locals[assigned_cpu].scheduler_lock);
    list_push_back(&cpu_locals[assigned_cpu].run_queue, &proc->run_node);
    cpu_locals[assigned_cpu].run_count++;
    spin_unlock(&cpu_locals[assigned_cpu].scheduler_lock);

    return proc;
}

// Update TSS IOPM for the current CPU to match the given process
static void update_tss_iopm(task_t *proc) {
    int cpu = get_cpu_local()->cpu_id;
    tss_t *tss = &per_cpu_tss[cpu];
    if (proc->iopm) {
        __memcpy(tss->iopm, proc->iopm, IOPM_SIZE);
    } else {
        // Deny all ports
        for (int i = 0; i < IOPM_SIZE; i++)
            tss->iopm[i] = 0xFF;
    }
}

__attribute__((no_sanitize("kernel-address")))
void schedule() {
    int my_cpu = get_cpu_local()->cpu_id;
    task_t *idle = get_cpu_local()->idle_proc;
    task_t *prev = current_task;

    uint64_t flags;
    spin_lock_irqsave(&cpu_locals[my_cpu].scheduler_lock, &flags);

    // Check if run_queue has a runnable process
    if (list_empty(&cpu_locals[my_cpu].run_queue)) {
        // If prev is BLOCKED, ZOMBIE, or REAPING, it cannot continue running —
        // switch to idle so the CPU halts until an IRQ wakes a process.
        if (prev != idle && (prev->state == BLOCKED || prev->state == ZOMBIE || prev->state == REAPING)) {
            // Account prev's CPU time before switching to idle
            if (prev->last_sched != 0) {
                prev->cpu_time_ns += sched_clock() - prev->last_sched;
            }
            current_task = idle;
            per_cpu_tss[my_cpu].rsp0 = idle->k_stack_top;
            get_cpu_local()->tss_rsp0 = idle->k_stack_top;
            update_tss_iopm(idle);
            spin_unlock(&cpu_locals[my_cpu].scheduler_lock);
            switch_to(prev, idle);
            spin_lock_irqsave(&cpu_locals[my_cpu].scheduler_lock, &flags);
            spin_unlock_irqrestore(&cpu_locals[my_cpu].scheduler_lock, flags);
            return;
        }
        spin_unlock_irqrestore(&cpu_locals[my_cpu].scheduler_lock, flags);
        return; // no runnable process, prev continues
    }

    // Dequeue next process from head (FIFO round-robin)
    list_node_t *next_node = list_front(&cpu_locals[my_cpu].run_queue);
    task_t *next = LIST_ENTRY(next_node, task_t, run_node);
    list_remove(&next->run_node);

    // Account prev's CPU time before switching out
    if (prev != idle && prev->last_sched != 0) {
        prev->cpu_time_ns += sched_clock() - prev->last_sched;
    }

    // State transition for prev
    if (prev != idle && prev->state == RUNNING) {
        prev->state = READY;
        list_push_back(&cpu_locals[my_cpu].run_queue, &prev->run_node);
        cpu_locals[my_cpu].run_count++;
    }
    // if prev->state == BLOCKED, ZOMBIE, or REAPING: don't enqueue, run_count unchanged

    next->state = RUNNING;
    next->last_sched = sched_clock();
    cpu_locals[my_cpu].run_count--;
    current_task = next;
    per_cpu_tss[my_cpu].rsp0 = next->k_stack_top;
    get_cpu_local()->tss_rsp0 = next->k_stack_top;
    update_tss_iopm(next);

    // Release lock but keep interrupts disabled — switch_to must run under cli
    // to prevent interrupt handlers from corrupting the stack during RSP/CR3 switch.
    // After switch_to returns (prev is resumed on prev's stack), re-acquire and
    // restore interrupts via flags saved on prev's stack by spin_lock_irqsave.
    spin_unlock(&cpu_locals[my_cpu].scheduler_lock);
    switch_to(prev, next);
    spin_lock_irqsave(&cpu_locals[my_cpu].scheduler_lock, &flags);
    spin_unlock_irqrestore(&cpu_locals[my_cpu].scheduler_lock, flags);
}

// task_reap: reclaim all resources of a process
// Called by sys_exit (no-parent path) or sys_waitpid
void task_reap(task_t *proc) {
    ASSERT(proc->state == ZOMBIE || proc->state == REAPING);
    pid_t owner_pid = proc->pid;

    // 1. Free kernel stack (2 pages)
    uint64_t k_stack_phys_base = (__force uint64_t)PHY_ADDR(proc->k_stack_top - 2 * PAGE_SIZE);
    Page *stack_page = &bfc_frames[PHY_TO_PAGE(k_stack_phys_base)];
    bfc_free_page(stack_page, 2);

    // 2. Free IOPM bitmap
    if (proc->iopm) {
        kfree(proc->iopm);
        proc->iopm = NULL;
    }

    // 3. mm_put (decrement triggers mm_release when ref_count hits 0)
    //    mm_release will: free user pages+PML4+mmap+SHM+files+devtmpfs+irq_owner+wake waiters
    if (proc->mm) {
        pid_t pid_for_cleanup = owner_pid;
        mm_t *mm = proc->mm;
        proc->mm = NULL;
        if (refcount_dec_and_test(&mm->m_count)) {
            mm_release(mm, pid_for_cleanup);
        }
    } else {
        // idle process: still need cleanup for devtmpfs/irq_owner
        devtmpfs_cleanup_pid(owner_pid);
        irq_owner_cleanup(owner_pid);
    }

    // 4. Free any RECV_MSG entries in recv queue (kfree their kmaddr)
    spin_lock(&proc->recv_lock);
    uint32_t idx = proc->recv_tail;
    while (idx != proc->recv_head) {
        recv_msg_t *m = (recv_msg_t *)proc->recv_buf[idx];
        if (m->type == RECV_MSG && m->msg.kmaddr) {
            kfree(m->msg.kmaddr);
            m->msg.kmaddr = NULL;
        }
        idx = (idx + 1) % RECV_QUEUE_SIZE;
    }
    spin_unlock(&proc->recv_lock);

    // 5. Clear MSG caller state (server died before responding)
    proc->msg_caller_pid = -1;

    // 6. Clear signal state (pending signals die with the process)
    proc->sig.pending = 0;
    proc->sig.blocked = 0;

    // 7. Clear PCB slot
    spin_lock(&tasks_lock);
    proc->pid = -1;
    proc->state = UNUSED;
    proc->k_rsp = 0;
    proc->k_stack_top = 0;
    proc->cr3 = 0;
    proc->entry = 0;
    proc->wait_event = WAIT_NONE;
    proc->tgid = -1;
    proc->mm = NULL;
    proc->assigned_cpu = -1;
    proc->iopm = NULL;
    proc->exit_code = 0;
    proc->wait_deadline = 0;
    proc->wait_timed_out = 0;
    list_init(&proc->run_node);
    list_init(&proc->wait_node);
    // recv queue
    proc->recv_head = 0;
    proc->recv_tail = 0;
    proc->recv_lock = SPINLOCK_INIT;
    // REQ state
    proc->req_caller_pid = -1;
    proc->req_reply_buf = NULL;
    proc->req_result = 0;
    proc->req_target_pid = -1;
    // MSG state
    proc->msg_reply_buf = NULL;
    proc->msg_reply_len = 0;
    proc->msg_caller_pid = -1;
    proc->msg_result = 0;
    proc->msg_target_pid = -1;
    proc->cpu_time_ns = 0;
    proc->last_sched = 0;
    // Signal state
    proc->sig.pending = 0;
    proc->sig.blocked = 0;
    __memset(&proc->sig_force_info, 0, sizeof(siginfo_t));
    // Session / controlling terminal
    proc->sid = 0;
    proc->pgid = 0;
    proc->ctty = NULL;
    spin_unlock(&tasks_lock);
}
