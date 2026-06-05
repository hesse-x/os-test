#include <stdint.h>
#include <stddef.h>

#include "kernel/proc.h"
#include "kernel/serial.h"
#include "kernel/mem/alloc.h"
#include "kernel/elf.h"
#include "arch/x86/paging.h"
#include "arch/x86/trap.h"
#include "arch/x86/utils.h"

proc_t procs[MAX_PROC];
proc_t *current_proc = nullptr;

// getpid → add '0' → putc → yield → loop
static const uint8_t init_code[] = {
    0xB8, 0x01, 0x00, 0x00, 0x00,  // mov eax, 1 (sys_getpid)
    0xCD, 0x80,                      // int 0x80   ← call getpid
    0x83, 0xC0, 0x30,                // add eax, '0'
    0x89, 0xC3,                      // mov ebx, eax
    0xB8, 0x00, 0x00, 0x00, 0x00,  // mov eax, 0 (sys_putc)
    0xCD, 0x80,                      // int 0x80   ← call putc
    0xB8, 0x02, 0x00, 0x00, 0x00,  // mov eax, 2 (sys_yield)
    0xCD, 0x80,                      // int 0x80   ← call yield
    0xEB, 0xE4                       // jmp -28 (back to start)
};

extern "C" const uint8_t stack_bottom[8192];

// Convert Page descriptor pointer to physical address of the actual memory page
static uint32_t page_to_phys(Page *p) {
    return (uint32_t)(p - BFCAllocator::frames) * PAGE_SIZE;
}

// Convert physical address to higher-half virtual address
static uint32_t phys_to_virt(uint32_t phys) {
    return phys + VMA_BASE;
}

void proc_init() {
    for (int i = 0; i < MAX_PROC; i++) {
        procs[i].pid = -1;
        procs[i].state = READY;
        procs[i].k_esp = 0;
        procs[i].k_stack_top = 0;
        procs[i].cr3 = 0;
        procs[i].entry = 0;
        procs[i].wait_event = WAIT_NONE;
    }
    current_proc = nullptr;
}

void init_idle_proc() {
    proc_t *idle = &procs[0];
    idle->pid = 0;
    idle->state = RUNNING;

    // Get current ESP (boot stack position)
    uint32_t esp;
    __asm__ volatile("movl %%esp, %0" : "=r"(esp));
    idle->k_esp = esp;
    idle->k_stack_top = (uint32_t)&stack_bottom + 8192;
    idle->cr3 = PHY_ADDR((uintptr_t)page_directory);
    idle->entry = 0;
    idle->wait_event = WAIT_NONE;

    current_proc = idle;
    tss.esp0 = idle->k_stack_top;
}

