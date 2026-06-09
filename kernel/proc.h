#ifndef KERNEL_PROC_H
#define KERNEL_PROC_H

#include <stdint.h>
#include "arch/x64/trap.h"
#include "arch/x64/smp.h"

typedef int32_t pid_t;

enum proc_state_t { READY, RUNNING, BLOCKED };

enum wait_event_t { WAIT_NONE, WAIT_NOTIFY };

struct proc_t {
    pid_t pid;
    proc_state_t state;
    uint64_t k_rsp;        // saved kernel RSP (for switch_to)
    uint64_t k_stack_top;  // kernel stack top (8KB region high end)
    uint64_t cr3;          // PML4 physical address
    uint64_t entry;        // user entry RIP
    wait_event_t wait_event; // 阻塞原因
    int assigned_cpu;      // which CPU this process runs on
    uint8_t iopl;          // IOPL for this process (0=normal, 3=driver)
};

#define MAX_PROC 64

extern proc_t procs[MAX_PROC];
// current_proc is now per-CPU, accessed via macro in smp.h

extern "C" {
void proc_init();
void init_idle_proc();
void init_ap_idle(int cpu_id, uint64_t k_stack_top);
proc_t *process_create(uint64_t entry);
proc_t *process_create_elf(const uint8_t *elf_data, uint64_t elf_size, uint8_t iopl = 0);
void schedule();
void switch_to(proc_t *prev, proc_t *next);
void process_entry();
void shm_init();
}

#endif // KERNEL_PROC_H
