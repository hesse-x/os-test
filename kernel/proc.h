#ifndef KERNEL_PROC_H
#define KERNEL_PROC_H

#include <stdint.h>
#include "kernel/sparse.h"
#include "kernel/list.h"
#include "kernel/mem/alloc.h"
#include "arch/x64/trap.h"
#include "arch/x64/smp.h"
#include "common/signal.h"
#include "common/mman.h"
#include "common/types.h"

// ===================== SHM fd model =====================
#define SHM_KERNEL  1  // page managed by kernel, don't free on ref_count==0
#define SHM_SEALED  2  // MFD_ALLOW_SEALING was set (sealing allowed)

typedef enum proc_state_t { UNUSED, READY, RUNNING, BLOCKED, ZOMBIE, REAPING } proc_state_t;

typedef enum wait_event_t { WAIT_NONE, WAIT_RECV, WAIT_REQ_REPLY, WAIT_CHILD, WAIT_PIPE, WAIT_MSG_REPLY, WAIT_POLL } wait_event_t;

#define RECV_MSG_SIZE   64
#define RECV_QUEUE_SIZE 16

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

#define FD_NONE    0
#define FD_PIPE    1
#define FD_REGULAR 2
#define FD_DEV     3
#define FD_DIR     4
#define FD_SOCKET  5
#define FD_SHM     6
#define FD_FILE    7
#define FD_TTY     8

#include "common/fcntl.h"

typedef struct pipe {
    uint8_t *buf;        // 4KB ring buffer (kmalloc)
    uint32_t head;       // write position
    uint32_t tail;       // read position
    pid_t read_pid;      // reader blocked process PID (-1 if none)
    pid_t write_pid;     // writer blocked process PID (-1 if none)
    int ref_count;       // open fd count
} pipe_t;

struct unix_sock;  // forward declaration from kernel/socket.h
struct inode;      // forward declaration from kernel/inode.h
struct pty;        // forward declaration from kernel/pty.h

typedef struct file {
    int type;            // FD_NONE / FD_PIPE / FD_REGULAR / FD_DEV / FD_DIR / FD_SOCKET / FD_SHM / FD_FILE
    int flags;           // O_RDONLY / O_WRONLY / O_RDWR
    struct inode *inode; // FD_REGULAR, FD_DEV, FD_DIR shared field
    uint64_t offset;     // per-open offset (FD_REGULAR)
    union {
        struct pipe *pipe;       // if type == FD_PIPE
        struct shm  *shm;        // if type == FD_SHM
        pid_t target_pid;        // if type == FD_DEV (driver PID, user-space driver only)
        struct {                  // if type == FD_FILE (user-space FS proxy)
            pid_t   fs_pid;
            int32_t fs_fd;
            uint64_t _offset;
            uint64_t file_size;
            int      ref_count;
        } file_data;
        struct unix_sock *sock;  // if type == FD_SOCKET
        struct pty *pty;         // if type == FD_TTY
    };
} file_t;

// ===================== files_t (file descriptor table, independent refcount) =====================
typedef struct files_t {
    struct file fd_table[MAX_FD];     // per-process file descriptor table
    int ref_count;                    // reference count, initial=1; CLONE_FILES shares +1
} files_t;

// ===================== mm_t (address space) =====================
typedef struct mm_t {
    uint64_t cr3;                     // authoritative PML4 physical address (task_t.cr3 is cached copy)
    int ref_count;                    // COW/CLONE_VM reserved, initial=1
    struct files_t *files;            // file descriptor table pointer (independent refcount)
    uint64_t mmap_brk;               // mmap area high watermark (initial 0x800000)
    uint64_t mmap_phys_brk;          // MAP_PHYSICAL area high watermark (initial MAP_PHYSICAL_BASE)
    struct mmap_region *mmap_regions; // mmap region list head (includes user stack region)
    pid_t    parent_pid;             // parent process PID
} mm_t;

