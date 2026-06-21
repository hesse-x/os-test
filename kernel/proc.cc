#include <stdint.h>
#include <stddef.h>

#include "kernel/proc.h"
#include "kernel/serial.h"
#include "kernel/trap.h"
#include "kernel/trap.h"
#include "kernel/mem/alloc.h"
#include "kernel/mem/slab.h"
#include "kernel/elf_loader.h"
#include "common/shm.h"
#include "common/macro.h"
#include "common/errno.h"
#include "kernel/fb.h"
#include "arch/x64/paging.h"
#include "arch/x64/trap.h"
#include "arch/x64/utils.h"
#include "arch/x64/apic.h"
#include "common/dev.h"
#include "arch/x64/apic.h"
#include "kernel/socket.h"

// Minimal file_io_req for FD_FILE CLOSE notification (must match fs_driver struct layout)
struct file_io_close_req {
    uint32_t cmd;
    uint8_t  _path[256];
    uint32_t _flags;
    int32_t  fs_fd;          // at offset 264, same as fs_driver's file_req.fs_fd
};

task_t tasks[MAX_PROC];
// current_task is per-CPU (in cpu_local_t), accessed via macro

spinlock_t tasks_lock = {0};
pid_t init_pid = -1;

// Free a page table page by physical address
static void free_table_page(uint64_t phys) {
    Page *p = &BFCAllocator::frames[PHY_TO_PAGE(phys)];
    bfc_alloc.free_page(p, 1);
}

// ===================== mm_t helpers =====================

mm_t *mm_create() {
    mm_t *mm = (mm_t *)kcalloc(1, sizeof(mm_t));
    if (!mm) return nullptr;
    mm->ref_count = 1;
    mm->mmap_brk = 0x800000;
    mm->mmap_phys_brk = MAP_PHYSICAL_BASE;
    mm->parent_pid = -1;
    mm->sig.sig_lock = SPINLOCK_INIT;
    for (int si = 0; si < NSIG; si++) {
        mm->sig.action[si].sa_handler = SIG_DFL;
        mm->sig.action[si].sa_mask = 0;
        mm->sig.action[si].sa_flags = 0;
    }
    for (int j = 0; j < MAX_FD; j++) {
        mm->fd_table[j].type = FD_NONE;
    }
    return mm;
}

