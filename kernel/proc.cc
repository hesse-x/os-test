#include <stdint.h>
#include <stddef.h>

#include "kernel/proc.h"
#include "kernel/serial.h"
#include "kernel/mem/alloc.h"
#include "arch/x86/paging.h"
#include "arch/x86/trap.h"
#include "arch/x86/utils.h"

proc_t procs[MAX_PROC];
proc_t *current_proc = nullptr;

// jmp $ — 2 bytes, infinite loop (hlt is privileged in ring 3)
static const uint8_t init_code[] = {0xEB, 0xFE};

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

    // 3. Use global PD — all processes share kernel address space
    uint32_t pd_phys = PHY_ADDR((uintptr_t)page_directory);

    // 4. Map user code in global PD at 0x400000 (only once)
    static bool user_code_mapped = false;
    if (!user_code_mapped) {
        Page *user_code_page = bfc_alloc.alloc_page(1);
        if (!user_code_page) return nullptr;
        uint32_t user_code_phys = page_to_phys(user_code_page);
        uint32_t user_code_virt = phys_to_virt(user_code_phys);

        // Copy user code bytes to physical page
        uint8_t *code_dst = (uint8_t *)user_code_virt;
        for (size_t i = 0; i < sizeof(init_code); i++) {
            code_dst[i] = init_code[i];
        }

        // Allocate PT for user code at 0x400000 (PD[1])
        Page *user_pt_page = bfc_alloc.alloc_page(1);
        if (!user_pt_page) return nullptr;
        uint32_t user_pt_phys = page_to_phys(user_pt_page);
        uint32_t user_pt_virt = phys_to_virt(user_pt_phys);

        // Clear PT
        uint32_t *pt = (uint32_t *)user_pt_virt;
        for (int i = 0; i < 1024; i++) {
            pt[i] = 0;
        }
        // PT[0] = user code page, Present + Writable + User (0x07)
        pt[0] = user_code_phys | 0x07;

        // PD[1] = user PT, Present + Writable + User (0x07)
        page_directory[1] = user_pt_phys | 0x07;

        user_code_mapped = true;
    }

    // 5. Build trapframe + switch_to frame on kernel stack
    uint32_t *stack = (uint32_t *)k_stack_top;

    // trapframe (from high address downward)
    stack[-1]  = 0x23;           // ss (USER_DS)
    stack[-2]  = 0xBFFFFFFC;     // esp (dummy)
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

    // 6. Fill PCB
    proc->pid = alloc_idx;
    proc->state = READY;
    proc->k_esp = k_esp;
    proc->k_stack_top = k_stack_top;
    proc->cr3 = pd_phys;
    proc->entry = entry;

    return proc;
}

void schedule() {
    if (current_proc == nullptr) return;

    proc_t *next = nullptr;
    // Round-robin: scan from current_proc+1
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

    // Update process states
    current_proc->state = READY;
    next->state = RUNNING;

    serial_puts("schedule: proc ");
    serial_put_hex(current_proc->pid);
    serial_puts(" -> proc ");
    serial_put_hex(next->pid);
    serial_puts("\n");

    proc_t *prev = current_proc;
    current_proc = next;

    switch_to(prev, next);
}