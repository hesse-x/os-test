#ifndef KERNEL_PROC_H
#define KERNEL_PROC_H

#include <stdint.h>
#include "kernel/list.h"
#include "kernel/mem/alloc.h"
#include "arch/x64/trap.h"
#include "arch/x64/smp.h"

typedef int32_t pid_t;

enum proc_state_t { READY, RUNNING, BLOCKED, ZOMBIE, REAPING };

enum wait_event_t { WAIT_NONE, WAIT_RECV, WAIT_REQ_REPLY, WAIT_CHILD, WAIT_PIPE, WAIT_MSG_REPLY };

#define RECV_MSG_SIZE   64
#define RECV_QUEUE_SIZE 16

struct mmap_region {
    uint64_t vaddr;
    uint64_t size;
    uint64_t phys;       // physical address (for DMA buffers)
    mmap_region *next;
};

#define MAX_SHM_PER_PROC 4
#define SHM_VADDR_BASE 0x510000

// ===================== fd / pipe =====================
#define MAX_FD       32
#define PIPE_BUF_SIZE 4096

#define FD_NONE   0
#define FD_PIPE   1

#define O_RDONLY  0
#define O_WRONLY  1
#define O_RDWR    2
#define O_NONBLOCK 4

struct pipe {
    uint8_t *buf;        // 4KB ring buffer (kmalloc)
    uint32_t head;       // write position
    uint32_t tail;       // read position
    pid_t read_pid;      // reader blocked process PID (-1 if none)
    pid_t write_pid;     // writer blocked process PID (-1 if none)
    int ref_count;       // open fd count
};

struct file {
    int type;            // FD_NONE / FD_PIPE
    int flags;           // O_RDONLY / O_WRONLY / O_RDWR
    struct pipe *pipe;   // if type == FD_PIPE
};

struct shm_region {
    uint64_t vaddr;       // virtual address in this process
    uint64_t phys;        // physical page start address
    size_t   npages;      // number of pages
    uint32_t ref_count;   // reference count (0 = free slot)
};

struct proc_t {
    pid_t pid;
    proc_state_t state;
    uint64_t k_rsp;        // saved kernel RSP (for switch_to)
    uint64_t k_stack_top;  // kernel stack top (8KB region high end)
    uint64_t cr3;          // PML4 physical address
    uint64_t entry;        // user entry RIP
    wait_event_t wait_event; // 阻塞原因
    int assigned_cpu;      // which CPU this process runs on
    uint8_t *iopm;         // IOPM bitmap (NULL = deny all), 8KB if allocated
    pid_t parent_pid;      // 父进程 PID，启动时进程设为 -1
    int32_t exit_code;     // 退出码，ZOMBIE 时有效
    uint64_t mmap_brk;     // mmap 区域高水位（初始 0x800000）
    mmap_region *mmap_regions; // mmap 区域链表头
    list_node_t run_node;  // embedded in per-CPU run_queue
    list_node_t wait_node; // embedded in per-CPU timer_queue (sorted by wait_deadline)
    uint64_t wait_deadline; // sched_clock() nanosecond deadline, 0 = no timeout
    uint8_t  wait_timed_out; // 1 = timer expired wakeup, 0 = notify wakeup
    shm_region shm_regions[MAX_SHM_PER_PROC]; // dynamic shared memory regions
    struct file fd_table[MAX_FD];  // per-process file descriptor table

    // === 统一 recv 队列 ===
    uint8_t  recv_buf[RECV_QUEUE_SIZE][RECV_MSG_SIZE]; // 16 × 64B = 1KB
    uint32_t recv_head;         // producer write position
    uint32_t recv_tail;         // consumer read position
    spinlock_t recv_lock;       // protects recv_buf/head/tail

    // === REQ 状态 ===
    pid_t    req_caller_pid;    // current REQ caller PID (-1 = none)
    void    *req_reply_buf;     // caller's reply buffer user-space address
    int32_t  req_result;        // 0 = success, positive errno on error
    pid_t    req_target_pid;    // for crash cleanup: who we're waiting on

    // === MSG 状态（独立于 req 字段） ===
    void    *msg_reply_buf;     // caller's reply buffer user-space address
    size_t   msg_reply_len;     // caller's reply buffer size
    pid_t    msg_caller_pid;    // server side: who sent the msg (-1 = none)
    int32_t  msg_result;        // 0 = success, negative errno on error
    pid_t    msg_target_pid;    // caller side: who we're waiting on (crash cleanup)

    // === CPU 时间记账 ===
    uint64_t cpu_time_ns;       // 累计 CPU 时间（纳秒）
    uint64_t last_sched;        // 上次被调度时的 sched_clock() 值
};

#define MAX_PROC 64

extern proc_t procs[MAX_PROC];
extern spinlock_t procs_lock;
extern pid_t init_pid;
// current_proc is now per-CPU, accessed via macro in smp.h

extern "C" {
void proc_init();
proc_t *process_create_elf(const uint8_t *elf_data, uint64_t elf_size);
void schedule();
void switch_to(proc_t *prev, proc_t *next);
void process_entry();
void idle_entry();
proc_t *create_idle_process(int cpu_id);
void proc_reap(proc_t *proc);
}

// Timer queue operations (must be called under scheduler_lock)
void timer_queue_insert(int cpu, proc_t *proc);
void timer_queue_remove(proc_t *proc);

#endif // KERNEL_PROC_H
