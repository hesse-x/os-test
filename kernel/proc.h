#ifndef KERNEL_PROC_H
#define KERNEL_PROC_H

#include <stdint.h>
#include "kernel/list.h"
#include "kernel/mem/alloc.h"
#include "arch/x64/trap.h"
#include "arch/x64/smp.h"

typedef int32_t pid_t;

enum proc_state_t { READY, RUNNING, BLOCKED, ZOMBIE };

enum wait_event_t { WAIT_NONE, WAIT_NOTIFY, WAIT_CHILD };

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
    pid_t parent_pid;      // 父进程 PID，启动时进程设为 -1
    int32_t exit_code;     // 退出码，ZOMBIE 时有效
    uint64_t brk;          // 堆顶地址，idle=0，用户进程=0x600000
    list_node_t run_node;  // embedded in per-CPU run_queue
    list_node_t wait_node; // embedded in wait_queue (reserved)
};

#define MAX_PROC 64

extern proc_t procs[MAX_PROC];
extern spinlock_t procs_lock;
// current_proc is now per-CPU, accessed via macro in smp.h

extern "C" {
void proc_init();
proc_t *process_create(uint64_t entry);
proc_t *process_create_elf(const uint8_t *elf_data, uint64_t elf_size, uint8_t iopl = 0, bool map_fb = false);
void schedule();
void switch_to(proc_t *prev, proc_t *next);
void process_entry();
void idle_entry();
proc_t *create_idle_process(int cpu_id);
void shm_init();
void proc_reap(proc_t *proc);
}

#endif // KERNEL_PROC_H
