#include <stdint.h>
#include <stddef.h>

#include "kernel/proc.h"
#include "kernel/serial.h"
#include "kernel/trap.h"
#include "kernel/mem/alloc.h"
#include "common/elf.h"
#include "common/shm.h"
#include "arch/x64/paging.h"
#include "arch/x64/trap.h"
#include "arch/x64/utils.h"
#include "arch/x64/apic.h"

proc_t procs[MAX_PROC];
// current_proc is per-CPU (in cpu_local_t), accessed via macro

spinlock_t procs_lock = {0};

// 64-bit init_code: getpid → add '0' → putc → yield → loop
// Using SYSCALL with: rax=syscall#, rdi=arg1
static const uint8_t init_code[] = {
    0x48, 0xC7, 0xC0, 0x01, 0x00, 0x00, 0x00,  // movq $1, %rax (sys_getpid)
    0x0F, 0x05,                                   // syscall
    0x48, 0x83, 0xC0, 0x30,                      // addq $0x30, %rax
    0x48, 0x89, 0xC7,                             // movq %rax, %rdi
    0x48, 0xC7, 0xC0, 0x00, 0x00, 0x00, 0x00,  // movq $0, %rax (sys_putc)
    0x0F, 0x05,                                   // syscall
    0x48, 0xC7, 0xC0, 0x02, 0x00, 0x00, 0x00,  // movq $2, %rax (sys_yield)
    0x0F, 0x05,                                   // syscall
    0xEB, 0xDC                                    // jmp back (offset = -36)
};

// Convert Page descriptor pointer to physical address of the actual memory page
static uint64_t page_to_phys(Page *p) {
    return (uint64_t)(p - BFCAllocator::frames) * PAGE_SIZE;
}

// Convert physical address to higher-half virtual address
static uint64_t phys_to_virt(uint64_t phys) {
    return phys + VMA_BASE;
}

// ===================== Shared pages =====================
// Allocated in shm_init(), mapped into all user processes
static uint64_t kbd_shm_phys = 0;
static uint64_t disk_req_shm_phys = 0;
static uint64_t disk_resp_shm_phys = 0;

void proc_init() {
    for (int i = 0; i < MAX_PROC; i++) {
        procs[i].pid = -1;
        procs[i].state = READY;
        procs[i].k_rsp = 0;
        procs[i].k_stack_top = 0;
        procs[i].cr3 = 0;
        procs[i].entry = 0;
        procs[i].wait_event = WAIT_NONE;
        procs[i].assigned_cpu = -1;
        procs[i].iopl = 0;
        list_init(&procs[i].run_node);
        list_init(&procs[i].wait_node);
    }
    cpu_locals[0]._cur_proc = nullptr;
    cpu_locals[0].run_count = 0;
    cpu_locals[0].idle_proc = nullptr;
}

// Ensure a PDPT entry exists for the given virtual address in user PML4.
// Returns the virtual address of the PD, or allocates a new one.
static uint64_t *ensure_pd(uint64_t *new_pml4, uint64_t vaddr) {
    uint64_t pml4_idx = (vaddr >> 39) & 0x1FF;
    if (new_pml4[pml4_idx] & 0x01) {
        return (uint64_t *)phys_to_virt(new_pml4[pml4_idx] & ~0xFFF);
    }
    // Allocate new PDPT
    Page *pdpt_page = bfc_alloc.alloc_page(1);
    if (!pdpt_page) return nullptr;
    uint64_t pdpt_phys = page_to_phys(pdpt_page);
    uint64_t pdpt_virt = phys_to_virt(pdpt_phys);
    uint64_t *pdpt = (uint64_t *)pdpt_virt;
    for (int i = 0; i < 512; i++) {
        pdpt[i] = 0;
    }
    new_pml4[pml4_idx] = pdpt_phys | PTE_PRESENT | PTE_RW | PTE_USER;
    return pdpt;
}

// Ensure a PD entry exists for the given virtual address.
// Returns the virtual address of the PT.
static uint64_t *ensure_pt_in_pd(uint64_t *pd_or_pdpt, uint64_t vaddr, int level) {
    // level 2 = PDPT (need PD), level 1 = PD (need PT)
    uint64_t idx;
    if (level == 2) {
        idx = (vaddr >> 30) & 0x1FF;
    } else {
        idx = (vaddr >> 21) & 0x1FF;
    }
    if (pd_or_pdpt[idx] & 0x01) {
        return (uint64_t *)phys_to_virt(pd_or_pdpt[idx] & ~0xFFF);
    }
    // Allocate next-level table
    Page *table_page = bfc_alloc.alloc_page(1);
    if (!table_page) return nullptr;
    uint64_t table_phys = page_to_phys(table_page);
    uint64_t table_virt = phys_to_virt(table_phys);
    uint64_t *table = (uint64_t *)table_virt;
    for (int i = 0; i < 512; i++) {
        table[i] = 0;
    }
    pd_or_pdpt[idx] = table_phys | PTE_PRESENT | PTE_RW | PTE_USER;
    return table;
}