proc_t *process_create(uint32_t entry) {
    // 1. Find free slot (pid == -1)
    proc_t *proc = nullptr;
    int alloc_idx = -1;
    for (int i = 0; i < MAX_PROC; i++) {
        if (procs[i].pid < 0) {
            proc = &procs[i];
            alloc_idx = i;
            break;
        }
    }
    if (!proc) return nullptr;

    // 2. Allocate kernel stack (8KB = 2 pages)
    Page *stack_pages = bfc_alloc.alloc_page(2);
    if (!stack_pages) return nullptr;
    uint32_t k_stack_phys = page_to_phys(stack_pages);
    uint32_t k_stack_top = phys_to_virt(k_stack_phys) + 2 * PAGE_SIZE;

    // 3. Allocate per-process PD
    Page *pd_page = bfc_alloc.alloc_page(1);
    if (!pd_page) return nullptr;
    uint32_t pd_phys = page_to_phys(pd_page);
    uint32_t pd_virt = phys_to_virt(pd_phys);

    // 4. Clear PD
    uint32_t *new_pd = (uint32_t *)pd_virt;
    for (int i = 0; i < 1024; i++) {
        new_pd[i] = 0;
    }

    // 5. Copy kernel PDEs (PD[768..1023]), share kernel PT
    for (int i = 768; i < 1024; i++) {
        new_pd[i] = page_directory[i];
    }

    // 6. Allocate user code page + PT
    Page *user_code_page = bfc_alloc.alloc_page(1);
    if (!user_code_page) return nullptr;
    uint32_t user_code_phys = page_to_phys(user_code_page);
    uint32_t user_code_virt = phys_to_virt(user_code_phys);

    // Copy user code bytes
    uint8_t *code_dst = (uint8_t *)user_code_virt;
    for (size_t i = 0; i < sizeof(init_code); i++) {
        code_dst[i] = init_code[i];
    }

    // Allocate PT for user code at 0x400000 (PD[1])
    Page *user_code_pt_page = bfc_alloc.alloc_page(1);
    if (!user_code_pt_page) return nullptr;
    uint32_t user_code_pt_phys = page_to_phys(user_code_pt_page);
    uint32_t user_code_pt_virt = phys_to_virt(user_code_pt_phys);

    // Clear PT and set entry
    uint32_t *code_pt = (uint32_t *)user_code_pt_virt;
    for (int i = 0; i < 1024; i++) {
        code_pt[i] = 0;
    }
    code_pt[0] = user_code_phys | 0x07;  // Present + Writable + User
    new_pd[1] = user_code_pt_phys | 0x07;

    // 7. Allocate user stack page + PT (PD[767], PT[1023] → 0xBFFFF000)
    Page *user_stack_page = bfc_alloc.alloc_page(1);
    if (!user_stack_page) return nullptr;
    uint32_t user_stack_phys = page_to_phys(user_stack_page);

    Page *user_stack_pt_page = bfc_alloc.alloc_page(1);
    if (!user_stack_pt_page) return nullptr;
    uint32_t user_stack_pt_phys = page_to_phys(user_stack_pt_page);
    uint32_t user_stack_pt_virt = phys_to_virt(user_stack_pt_phys);

    // Clear PT and set entry
    uint32_t *stack_pt = (uint32_t *)user_stack_pt_virt;
    for (int i = 0; i < 1024; i++) {
        stack_pt[i] = 0;
    }
    stack_pt[1023] = user_stack_phys | 0x07;  // PT[1023] → 0xBFFFF000
    new_pd[767] = user_stack_pt_phys | 0x07;

    // 8. Build trapframe + switch_to frame on kernel stack
    uint32_t *stack = (uint32_t *)k_stack_top;

    // trapframe (from high address downward)
    stack[-1]  = 0x23;           // ss (USER_DS)
    stack[-2]  = 0xC0000000;     // esp (stack top, grows down from 0xBFFFFFFC)
    stack[-3]  = 0x202;          // eflags (IF=1)
    stack[-4]  = 0x1B;           // cs (USER_CS)
    stack[-5]  = entry;          // eip
    stack[-6]  = 0;              // err_code
    stack[-7]  = 0;              // trapno
    stack[-8]  = 0x23;           // ds
    stack[-9]  = 0x23;           // es
    stack[-10] = 0x23;           // fs
    stack[-11] = 0x23;           // gs
    // pushregs_t: edi, esi, ebp, esp_ignored, ebx, edx, ecx, eax
    stack[-12] = 0;              // eax
    stack[-13] = 0;              // ecx
    stack[-14] = 0;              // edx
    stack[-15] = 0;              // ebx
    stack[-16] = 0;              // esp_ignored
    stack[-17] = 0;              // ebp
    stack[-18] = 0;              // esi
    stack[-19] = 0;              // edi

    // switch_to restore frame (below trapframe)
    stack[-20] = (uint32_t)process_entry;  // return address
    stack[-21] = 0;                         // ebp
    stack[-22] = 0;                         // edi
    stack[-23] = 0;                         // esi
    stack[-24] = 0;                         // ebx

    uint32_t k_esp = (uint32_t)&stack[-24];

    // 9. Fill PCB
    proc->pid = alloc_idx;
    proc->state = READY;
    proc->k_esp = k_esp;
    proc->k_stack_top = k_stack_top;
    proc->cr3 = pd_phys;
    proc->entry = entry;
    proc->wait_event = WAIT_NONE;

    return proc;
}

