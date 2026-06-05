#ifndef KERNEL_PROC_H
#define KERNEL_PROC_H

#include <stdint.h>
#include "arch/x86/trap.h"

typedef int32_t pid_t;

enum proc_state_t { READY, RUNNING, BLOCKED };

enum wait_event_t { WAIT_NONE, WAIT_KBD };

struct proc_t {
    pid_t pid;
    proc_state_t state;
    uint32_t k_esp;         // saved kernel ESP (for switch_to)
    uint32_t k_stack_top;   // kernel stack top (8KB region high end)
    uint32_t cr3;           // page directory physical address
    uint32_t entry;         // user entry EIP
    wait_event_t wait_event; // 阻塞原因
};

#define MAX_PROC 64

extern proc_t procs[MAX_PROC];
extern proc_t *current_proc;

extern "C" {
void proc_init();
void init_idle_proc();
proc_t *process_create(uint32_t entry);
proc_t *process_create_elf(const uint8_t *elf_data, uint32_t elf_size);
void schedule();
void switch_to(proc_t *prev, proc_t *next);
void process_entry();
}

#endif // KERNEL_PROC_H