// Map a single 4KB page at vaddr into new_pml4, copying data from src.
static bool map_user_page(uint64_t *new_pml4, uint64_t vaddr, const uint8_t *src,
                          uint64_t copy_len) {
    Page *page = bfc_alloc.alloc_page(1);
    if (!page) return false;
    uint64_t page_phys = page_to_phys(page);
    uint64_t page_virt = phys_to_virt(page_phys);

    // Clear page first (handles BSS zeroing)
    uint8_t *dst = (uint8_t *)page_virt;
    for (size_t i = 0; i < PAGE_SIZE; i++) {
        dst[i] = 0;
    }

    // Copy file data
    if (src && copy_len > 0) {
        for (uint64_t i = 0; i < copy_len; i++) {
            dst[i] = src[i];
        }
    }

    // Walk page tables: PML4 → PDPT → PD → PT
    uint64_t *pdpt = ensure_pd(new_pml4, vaddr);
    if (!pdpt) return false;
    uint64_t *pd = ensure_pt_in_pd(pdpt, vaddr, 2);
    if (!pd) return false;
    uint64_t *pt = ensure_pt_in_pd(pd, vaddr, 1);
    if (!pt) return false;

    uint64_t pt_idx = (vaddr >> 12) & 0x1FF;
    pt[pt_idx] = page_phys | PTE_PRESENT | PTE_RW | PTE_USER;

    return true;
}

// Map a physical page directly at vaddr in new_pml4 (no copy, page already initialized)
// flags: PTE flags (Present+RW+User+optional NX, etc.)
static bool map_user_page_direct(uint64_t *new_pml4, uint64_t vaddr, uint64_t phys,
                                 uint64_t flags) {
    uint64_t *pdpt = ensure_pd(new_pml4, vaddr);
    if (!pdpt) return false;
    uint64_t *pd = ensure_pt_in_pd(pdpt, vaddr, 2);
    if (!pd) return false;
    uint64_t *pt = ensure_pt_in_pd(pd, vaddr, 1);
    if (!pt) return false;

    uint64_t pt_idx = (vaddr >> 12) & 0x1FF;
    pt[pt_idx] = phys | flags;
    return true;
}

void shm_init() {
    Page *p;

    p = bfc_alloc.alloc_page(1);
    if (!p) { serial_puts("shm_init: kbd alloc failed\n"); halt(); }
    kbd_shm_phys = page_to_phys(p);

    p = bfc_alloc.alloc_page(1);
    if (!p) { serial_puts("shm_init: disk_req alloc failed\n"); halt(); }
    disk_req_shm_phys = page_to_phys(p);

    p = bfc_alloc.alloc_page(1);
    if (!p) { serial_puts("shm_init: disk_resp alloc failed\n"); halt(); }
    disk_resp_shm_phys = page_to_phys(p);

    // Zero out shared pages
    uint8_t *v;
    v = (uint8_t *)phys_to_virt(kbd_shm_phys);
    for (size_t i = 0; i < PAGE_SIZE; i++) v[i] = 0;
    v = (uint8_t *)phys_to_virt(disk_req_shm_phys);
    for (size_t i = 0; i < PAGE_SIZE; i++) v[i] = 0;
    v = (uint8_t *)phys_to_virt(disk_resp_shm_phys);
    for (size_t i = 0; i < PAGE_SIZE; i++) v[i] = 0;

    serial_puts("shm_init: ok\n");
}

// Map the 3 shared pages into a user PML4
static bool map_shared_pages(uint64_t *new_pml4) {
    if (!map_user_page_direct(new_pml4, KBD_SHM_ADDR, kbd_shm_phys,
                             PTE_PRESENT | PTE_RW | PTE_USER | PTE_NX))
        return false;
    if (!map_user_page_direct(new_pml4, DISK_REQ_ADDR, disk_req_shm_phys,
                             PTE_PRESENT | PTE_RW | PTE_USER | PTE_NX))
        return false;
    if (!map_user_page_direct(new_pml4, DISK_RESP_ADDR, disk_resp_shm_phys,
                             PTE_PRESENT | PTE_RW | PTE_USER | PTE_NX))
        return false;
    return true;
}