typedef struct task_t {
    pid_t pid;                  // offset 0 (switch_to compatible)
    proc_state_t state;         // offset 4
    // 4 bytes padding
    uint64_t k_rsp;             // offset 16 (switch_to: movq %rsp, 8(%rdi))
    uint64_t k_stack_top;       // offset 24
    uint64_t cr3;               // offset 32 (cached PML4 phys, authoritative copy in mm->cr3)
    uint64_t entry;             // user entry RIP
    wait_event_t wait_event;    // block reason
    pid_t tgid;                 // thread group ID (== pid for single-threaded)
    struct mm_t *mm;            // address space pointer (NULL for idle)
    int assigned_cpu;           // which CPU this process runs on
    uint8_t *iopm;              // IOPM bitmap (NULL = deny all), 8KB if allocated
    int32_t exit_code;          // exit code, valid when ZOMBIE
    list_node_t run_node;       // embedded in per-CPU run_queue
    list_node_t wait_node;      // embedded in per-CPU timer_queue (sorted by wait_deadline)
    uint64_t wait_deadline;     // sched_clock() nanosecond deadline, 0 = no timeout
    uint8_t  wait_timed_out;    // 1 = timer expired wakeup, 0 = notify wakeup
    uint8_t  recv_intr;         // set by wake_process when WAIT_RECV, checked by sys_recv for EINTR

    // === unified recv queue ===
    uint8_t  recv_buf[RECV_QUEUE_SIZE][RECV_MSG_SIZE]; // 16 × 64B = 1KB
    uint32_t recv_head;         // producer write position
    uint32_t recv_tail;         // consumer read position
    spinlock_t recv_lock;       // protects recv_buf/head/tail

    // === REQ state ===
    pid_t    req_caller_pid;    // current REQ caller PID (-1 = none)
    void __user *req_reply_buf; // caller's reply buffer user-space address
    size_t   req_reply_len;     // reply buffer size (RECV_MSG_SIZE for sys_req, 56 for ioctl proxy)
    int32_t  req_result;        // 0 = success, positive errno on error
    pid_t    req_target_pid;    // for crash cleanup: who we're waiting on

    // === MSG state (independent of req fields) ===
    void __user *msg_reply_buf; // caller's reply buffer user-space address
    size_t   msg_reply_len;     // caller's reply buffer size
    pid_t    msg_caller_pid;    // server side: who sent the msg (-1 = none)
    int32_t  msg_result;        // 0 = success, negative errno on error
    pid_t    msg_target_pid;    // caller side: who we're waiting on (crash cleanup)

    // === CPU time accounting ===
    uint64_t cpu_time_ns;       // accumulated CPU time (nanoseconds)
    uint64_t last_sched;        // sched_clock() value at last scheduling

    // === signal state ===
    struct signal_state {
        uint64_t      pending;           // bitmask: pending signals
        sigset_t      blocked;           // currently blocked signal set
        struct sigaction action[NSIG];   // per-signal handler
    } sig;

    // === force_sig temp data ===
    siginfo_t sig_force_info;  // force_sig temp siginfo (kernel-stack scope)

    // === Session / controlling terminal (reserved for job control) ===
    pid_t    sid;              // session ID (0 = no session)
    pid_t    pgid;             // process group ID (0 = none)
    struct pty *ctty;          // controlling terminal (NULL = none)
} task_t;

#define MAX_PROC 64

extern task_t tasks[MAX_PROC];
extern spinlock_t tasks_lock;
extern pid_t init_pid;
// current_task is per-CPU, accessed via macro in smp.h

void proc_init(void);
task_t *process_create_elf(const uint8_t *elf_data, uint64_t elf_size);
void schedule(void) __attribute__((no_sanitize("kernel-address")));
void switch_to(task_t *prev, task_t *next);
void process_entry(void);
void idle_entry(void) __attribute__((no_sanitize("kernel-address")));
task_t *create_idle_process(int cpu_id);
void task_reap(task_t *proc);

// ===================== mm_t lifecycle =====================
mm_t *mm_create(void);                      // kmalloc + ref_count=1 + allocate PML4
void  mm_put(mm_t *mm);                     // atomic --ref_count; if==0 call mm_release
void  mm_release(mm_t *mm, pid_t owner_pid); // free user page tables+phys pages+PML4+mmap+SHM+files
void  mm_release_pages(mm_t *mm);           // free user page tables+phys pages+PML4 only (for execve)

// ===================== files_t lifecycle =====================
files_t *files_create(void);                // kmalloc + ref_count=1 + fd_table init FD_NONE
void     files_put(files_t *files);         // atomic --ref_count; if==0 close all fds + kfree

// SHM reference counting helpers
struct shm *shm_get(struct shm *shm);
void shm_put(struct shm *shm);

// Page table deep copy (for fork)
int copy_page_table(uint64_t *src_pml4, uint64_t *dst_pml4, mmap_region_t *mmap_regions);

// mmap region allocation helper
mmap_region_t *add_mmap_region(task_t *proc, uint64_t vaddr, uint64_t size,
                                uint64_t phys, struct shm *shm_obj);

// Timer queue operations (must be called under scheduler_lock)
void timer_queue_insert(int cpu, task_t *proc);
void timer_queue_remove(task_t *proc);

// Internal helper: close a single fd in a files_t
void close_fd_internal(files_t *files, int fd);

#endif // KERNEL_PROC_H
