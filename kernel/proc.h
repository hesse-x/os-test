#ifndef KERNEL_PROC_H
#define KERNEL_PROC_H

#include <stdint.h>
#include "kernel/sparse.h"
#include "kernel/list.h"
#include "kernel/mem/alloc.h"
#include "arch/x64/trap.h"
#include "arch/x64/smp.h"
#include "common/signal.h"

typedef int32_t pid_t;

typedef enum proc_state_t { UNUSED, READY, RUNNING, BLOCKED, ZOMBIE, REAPING } proc_state_t;

typedef enum wait_event_t { WAIT_NONE, WAIT_RECV, WAIT_REQ_REPLY, WAIT_CHILD, WAIT_PIPE, WAIT_MSG_REPLY, WAIT_POLL } wait_event_t;

#define RECV_MSG_SIZE   64
#define RECV_QUEUE_SIZE 16

// ===================== SHM fd model =====================
#define SHM_KERNEL  1  // page managed by kernel, don't free on ref_count==0
#define SHM_SEALED  2  // MFD_ALLOW_SEALING was set (sealing allowed)

// Linux-compatible memfd_create flags
#define MFD_CLOEXEC       0x0001U
#define MFD_ALLOW_SEALING 0x0002U

// Linux-compatible sealing constants
#define F_ADD_SEALS  1033
#define F_GET_SEALS  1034
#define F_SEAL_SEAL   0x0001  // further fcntl(F_ADD_SEALS) fails
#define F_SEAL_SHRINK 0x0002  // ftruncate shrink fails
#define F_SEAL_GROW   0x0004  // ftruncate grow fails
#define F_SEAL_WRITE  0x0008  // mmap(PROT_WRITE) fails

// fd flag for close-on-exec (stored in struct file::flags)
#define FD_CLOEXEC 0x8000

typedef struct shm {
    uint64_t phys;          // physical page start address (0 if page_list used)
    size_t   npages;        // contiguous pages (0 if page_list used)
    size_t   file_size;     // logical size set by ftruncate (≤ total * PAGE_SIZE)
    int      ref_count;     // reference count
    int      flags;         // SHM_KERNEL | SHM_SEALED
    uint32_t seals;         // active F_SEAL_* bitmask
    char     name[32];      // debug name from memfd_create (null-terminated)
    // Discrete page support for resize (when bfc_alloc can't allocate contiguous)
    uint64_t *page_list;    // NULL = pages in phys (contiguous), else each entry is a 4K page phys addr
    int      num_pages;     // page_list length (0 when page_list==NULL)
} shm_t;

typedef struct mmap_region {
    uint64_t vaddr;
    uint64_t size;
    uint64_t phys;       // physical address (for DMA buffers, non-zero = MAP_PHYSICAL)
    struct shm *shm_obj; // non-NULL = SHM fd mmap (phys/npages from this)
    struct mmap_region *next;
} mmap_region_t;

#define MAP_PHYSICAL_BASE 0x70000000  // framebuffer MAP_PHYSICAL fixed high base

// ===================== fd / pipe / shm =====================
#define MAX_FD       32
#define PIPE_BUF_SIZE 4096

#define FD_NONE   0
#define FD_PIPE   1
#define FD_SHM    2
#define FD_DEV    3
#define FD_FILE   4
#define FD_SOCKET 5
#define FD_SERIAL 6

#define O_RDONLY  0
#define O_WRONLY  1
#define O_RDWR    2
#define O_NONBLOCK 4
#define O_APPEND  8

typedef struct pipe {
    uint8_t *buf;        // 4KB ring buffer (kmalloc)
    uint32_t head;       // write position
    uint32_t tail;       // read position
    pid_t read_pid;      // reader blocked process PID (-1 if none)
    pid_t write_pid;     // writer blocked process PID (-1 if none)
    int ref_count;       // open fd count
} pipe_t;

struct unix_sock;  // forward declaration from kernel/socket.h