proc_t *process_create_elf(const uint8_t *elf_data, uint32_t elf_size) {
    // 1. Find free slot
    proc_t *proc = nullptr;
    int alloc_idx = -1;
    for (int i = 0; i < MAX_PROC; i++) {
        if (procs[i].pid < 0) {
            proc = &procs[i];
            alloc_idx = i;
            break;
        }
    }
    if (!proc) return nullptr;

    // 2. Allocate kernel stack (8KB = 2 pages)
    Page *stack_pages = bfc_alloc.alloc_page(2);
    if (!stack_pages) return nullptr;
    uint32_t k_stack_phys = page_to_phys(stack_pages);
    uint32_t k_stack_top = phys_to_virt(k_stack_phys) + 2 * PAGE_SIZE;

    // 3. Allocate per-process PD
    Page *pd_page = bfc_alloc.alloc_page(1);
    if (!pd_page) return nullptr;
    uint32_t pd_phys = page_to_phys(pd_page);
    uint32_t pd_virt = phys_to_virt(pd_phys);

    // 4. Clear PD + copy kernel PDEs
    uint32_t *new_pd = (uint32_t *)pd_virt;
    for (int i = 0; i < 1024; i++) {
        new_pd[i] = 0;
    }
    for (int i = 768; i < 1024; i++) {
        new_pd[i] = page_directory[i];
    }

    // 5. Load ELF segments into user address space
    elf_load_result lr = elf_load(elf_data, elf_size, new_pd);
    if (!lr.success) return nullptr;

    // 6. Allocate user stack page + PT (PD[767], PT[1023] → 0xBFFFF000)
    Page *user_stack_page = bfc_alloc.alloc_page(1);
    if (!user_stack_page) return nullptr;
    uint32_t user_stack_phys = page_to_phys(user_stack_page);

    Page *user_stack_pt_page = bfc_alloc.alloc_page(1);
    if (!user_stack_pt_page) return nullptr;
    uint32_t user_stack_pt_phys = page_to_phys(user_stack_pt_page);
    uint32_t user_stack_pt_virt = phys_to_virt(user_stack_pt_phys);

    uint32_t *stack_pt = (uint32_t *)user_stack_pt_virt;
    for (int i = 0; i < 1024; i++) {
        stack_pt[i] = 0;
    }
    stack_pt[1023] = user_stack_phys | 0x07;
    new_pd[767] = user_stack_pt_phys | 0x07;

    // 7. Build trapframe + switch_to frame on kernel stack
    uint32_t *stack = (uint32_t *)k_stack_top;

    stack[-1]  = 0x23;           // ss (USER_DS)
    stack[-2]  = 0xC0000000;     // esp (stack top)
    stack[-3]  = 0x202;          // eflags (IF=1)
    stack[-4]  = 0x1B;           // cs (USER_CS)
    stack[-5]  = lr.entry;       // eip
    stack[-6]  = 0;              // err_code
    stack[-7]  = 0;              // trapno
    stack[-8]  = 0x23;           // ds
    stack[-9]  = 0x23;           // es
    stack[-10] = 0x23;           // fs
    stack[-11] = 0x23;           // gs
    stack[-12] = 0;              // eax
    stack[-13] = 0;              // ecx
    stack[-14] = 0;              // edx
    stack[-15] = 0;              // ebx
    stack[-16] = 0;              // esp_ignored
    stack[-17] = 0;              // ebp
    stack[-18] = 0;              // esi
    stack[-19] = 0;              // edi

    stack[-20] = (uint32_t)process_entry;
    stack[-21] = 0;              // ebp
    stack[-22] = 0;              // edi
    stack[-23] = 0;              // esi
    stack[-24] = 0;              // ebx

    uint32_t k_esp = (uint32_t)&stack[-24];

    // 8. Fill PCB
    proc->pid = alloc_idx;
    proc->state = READY;
    proc->k_esp = k_esp;
    proc->k_stack_top = k_stack_top;
    proc->cr3 = pd_phys;
    proc->entry = lr.entry;
    proc->wait_event = WAIT_NONE;

    return proc;
}

void schedule() {
    if (current_proc == nullptr) return;

    proc_t *next = nullptr;
    // Round-robin: scan from current_proc+1, only find READY processes
    for (int i = current_proc->pid + 1; i < MAX_PROC + current_proc->pid + 1; i++) {
        int idx = i % MAX_PROC;
        if (procs[idx].pid >= 0 && procs[idx].state == READY) {
            next = &procs[idx];
            break;
        }
    }
    if (next == nullptr) return;

    // Update TSS.esp0 for next process
    tss.esp0 = next->k_stack_top;

    // Only set READY if currently RUNNING (preempted by timer/yield)
    // BLOCKED processes stay BLOCKED
    if (current_proc->state == RUNNING) {
        current_proc->state = READY;
    }
    next->state = RUNNING;

    serial_puts("schedule: ");
    serial_put_hex(current_proc->pid);
    serial_puts(" -> ");
    serial_put_hex(next->pid);
    serial_puts("\n");

    proc_t *prev = current_proc;
    current_proc = next;

    switch_to(prev, next);
    // After switch_to returns (prev is resumed), ensure interrupts are on.
    // If we were switched out from a syscall (int 0x80 gate auto-CLI'd),
    // IF is still 0 here. STI is safe: the caller will iret and restore
    // the saved EFLAGS anyway.
    __asm__ volatile("sti");
}