// switch_to 恢复帧：callee-saved 寄存器 + 返回地址
struct switch_frame_t {
    uint64_t rbx;
    uint64_t rbp;
    uint64_t r12;
    uint64_t r13;
    uint64_t r14;
    uint64_t r15;
    uint64_t ret_addr;
};

// 在内核栈顶构建 trapframe + switch_frame，返回 k_rsp
static uint64_t build_kstack(uint64_t k_stack_top, uint64_t entry_rip, uint8_t iopl) {
    trapframe_t tf = {};
    tf.ss      = 0x23;                   // USER_DS
    tf.rsp     = 0x00007FFFFFFFE000;      // user stack top (top of mapped page at 0x7FFFFFFFD000)
    tf.rflags  = 0x202 | ((uint64_t)iopl << 12); // IF=1, IOPL
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

// Create idle process for the specified CPU
proc_t *create_idle_process(int cpu_id) {
    spin_lock(&procs_lock);
    proc_t *proc = nullptr;
    int alloc_idx = -1;
    for (int i = 0; i < MAX_PROC; i++) {
        if (procs[i].pid < 0) {
            proc = &procs[i];
            alloc_idx = i;
            break;
        }
    }
    if (!proc) { spin_unlock(&procs_lock); serial_puts("create_idle_process: no free slot\n"); return nullptr; }

    // Allocate kernel stack (8KB = 2 pages)
    Page *stack_pages = bfc_alloc.alloc_page(2);
    if (!stack_pages) { spin_unlock(&procs_lock); serial_puts("create_idle_process: alloc stack failed\n"); return nullptr; }
    uint64_t k_stack_phys = page_to_phys(stack_pages);
    uint64_t k_stack_top = phys_to_virt(k_stack_phys) + 2 * PAGE_SIZE;

    // Build idle switch_frame on kernel stack (no trapframe, no user mode)
    uint64_t k_rsp = build_idle_kstack(k_stack_top);

    // Fill PCB: idle uses kernel PML4, no user address space
    proc->pid = alloc_idx;
    proc->state = RUNNING;  // idle starts as RUNNING on its CPU
    proc->k_rsp = k_rsp;
    proc->k_stack_top = k_stack_top;
    proc->cr3 = PHY_ADDR((uintptr_t)pml4); // kernel PML4 physical address
    proc->entry = (uint64_t)idle_entry;
    proc->wait_event = WAIT_NONE;
    proc->assigned_cpu = cpu_id;
    proc->iopl = 0;
    list_init(&proc->run_node);
    list_init(&proc->wait_node);
    spin_unlock(&procs_lock);

    // Store in cpu_locals
    cpu_locals[cpu_id].idle_proc = proc;

    return proc;
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

proc_t *process_create(uint64_t entry) {
    // 1. Find free slot + fill PCB under procs_lock
    spin_lock(&procs_lock);
    proc_t *proc = nullptr;
    int alloc_idx = -1;
    for (int i = 0; i < MAX_PROC; i++) {
        if (procs[i].pid < 0) {
            proc = &procs[i];
            alloc_idx = i;
            break;
        }
    }
    if (!proc) {
        spin_unlock(&procs_lock);
        serial_puts("process_create: no free slot\n");
        return nullptr;
    }

    // 2. Allocate kernel stack (8KB = 2 pages)
    Page *stack_pages = bfc_alloc.alloc_page(2);
    if (!stack_pages) {
        spin_unlock(&procs_lock);
        serial_puts("process_create: alloc stack failed\n");
        return nullptr;
    }
    serial_puts("process_create: stack ok\n");
    uint64_t k_stack_phys = page_to_phys(stack_pages);
    uint64_t k_stack_top = phys_to_virt(k_stack_phys) + 2 * PAGE_SIZE;

    // 3. Allocate per-process PML4
    Page *pml4_page = bfc_alloc.alloc_page(1);
    if (!pml4_page) {
        spin_unlock(&procs_lock);
        serial_puts("process_create: alloc pml4 failed\n");
        return nullptr;
    }
    serial_puts("process_create: pml4 ok\n");
    uint64_t pml4_phys = page_to_phys(pml4_page);
    uint64_t pml4_virt = phys_to_virt(pml4_phys);

    // 4. Clear PML4
    uint64_t *new_pml4 = (uint64_t *)pml4_virt;
    for (int i = 0; i < 512; i++) {
        new_pml4[i] = 0;
    }

    // 5. Copy kernel PML4 entries (PML4[511] = higher-half)
    extern uint64_t pdpt_hh[512];
    new_pml4[511] = pml4[511];

    // 6. Map user code page at 0x400000
    Page *user_code_page = bfc_alloc.alloc_page(1);
    if (!user_code_page) {
        spin_unlock(&procs_lock);
        serial_puts("process_create: alloc code failed\n");
        return nullptr;
    }
    serial_puts("process_create: code page ok\n");
    uint64_t user_code_phys = page_to_phys(user_code_page);
    uint64_t user_code_virt = phys_to_virt(user_code_phys);

    uint8_t *code_dst = (uint8_t *)user_code_virt;
    for (size_t i = 0; i < sizeof(init_code); i++) {
        code_dst[i] = init_code[i];
    }

    if (!map_user_page_direct(new_pml4, 0x400000, user_code_phys,
                             PTE_PRESENT | PTE_RW | PTE_USER)) {
        spin_unlock(&procs_lock);
        serial_puts("process_create: map code failed\n");
        return nullptr;
    }

    // 7. Map user stack page at 0x00007FFFFFFFD000
    Page *user_stack_page = bfc_alloc.alloc_page(1);
    if (!user_stack_page) {
        spin_unlock(&procs_lock);
        serial_puts("process_create: alloc stack page failed\n");
        return nullptr;
    }
    serial_puts("process_create: stack page ok\n");
    uint64_t user_stack_phys = page_to_phys(user_stack_page);

    if (!map_user_page_direct(new_pml4, 0x00007FFFFFFFD000, user_stack_phys,
                             PTE_PRESENT | PTE_RW | PTE_USER | PTE_NX)) {
        spin_unlock(&procs_lock);
        serial_puts("process_create: map stack failed\n");
        return nullptr;
    }

    // 8. Build trapframe + switch_to frame on kernel stack
    uint64_t k_rsp = build_kstack(k_stack_top, entry, 0);

    // 9. Fill PCB (still under procs_lock)
    int assigned_cpu = pick_cpu();
    proc->pid = alloc_idx;
    proc->state = READY;
    proc->k_rsp = k_rsp;
    proc->k_stack_top = k_stack_top;
    proc->cr3 = pml4_phys;
    proc->entry = entry;
    proc->wait_event = WAIT_NONE;
    proc->assigned_cpu = assigned_cpu;
    proc->iopl = 0;
    list_init(&proc->run_node);
    list_init(&proc->wait_node);
    spin_unlock(&procs_lock);

    // Enqueue to target CPU's run_queue under scheduler_lock
    spin_lock(&cpu_locals[assigned_cpu].scheduler_lock);
    list_push_back(&cpu_locals[assigned_cpu].run_queue, &proc->run_node);
    cpu_locals[assigned_cpu].run_count++;
    spin_unlock(&cpu_locals[assigned_cpu].scheduler_lock);

    return proc;
}

proc_t *process_create_elf(const uint8_t *elf_data, uint64_t elf_size, uint8_t iopl) {
    // 1. Find free slot under procs_lock
    spin_lock(&procs_lock);
    proc_t *proc = nullptr;
    int alloc_idx = -1;
    for (int i = 0; i < MAX_PROC; i++) {
        if (procs[i].pid < 0) {
            proc = &procs[i];
            alloc_idx = i;
            break;
        }
    }
    if (!proc) { spin_unlock(&procs_lock); return nullptr; }

    // 2. Allocate kernel stack (8KB = 2 pages)
    Page *stack_pages = bfc_alloc.alloc_page(2);
    if (!stack_pages) { spin_unlock(&procs_lock); return nullptr; }
    uint64_t k_stack_phys = page_to_phys(stack_pages);
    uint64_t k_stack_top = phys_to_virt(k_stack_phys) + 2 * PAGE_SIZE;

    // 3. Allocate per-process PML4
    Page *pml4_page = bfc_alloc.alloc_page(1);
    if (!pml4_page) { spin_unlock(&procs_lock); return nullptr; }
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
    if (!lr.success) { spin_unlock(&procs_lock); serial_puts("process_create_elf: elf_load failed\n"); return nullptr; }
    serial_puts("process_create_elf: entry=");
    serial_put_hex(lr.entry);
    serial_puts("\n");

    // 6. Map shared pages (KBD, DISK_REQ, DISK_RESP)
    if (!map_shared_pages(new_pml4)) { spin_unlock(&procs_lock); serial_puts("process_create_elf: map_shared_pages failed\n"); return nullptr; }

    // Debug: verify .data page at 0x403000 is mapped
    {
        uint64_t *pdpt = ensure_pd(new_pml4, 0x403000);
        uint64_t *pd = ensure_pt_in_pd(pdpt, 0x403000, 2);
        uint64_t *pt = ensure_pt_in_pd(pd, 0x403000, 1);
        uint64_t pt_idx = (0x403000 >> 12) & 0x1FF;
        serial_puts("  .data PTE: ");
        serial_put_hex(pt[pt_idx]);
        serial_puts("\n");
        if (pt[pt_idx] & 0x01) {
            uint8_t *data_virt = (uint8_t *)phys_to_virt(pt[pt_idx] & ~0xFFF);
            serial_puts("  .data content: ");
            serial_put_hex(*(uint64_t *)data_virt);
            serial_puts("\n");
        }
    }

    // 7. Map user stack page at 0x00007FFFFFFFD000
    Page *user_stack_page = bfc_alloc.alloc_page(1);
    if (!user_stack_page) { spin_unlock(&procs_lock); return nullptr; }
    uint64_t user_stack_phys = page_to_phys(user_stack_page);

    if (!map_user_page_direct(new_pml4, 0x00007FFFFFFFD000, user_stack_phys,
                             PTE_PRESENT | PTE_RW | PTE_USER | PTE_NX)) {
        spin_unlock(&procs_lock);
        return nullptr;
    }

    // 8. Build trapframe + switch_to frame on kernel stack
    uint64_t k_rsp = build_kstack(k_stack_top, lr.entry, iopl);

    // 9. Fill PCB (still under procs_lock)
    int assigned_cpu = pick_cpu();
    proc->pid = alloc_idx;
    proc->state = READY;
    proc->k_rsp = k_rsp;
    proc->k_stack_top = k_stack_top;
    proc->cr3 = pml4_phys;
    proc->entry = lr.entry;
    proc->wait_event = WAIT_NONE;
    proc->assigned_cpu = assigned_cpu;
    proc->iopl = iopl;
    list_init(&proc->run_node);
    list_init(&proc->wait_node);
    spin_unlock(&procs_lock);

    // Enqueue to target CPU's run_queue under scheduler_lock
    spin_lock(&cpu_locals[assigned_cpu].scheduler_lock);
    list_push_back(&cpu_locals[assigned_cpu].run_queue, &proc->run_node);
    cpu_locals[assigned_cpu].run_count++;
    spin_unlock(&cpu_locals[assigned_cpu].scheduler_lock);

    return proc;
}

void schedule() {
    int my_cpu = get_cpu_local()->cpu_id;
    proc_t *idle = get_cpu_local()->idle_proc;
    proc_t *prev = current_proc;

    uint64_t flags;
    spin_lock_irqsave(&cpu_locals[my_cpu].scheduler_lock, &flags);

    // Check if run_queue has a runnable process
    if (list_empty(&cpu_locals[my_cpu].run_queue)) {
        // If prev is BLOCKED (e.g. sys_wait), it cannot continue running —
        // switch to idle so the CPU halts until an IRQ wakes a process.
        if (prev != idle && prev->state == BLOCKED) {
            current_proc = idle;
            per_cpu_tss[my_cpu].rsp0 = idle->k_stack_top;
            get_cpu_local()->tss_rsp0 = idle->k_stack_top;
            spin_unlock_irqrestore(&cpu_locals[my_cpu].scheduler_lock, flags);
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
    proc_t *next = LIST_ENTRY(next_node, proc_t, run_node);
    list_remove(&next->run_node);

    // State transition for prev
    if (prev != idle && prev->state == RUNNING) {
        prev->state = READY;
        list_push_back(&cpu_locals[my_cpu].run_queue, &prev->run_node);
        cpu_locals[my_cpu].run_count++;
    }
    // if prev->state == BLOCKED: don't enqueue, run_count unchanged (already decremented in sys_wait)

    next->state = RUNNING;
    cpu_locals[my_cpu].run_count--;
    current_proc = next;
    per_cpu_tss[my_cpu].rsp0 = next->k_stack_top;
    get_cpu_local()->tss_rsp0 = next->k_stack_top;

    // Release lock before switch_to, re-acquire after — same pattern as old BKL
    spin_unlock_irqrestore(&cpu_locals[my_cpu].scheduler_lock, flags);
    switch_to(prev, next);
    spin_lock_irqsave(&cpu_locals[my_cpu].scheduler_lock, &flags);
    spin_unlock_irqrestore(&cpu_locals[my_cpu].scheduler_lock, flags);
}