typedef struct file {
    int type;            // FD_NONE / FD_PIPE / FD_SHM / FD_DEV / FD_FILE / FD_SOCKET
    int flags;           // O_RDONLY / O_WRONLY / O_RDWR
    union {
        struct pipe *pipe;   // if type == FD_PIPE
        struct shm  *shm;    // if type == FD_SHM
        pid_t target_pid;    // if type == FD_DEV (driver PID)
        struct {             // if type == FD_FILE
            pid_t   fs_pid;
            int32_t fs_fd;
            uint64_t offset;
            uint64_t file_size;
            int      ref_count;
        } file_data;
        struct unix_sock *sock; // if type == FD_SOCKET
    };
} file_t;

typedef struct proc_t {
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
    uint64_t mmap_phys_brk; // MAP_PHYSICAL 区域高水位（初始 MAP_PHYSICAL_BASE）
    struct mmap_region *mmap_regions; // mmap 区域链表头
    list_node_t run_node;  // embedded in per-CPU run_queue
    list_node_t wait_node; // embedded in per-CPU timer_queue (sorted by wait_deadline)
    uint64_t wait_deadline; // sched_clock() nanosecond deadline, 0 = no timeout
    uint8_t  wait_timed_out; // 1 = timer expired wakeup, 0 = notify wakeup
    struct file fd_table[MAX_FD];  // per-process file descriptor table

    // === 统一 recv 队列 ===
    uint8_t  recv_buf[RECV_QUEUE_SIZE][RECV_MSG_SIZE]; // 16 × 64B = 1KB
    uint32_t recv_head;         // producer write position
    uint32_t recv_tail;         // consumer read position
    spinlock_t recv_lock;       // protects recv_buf/head/tail

    // === REQ 状态 ===
    pid_t    req_caller_pid;    // current REQ caller PID (-1 = none)
    void __user *req_reply_buf;     // caller's reply buffer user-space address
    int32_t  req_result;        // 0 = success, positive errno on error
    pid_t    req_target_pid;    // for crash cleanup: who we're waiting on

    // === MSG 状态（独立于 req 字段） ===
    void __user *msg_reply_buf;     // caller's reply buffer user-space address
    size_t   msg_reply_len;     // caller's reply buffer size
    pid_t    msg_caller_pid;    // server side: who sent the msg (-1 = none)
    int32_t  msg_result;        // 0 = success, negative errno on error
    pid_t    msg_target_pid;    // caller side: who we're waiting on (crash cleanup)

    // === CPU 时间记账 ===
    uint64_t cpu_time_ns;       // 累计 CPU 时间（纳秒）
    uint64_t last_sched;        // 上次被调度时的 sched_clock() 值

    // === 信号状态 ===
    struct signal_state {
        uint64_t pending;           // bitmask: pending signals (bit N = signal N)
        sigaction_t action[NSIG];  // per-signal action
        uint64_t blocked;           // blocked mask (预留)
        int      have_handler;      // 是否有用户态 handler 待调起
        // sigreturn 恢复上下文
        uint64_t saved_rip;
        uint64_t saved_rsp;
        uint64_t saved_rflags;
    } sig;
} proc_t;

#define MAX_PROC 64

extern proc_t procs[MAX_PROC];
extern spinlock_t procs_lock;
extern pid_t init_pid;
// current_proc is now per-CPU, accessed via macro in smp.h

void proc_init(void);
proc_t *process_create_elf(const uint8_t *elf_data, uint64_t elf_size);
void schedule(void);
void switch_to(proc_t *prev, proc_t *next);
void process_entry(void);
void idle_entry(void);
proc_t *create_idle_process(int cpu_id);
void proc_reap(proc_t *proc);

// SHM reference counting helpers
struct shm *shm_get(struct shm *shm);
void shm_put(struct shm *shm);

// Timer queue operations (must be called under scheduler_lock)
void timer_queue_insert(int cpu, proc_t *proc);
void timer_queue_remove(proc_t *proc);

#endif // KERNEL_PROC_H