void mm_release_pages(mm_t *mm) {
    if (!mm) return;

    // Walk user PML4 entries (0-255, canonical low half), free leaf pages + page table pages
    uint64_t *pml4_virt = (uint64_t *)phys_to_virt(mm->cr3);
    for (int pml4_idx = 0; pml4_idx < 256; pml4_idx++) {
        uint64_t pdpt_entry = pml4_virt[pml4_idx];
        if (!(pdpt_entry & PTE_PRESENT)) continue;

        uint64_t pdpt_phys = pdpt_entry & 0x000FFFFFFFFFF000ULL;
        uint64_t *pdpt_virt = (uint64_t *)phys_to_virt(pdpt_phys);

        for (int pdpt_idx = 0; pdpt_idx < 512; pdpt_idx++) {
            uint64_t pd_entry = pdpt_virt[pdpt_idx];
            if (!(pd_entry & PTE_PRESENT)) continue;
            if (pd_entry & PTE_PS) continue;

            uint64_t pd_phys = pd_entry & 0x000FFFFFFFFFF000ULL;
            uint64_t *pd_virt = (uint64_t *)phys_to_virt(pd_phys);

            for (int pd_idx = 0; pd_idx < 512; pd_idx++) {
                uint64_t pt_entry = pd_virt[pd_idx];
                if (!(pt_entry & PTE_PRESENT)) continue;
                if (pt_entry & PTE_PS) continue;

                uint64_t pt_phys = pt_entry & 0x000FFFFFFFFFF000ULL;
                uint64_t *pt_virt = (uint64_t *)phys_to_virt(pt_phys);

                for (int pt_idx = 0; pt_idx < 512; pt_idx++) {
                    uint64_t pte = pt_virt[pt_idx];
                    if (pte & PTE_PRESENT) {
                        uint64_t leaf_phys = pte & 0x000FFFFFFFFFF000ULL;
                        bool is_shared = false;
                        for (mmap_region *mr = mm->mmap_regions; mr; mr = mr->next) {
                            if (mr->shm_obj != nullptr) {
                                struct shm *s = mr->shm_obj;
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
                        if (!is_shared) {
                            Page *leaf_page = &BFCAllocator::frames[PHY_TO_PAGE(leaf_phys)];
                            bfc_alloc.free_page(leaf_page, 1);
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

    // Free PML4 page itself
    free_table_page(mm->cr3);

    // Free mmap region metadata + release SHM references
    mmap_region *region = mm->mmap_regions;
    while (region) {
        mmap_region *next = region->next;
        if (region->shm_obj) {
            shm_put(region->shm_obj);
        }
        kfree(region);
        region = next;
    }
    mm->mmap_regions = nullptr;
}

void mm_release(mm_t *mm) {
    if (!mm) return;

    // 1. Release address space pages + page tables + mmap regions
    mm_release_pages(mm);

    // 2. Handle SHM fd references in fd_table
    for (int fd = 0; fd < MAX_FD; fd++) {
        if (mm->fd_table[fd].type == FD_SHM && mm->fd_table[fd].shm) {
            shm_put(mm->fd_table[fd].shm);
            mm->fd_table[fd].shm = nullptr;
        }
    }

    // 3. Close all open fds (pipe/file/socket cleanup)
    for (int fd = 0; fd < MAX_FD; fd++) {
        if (mm->fd_table[fd].type == FD_PIPE) {
            struct pipe *p = mm->fd_table[fd].pipe;
            if (p) {
                p->ref_count--;
                if (mm->fd_table[fd].flags & (O_WRONLY | O_RDWR)) {
                    if (p->read_pid >= 0) wake_process(p->read_pid);
                }
                if (mm->fd_table[fd].flags & (O_RDONLY | O_RDWR)) {
                    if (p->write_pid >= 0) wake_process(p->write_pid);
                }
                if (p->ref_count == 0) {
                    kfree(p->buf);
                    kfree(p);
                }
            }
        } else if (mm->fd_table[fd].type == FD_FILE) {
            mm->fd_table[fd].file_data.ref_count--;
            if (mm->fd_table[fd].file_data.ref_count == 0 && mm->fd_table[fd].file_data.fs_pid >= 0) {
                file_io_close_req req;
                __memset(&req, 0, sizeof(req));
                req.cmd = 4;  // FILE_CMD_CLOSE
                req.fs_fd = mm->fd_table[fd].file_data.fs_fd;
                kernel_msg_send(mm->fd_table[fd].file_data.fs_pid,
                                &req, sizeof(req), nullptr, 0);
            }
        } else if (mm->fd_table[fd].type == FD_SOCKET) {
            struct unix_sock *sock = mm->fd_table[fd].sock;
            if (sock) {
                sock_close(sock);
            }
        }
        __memset(&mm->fd_table[fd], 0, sizeof(struct file));
        mm->fd_table[fd].type = FD_NONE;
    }

    kfree(mm);
}

void mm_put(mm_t *mm) {
    if (!mm) return;
    int old = __atomic_fetch_sub(&mm->ref_count, 1, __ATOMIC_RELAXED);
    if (old == 1) {
        mm_release(mm);
    }
}

// ===================== Fork helpers =====================

void copy_fd_table(mm_t *dst, mm_t *src) {
    for (int fd = 0; fd < MAX_FD; fd++) {
        if (src->fd_table[fd].type != FD_NONE) {
            dst->fd_table[fd] = src->fd_table[fd];
            if (dst->fd_table[fd].type == FD_PIPE && dst->fd_table[fd].pipe) {
                dst->fd_table[fd].pipe->ref_count++;
            } else if (dst->fd_table[fd].type == FD_FILE) {
                dst->fd_table[fd].file_data.ref_count++;
            } else if (dst->fd_table[fd].type == FD_SHM && dst->fd_table[fd].shm) {
                shm_get(dst->fd_table[fd].shm);
            }
        }
    }
}

mmap_region *copy_mmap_regions(mmap_region *src) {
    mmap_region *head = nullptr;
    mmap_region **tail = &head;
    while (src) {
        mmap_region *dst = (mmap_region *)kmalloc(sizeof(mmap_region));
        if (!dst) break;
        *dst = *src;
        if (dst->shm_obj) shm_get(dst->shm_obj);
        dst->next = nullptr;
        *tail = dst;
        tail = &dst->next;
        src = src->next;
    }
    return head;
}

// ===================== Timer queue operations =====================
// Must be called under scheduler_lock of the target CPU

void timer_queue_insert(int cpu, task_t *task) {
    list_node_t *head = &cpu_locals[cpu].timer_queue;
    list_node_t *node = head->next;
    while (node != head) {
        task_t *t = LIST_ENTRY(node, task_t, wait_node);
        if (t->wait_deadline > task->wait_deadline) break;
        node = node->next;
    }
    task->wait_node.prev = node->prev;
    task->wait_node.next = node;
    node->prev->next = &task->wait_node;
    node->prev = &task->wait_node;
}

void timer_queue_remove(task_t *task) {
    list_remove(&task->wait_node);
}

// ===================== Process table =====================

void proc_init() {
    for (int i = 0; i < MAX_PROC; i++) {
        tasks[i].tid = -1;
        tasks[i].state = READY;
        tasks[i].k_rsp = 0;
        tasks[i].k_stack_top = 0;
        tasks[i].cr3 = 0;
        tasks[i].tgid = -1;
        tasks[i].entry = 0;
        tasks[i].wait_event = WAIT_NONE;
        tasks[i].assigned_cpu = -1;
        tasks[i].mm = nullptr;
        tasks[i].iopm = nullptr;
        tasks[i].exit_code = 0;
        tasks[i].clear_tid_addr = 0;
        tasks[i].wait_deadline = 0;
        tasks[i].wait_timed_out = 0;
        list_init(&tasks[i].run_node);
        list_init(&tasks[i].wait_node);
        // recv queue
        tasks[i].recv_head = 0;
        tasks[i].recv_tail = 0;
        tasks[i].recv_lock = SPINLOCK_INIT;
        // REQ state
        tasks[i].req_caller_pid = -1;
        tasks[i].req_reply_buf = nullptr;
        tasks[i].req_result = 0;
        tasks[i].req_target_pid = -1;
        // MSG state
        tasks[i].msg_reply_buf = nullptr;
        tasks[i].msg_reply_len = 0;
        tasks[i].msg_caller_pid = -1;
        tasks[i].msg_result = 0;
        tasks[i].msg_target_pid = -1;
        tasks[i].cpu_time_ns = 0;
        tasks[i].last_sched = 0;
        // Signal state
        tasks[i].sig_pending = 0;
        tasks[i].sig_blocked = 0;
        tasks[i].sig_have_handler = 0;
        tasks[i].sig_saved_rip = 0;
        tasks[i].sig_saved_rsp = 0;
        tasks[i].sig_saved_rflags = 0;
        // FPU state
        tasks[i].used_fpu = 0;
        tasks[i].fpu_state = nullptr;
        // futex
        list_init(&tasks[i].futex_node);
        tasks[i].futex_uaddr = 0;
        // TLS
        tasks[i].fs_base = 0;
    }
    cpu_locals[0]._cur_task = nullptr;
    cpu_locals[0].current_tf = nullptr;
    cpu_locals[0].run_count = 0;
    cpu_locals[0].idle_task = nullptr;
    for (int c = 0; c < NUM_KMALLOC_CLASSES; c++) {
        cpu_locals[0].active_slab[c] = nullptr;
    }
}

// 在内核栈顶构建 trapframe + switch_frame，返回 k_rsp
static uint64_t build_kstack(uint64_t k_stack_top, uint64_t entry_rip) {
    trapframe_t tf = {};
    tf.ss      = 0x23;                   // USER_DS
    tf.rsp     = 0x00007FFFFFFFE000;      // user stack top (top of mapped page at 0x7FFFFFFFD000)
    tf.rflags  = 0x202;                  // IF=1, IOPL=0
    tf.cs      = 0x2B;                   // USER_CS
    tf.rip     = entry_rip;
    tf.err_code = 0;
    tf.trapno  = 0;

    switch_frame_t sf = {};
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
    switch_frame_t sf = {};
    sf.ret_addr = (uint64_t)idle_entry;

    uint8_t *sp = (uint8_t *)k_stack_top;
    sp -= sizeof(switch_frame_t);
    __memcpy(sp, &sf, sizeof(switch_frame_t));

    return (uint64_t)sp;
}

// Create idle task for the specified CPU
task_t *create_idle_process(int cpu_id) {
    spin_lock(&tasks_lock);
    task_t *task = nullptr;
    int alloc_idx = -1;
    for (int i = 0; i < MAX_PROC; i++) {
        if (tasks[i].tid < 0) {
            task = &tasks[i];
            alloc_idx = i;
            break;
        }
    }
    if (!task) { spin_unlock(&tasks_lock); serial_printf("create_idle_process: no free slot\n"); return nullptr; }

    // Allocate kernel stack (8KB = 2 pages)
    Page *stack_pages = bfc_alloc.alloc_page(2);
    if (!stack_pages) { spin_unlock(&tasks_lock); serial_printf("create_idle_process: alloc stack failed\n"); return nullptr; }
    uint64_t k_stack_phys = page_to_phys(stack_pages);
    uint64_t k_stack_top = phys_to_virt(k_stack_phys) + 2 * PAGE_SIZE;

    // Build idle switch_frame on kernel stack (no trapframe, no user mode)
    uint64_t k_rsp = build_idle_kstack(k_stack_top);

    // Fill PCB: idle uses kernel PML4, no user address space
    task->tid = alloc_idx;
    task->state = RUNNING;  // idle starts as RUNNING on its CPU
    task->k_rsp = k_rsp;
    task->k_stack_top = k_stack_top;
    task->cr3 = PHY_ADDR((uintptr_t)pml4); // kernel PML4 physical address
    task->tgid = alloc_idx;
    task->entry = (uint64_t)idle_entry;
    task->wait_event = WAIT_NONE;
    task->assigned_cpu = cpu_id;
    task->mm = nullptr;  // idle has no user address space
    task->iopm = nullptr;
    task->exit_code = 0;
    task->clear_tid_addr = 0;
    task->cpu_time_ns = 0;
    task->last_sched = 0;
    task->sig_pending = 0;
    task->sig_blocked = 0;
    task->sig_have_handler = 0;
    task->sig_saved_rip = 0;
    task->sig_saved_rsp = 0;
    task->sig_saved_rflags = 0;
    task->used_fpu = 0;
    task->fpu_state = nullptr;
    task->fs_base = 0;
    list_init(&task->run_node);
    list_init(&task->wait_node);
    list_init(&task->futex_node);
    spin_unlock(&tasks_lock);

    cpu_locals[cpu_id].idle_task = task;

    return task;
}

void idle_entry() {
    sti();
    while (1) {
        schedule();
        sti();
        __asm__ volatile("hlt");
    }
}

// Pick the CPU with the fewest runnable processes
int pick_cpu() {
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
    task_t *task = nullptr;
    int alloc_idx = -1;
    for (int i = 0; i < MAX_PROC; i++) {
        if (tasks[i].tid < 0) {
            task = &tasks[i];
            alloc_idx = i;
            break;
        }
    }
    if (!task) { spin_unlock(&tasks_lock); serial_printf("process_create_elf: no free slot\n"); return nullptr; }
    serial_printf("process_create_elf: alloc_idx=%lx\n", (uint64_t)alloc_idx);

    // 2. Allocate kernel stack (8KB = 2 pages)
    Page *stack_pages = bfc_alloc.alloc_page(2);
    if (!stack_pages) { spin_unlock(&tasks_lock); return nullptr; }
    uint64_t k_stack_phys = page_to_phys(stack_pages);
    uint64_t k_stack_top = phys_to_virt(k_stack_phys) + 2 * PAGE_SIZE;

    // 3. Allocate per-process PML4
    Page *pml4_page = bfc_alloc.alloc_page(1);
    if (!pml4_page) { spin_unlock(&tasks_lock); return nullptr; }
    uint64_t pml4_phys = page_to_phys(pml4_page);
    uint64_t pml4_virt = phys_to_virt(pml4_phys);

    // 4. Clear PML4 + copy kernel entries
    uint64_t *new_pml4 = (uint64_t *)pml4_virt;
    for (int i = 0; i < 512; i++) {
        new_pml4[i] = 0;
    }
    new_pml4[511] = pml4[511];

    // 5. Load ELF segments into user address space
    elf_load_result lr = elf_load(elf_data, elf_size, new_pml4);
    if (!lr.success) { spin_unlock(&tasks_lock); serial_puts("process_create_elf: elf_load failed\n"); return nullptr; }
    serial_puts("process_create_elf: entry=");
    serial_put_hex(lr.entry);
    serial_puts(" elf_size=");
    serial_put_hex(elf_size);
    serial_puts("\n");

    // 7. Map user stack: 2048 pages (8MB) at 0x7FFFFFFF0000-0x7FFFFFFFE000
    int user_stack_pages = 2048;
    Page *user_stack_page = bfc_alloc.alloc_page(user_stack_pages);
    if (!user_stack_page) { spin_unlock(&tasks_lock); return nullptr; }
    uint64_t user_stack_phys = page_to_phys(user_stack_page);
    uint64_t stack_base = 0x00007FFFFFFFE000 - (uint64_t)user_stack_pages * PAGE_SIZE;

    for (int i = 0; i < user_stack_pages; i++) {
        if (!map_user_page_direct(new_pml4, stack_base + i * PAGE_SIZE,
                                 user_stack_phys + i * PAGE_SIZE,
                                 PTE_PRESENT | PTE_RW | PTE_USER | PTE_NX)) {
            spin_unlock(&tasks_lock);
            return nullptr;
        }
    }

    // Map shared trampoline page at fixed user address
    if (sig_trampoline_phys != 0) {
        if (!map_user_page_direct(new_pml4, SIG_TRAMPOLINE_ADDR, sig_trampoline_phys,
                                 PTE_PRESENT | PTE_USER)) {
            serial_printf("process_create_elf: failed to map trampoline page\n");
        }
    }

    // 8. Build trapframe + switch_to frame on kernel stack
    uint64_t k_rsp = build_kstack(k_stack_top, lr.entry);

    // 9. Allocate mm_t
    mm_t *mm = mm_create();
    if (!mm) { spin_unlock(&tasks_lock); return nullptr; }
    mm->cr3 = pml4_phys;

    // 10. Fill task (still under tasks_lock)
    int assigned_cpu = pick_cpu();
    task->tid = alloc_idx;
    task->state = READY;
    task->k_rsp = k_rsp;
    task->k_stack_top = k_stack_top;
    task->cr3 = pml4_phys;  // cached from mm->cr3 for switch_to assembly
    task->tgid = alloc_idx;
    task->entry = lr.entry;
    task->wait_event = WAIT_NONE;
    task->assigned_cpu = assigned_cpu;
    task->mm = mm;
    task->iopm = nullptr;
    task->exit_code = 0;
    task->clear_tid_addr = 0;
    task->cpu_time_ns = 0;
    task->last_sched = 0;
    task->sig_pending = 0;
    task->sig_blocked = 0;
    task->sig_have_handler = 0;
    task->sig_saved_rip = 0;
    task->sig_saved_rsp = 0;
    task->sig_saved_rflags = 0;
    task->used_fpu = 0;
    task->fpu_state = nullptr;
    task->fs_base = 0;
    list_init(&task->run_node);
    list_init(&task->wait_node);
    list_init(&task->futex_node);
    spin_unlock(&tasks_lock);

    // Enqueue to target CPU's run_queue under scheduler_lock
    spin_lock(&cpu_locals[assigned_cpu].scheduler_lock);
    list_push_back(&cpu_locals[assigned_cpu].run_queue, &task->run_node);
    cpu_locals[assigned_cpu].run_count++;
    spin_unlock(&cpu_locals[assigned_cpu].scheduler_lock);

    return task;
}

// Update TSS IOPM for the current CPU to match the given task
static void update_tss_iopm(task_t *task) {
    int cpu = get_cpu_local()->cpu_id;
    tss_t *tss = &per_cpu_tss[cpu];
    if (task->iopm) {
        __memcpy(tss->iopm, task->iopm, IOPM_SIZE);
    } else {
        // Deny all ports
        for (int i = 0; i < IOPM_SIZE; i++)
            tss->iopm[i] = 0xFF;
    }
}

// Save prev FPU state + set CR0.TS for lazy restore on next SSE use
static void fpu_context_switch(task_t *prev, task_t *next) {
    if (prev->mm && prev->used_fpu && prev->fpu_state) {
        __asm__ volatile("fxsave (%0)" :: "r"(prev->fpu_state) : "memory");
    }
    uint64_t cr0 = read_cr0();
    cr0 |= (1ULL << 3);  // set TS
    write_cr0(cr0);
}

void schedule() {
    int my_cpu = get_cpu_local()->cpu_id;
    task_t *idle = get_cpu_local()->idle_task;
    task_t *prev = current_task;

    uint64_t flags;
    spin_lock_irqsave(&cpu_locals[my_cpu].scheduler_lock, &flags);

    // Check if run_queue has a runnable task
    if (list_empty(&cpu_locals[my_cpu].run_queue)) {
        // If prev is BLOCKED, ZOMBIE, or REAPING, it cannot continue running —
        // switch to idle so the CPU halts until an IRQ wakes a task.
        if (prev != idle && (prev->state == BLOCKED || prev->state == ZOMBIE || prev->state == REAPING)) {
            // Account prev's CPU time before switching to idle
            if (prev->last_sched != 0) {
                prev->cpu_time_ns += sched_clock() - prev->last_sched;
            }
            current_task = idle;
            per_cpu_tss[my_cpu].rsp0 = idle->k_stack_top;
            get_cpu_local()->tss_rsp0 = idle->k_stack_top;
            update_tss_iopm(idle);
            spin_unlock_irqrestore(&cpu_locals[my_cpu].scheduler_lock, flags);
            if (prev != idle) fpu_context_switch(prev, idle);
            prev->fs_base = rdmsr(MSR_FS_BASE);
            switch_to(prev, idle);
            wrmsr(MSR_FS_BASE, current_task->fs_base);
            spin_lock_irqsave(&cpu_locals[my_cpu].scheduler_lock, &flags);
            spin_unlock_irqrestore(&cpu_locals[my_cpu].scheduler_lock, flags);
            return;
        }
        spin_unlock_irqrestore(&cpu_locals[my_cpu].scheduler_lock, flags);
        return; // no runnable task, prev continues
    }

    // Dequeue next task from head (FIFO round-robin)
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

    next->state = RUNNING;
    next->last_sched = sched_clock();
    cpu_locals[my_cpu].run_count--;
    current_task = next;
    per_cpu_tss[my_cpu].rsp0 = next->k_stack_top;
    get_cpu_local()->tss_rsp0 = next->k_stack_top;
    update_tss_iopm(next);

    // Release lock before switch_to, re-acquire after
    spin_unlock_irqrestore(&cpu_locals[my_cpu].scheduler_lock, flags);
    fpu_context_switch(prev, next);
    prev->fs_base = rdmsr(MSR_FS_BASE);
    switch_to(prev, next);
    wrmsr(MSR_FS_BASE, current_task->fs_base);
    spin_lock_irqsave(&cpu_locals[my_cpu].scheduler_lock, &flags);
    spin_unlock_irqrestore(&cpu_locals[my_cpu].scheduler_lock, flags);
}

// proc_reap: reclaim all resources of a task
// Called by sys_exit (no-parent path) or sys_waitpid
void proc_reap(task_t *task) {
    mm_t *mm = task->mm;

    // 1. Free kernel stack (2 pages)
    uint64_t k_stack_phys_base = PHY_ADDR(task->k_stack_top - 2 * PAGE_SIZE);
    Page *stack_page = &BFCAllocator::frames[PHY_TO_PAGE(k_stack_phys_base)];
    bfc_alloc.free_page(stack_page, 2);

    // 2. Handle clear_tid_addr: write 0 + futex_wake (needs valid address space)
    if (task->clear_tid_addr && mm) {
        pid_t *tid_ptr = (pid_t *)(uintptr_t)task->clear_tid_addr;
        uint64_t saved_cr3;
        __asm__ volatile("movq %%cr3, %0" : "=r"(saved_cr3));
        __asm__ volatile("movq %0, %%cr3" :: "r"(task->cr3) : "memory");
        *tid_ptr = 0;
        __asm__ volatile("movq %0, %%cr3" :: "r"(saved_cr3) : "memory");

        // Wake futex waiters on this address
        uint32_t hash = futex_hash((uint64_t)tid_ptr);
        futex_bucket *bucket = &futex_table[hash];
        spin_lock(&bucket->lock);
        list_node_t *node = bucket->waiters.next;
        while (node != &bucket->waiters) {
            task_t *waiter = LIST_ENTRY(node, task_t, futex_node);
            node = node->next;
            if (waiter->futex_uaddr == (uint64_t)tid_ptr) {
                list_remove(&waiter->futex_node);
                waiter->futex_uaddr = 0;
                int cpu = waiter->assigned_cpu;
                spin_lock(&cpu_locals[cpu].scheduler_lock);
                if (waiter->state == BLOCKED && waiter->wait_event == WAIT_FUTEX) {
                    waiter->state = READY;
                    waiter->wait_event = WAIT_NONE;
                    list_push_back(&cpu_locals[cpu].run_queue, &waiter->run_node);
                    cpu_locals[cpu].run_count++;
                }
                spin_unlock(&cpu_locals[cpu].scheduler_lock);
            }
        }
        spin_unlock(&bucket->lock);
        task->clear_tid_addr = 0;
    }

    // 3. Release address space
    mm_put(mm);

    // 4. Free IOPM bitmap
    if (task->iopm) {
        kfree(task->iopm);
        task->iopm = nullptr;
    }

    // 4. Free FPU state
    if (task->fpu_state) {
        kfree(task->fpu_state);
        task->fpu_state = nullptr;
    }

    // 5. Clear dev_table entries for this PID
    dev_table_cleanup(task->tid);

    // 6. Clear irq_owner entries for this PID
    irq_owner_cleanup(task->tid);

    // 7. Wake any tasks waiting for REQ reply from this task
    for (int i = 0; i < MAX_PROC; i++) {
        if (tasks[i].tid >= 0 &&
            tasks[i].state == BLOCKED &&
            tasks[i].wait_event == WAIT_REQ_REPLY &&
            tasks[i].req_target_pid == task->tid) {
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

    // 8. Wake any tasks waiting for MSG reply from this task
    for (int i = 0; i < MAX_PROC; i++) {
        if (tasks[i].tid >= 0 &&
            tasks[i].state == BLOCKED &&
            tasks[i].wait_event == WAIT_MSG_REPLY &&
            tasks[i].msg_target_pid == task->tid) {
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

    // 9. Clear MSG caller state (server died before responding)
    task->msg_caller_pid = -1;

    // 10. Clear signal state
    task->sig_pending = 0;
    task->sig_blocked = 0;
    task->sig_have_handler = 0;

    // 11. Free any RECV_MSG entries in recv queue (kfree their kmaddr)
    spin_lock(&task->recv_lock);
    uint32_t idx = task->recv_tail;
    while (idx != task->recv_head) {
        recv_msg *m = (recv_msg *)task->recv_buf[idx];
        if (m->type == RECV_MSG && m->msg.kmaddr) {
            kfree(m->msg.kmaddr);
            m->msg.kmaddr = nullptr;
        }
        idx = (idx + 1) % RECV_QUEUE_SIZE;
    }
    spin_unlock(&task->recv_lock);

    // 12. Clear PCB slot
    spin_lock(&tasks_lock);
    task->tid = -1;
    task->state = READY;
    task->k_rsp = 0;
    task->k_stack_top = 0;
    task->cr3 = 0;
    task->tgid = -1;
    task->entry = 0;
    task->wait_event = WAIT_NONE;
    task->assigned_cpu = -1;
    task->mm = nullptr;
    task->iopm = nullptr;
    task->exit_code = 0;
    task->clear_tid_addr = 0;
    task->wait_deadline = 0;
    task->wait_timed_out = 0;
    list_init(&task->run_node);
    list_init(&task->wait_node);
    // recv queue
    task->recv_head = 0;
    task->recv_tail = 0;
    task->recv_lock = SPINLOCK_INIT;
    // REQ state
    task->req_caller_pid = -1;
    task->req_reply_buf = nullptr;
    task->req_result = 0;
    task->req_target_pid = -1;
    // MSG state
    task->msg_reply_buf = nullptr;
    task->msg_reply_len = 0;
    task->msg_caller_pid = -1;
    task->msg_result = 0;
    task->msg_target_pid = -1;
    task->cpu_time_ns = 0;
    task->last_sched = 0;
    // Signal state
    task->sig_pending = 0;
    task->sig_blocked = 0;
    task->sig_have_handler = 0;
    task->sig_saved_rip = 0;
    task->sig_saved_rsp = 0;
    task->sig_saved_rflags = 0;
    // FPU
    task->used_fpu = 0;
    task->fpu_state = nullptr;
    // futex
    list_init(&task->futex_node);
    task->futex_uaddr = 0;
    // TLS
    task->fs_base = 0;
    spin_unlock(&tasks_lock);
}
