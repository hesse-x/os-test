#ifndef KERNEL_PROC_H
#define KERNEL_PROC_H

#include <stdint.h>
#include "kernel/list.h"
#include "kernel/mem/alloc.h"
#include "arch/x64/trap.h"
#include "arch/x64/smp.h"
#include "common/signal.h"

typedef int32_t pid_t;

enum proc_state_t { READY, RUNNING, BLOCKED, ZOMBIE, REAPING };

enum wait_event_t { WAIT_NONE, WAIT_RECV, WAIT_REQ_REPLY, WAIT_CHILD, WAIT_PIPE, WAIT_MSG_REPLY, WAIT_POLL, WAIT_FUTEX };

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

struct shm {
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
};

struct mmap_region {
    uint64_t vaddr;
    uint64_t size;
    uint64_t phys;       // physical address (for DMA buffers, non-zero = MAP_PHYSICAL)
    struct shm *shm_obj; // non-NULL = SHM fd mmap (phys/npages from this)
    mmap_region *next;
};

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

#define O_RDONLY  0
#define O_WRONLY  1
#define O_RDWR    2
#define O_NONBLOCK 4
#define O_APPEND  8

struct pipe {
    uint8_t *buf;        // 4KB ring buffer (kmalloc)
    uint32_t head;       // write position
    uint32_t tail;       // read position
    pid_t read_pid;      // reader blocked process PID (-1 if none)
    pid_t write_pid;     // writer blocked process PID (-1 if none)
    int ref_count;       // open fd count
};

struct unix_sock;  // forward declaration from kernel/socket.h

struct file {
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
};

// ===================== mm_t (address space + resources) =====================

struct mm_t {
    uint64_t cr3;              // PML4 physical address
    int ref_count;             // reference count (clone(CLONE_VM)++)

    // === fd table ===
    struct file fd_table[MAX_FD];

    // === mmap ===
    uint64_t mmap_brk;         // mmap 高水位（初始 0x800000）
    uint64_t mmap_phys_brk;    // MAP_PHYSICAL 高水位
    mmap_region *mmap_regions; // mmap 区域链表

    // === 信号（线程组共享） ===
    struct {
        uint64_t shared_pending;   // 进程级 pending（kill 产生）
        spinlock_t sig_lock;       // 保护 shared_pending
        struct sigaction action[NSIG];
    } sig;

    // === 进程关系 ===
    pid_t parent_pid;          // 父进程 PID（getppid 返回此值）

    // === 线程组退出 ===
    uint8_t group_exit;        // exit_group 标志
    int32_t group_exit_code;   // exit_group 退出码
};

// ===================== task_t (scheduling entity) =====================
// IMPORTANT: The first 5 fields (tid, state, k_rsp, k_stack_top, cr3) must
// remain at fixed offsets for assembly compatibility in switch_to:
//   k_rsp at offset 8, cr3 at offset 24.

struct task_t {
    pid_t tid;              // 全局唯一 ID（== 数组下标）
    proc_state_t state;     // READY/RUNNING/BLOCKED/ZOMBIE/REAPING
    uint64_t k_rsp;         // switch_to 保存的内核 RSP  (offset 8)
    uint64_t k_stack_top;   // 内核栈顶（8KB 高端）
    uint64_t cr3;           // PML4 物理地址 (offset 24, cached from mm->cr3)
    pid_t tgid;             // 线程组 ID（= 主线程 tid，单线程时 tgid==tid）
    uint64_t entry;         // 用户入口 RIP
    wait_event_t wait_event;// 阻塞原因
    int assigned_cpu;       // 绑定的 CPU
    list_node_t run_node;   // per-CPU run_queue 链表节点
    list_node_t wait_node;  // per-CPU timer_queue 链表节点
    uint64_t wait_deadline; // sched_clock() 超时纳秒
    uint8_t  wait_timed_out;// 超时标志
    struct mm_t *mm;        // 指向地址空间（线程组共享，idle 进程为 NULL）
    uint8_t *iopm;          // per-task IOPL 位图

    // === per-task IPC 状态 ===
    uint8_t  recv_buf[RECV_QUEUE_SIZE][RECV_MSG_SIZE];
    uint32_t recv_head;
    uint32_t recv_tail;
    spinlock_t recv_lock;
    pid_t    req_caller_pid;
    void    *req_reply_buf;
    int32_t  req_result;
    pid_t    req_target_pid;
    void    *msg_reply_buf;
    size_t   msg_reply_len;
    pid_t    msg_caller_pid;
    int32_t  msg_result;
    pid_t    msg_target_pid;

    // === per-task 信号 ===
    uint64_t sig_pending;       // 私有 pending（pthread_kill 产生）
    uint64_t sig_blocked;       // 信号阻塞掩码

    // === per-task 退出 ===
    int32_t  exit_code;         // 退出码
    pid_t    clear_tid_addr;    // CLONE_CHILD_CLEARTID 用户态地址（0=无）

    // === CPU 时间 ===
    uint64_t cpu_time_ns;
    uint64_t last_sched;

    // === FPU 状态 ===
    uint8_t  used_fpu;          // 该 task 是否使用过 FPU
    void    *fpu_state;         // fxsave 区域（lazy 分配）

    // === 信号 handler 状态（per-task，暂保留在 task 中） ===
    int      sig_have_handler;  // 是否有用户态 handler 待调起
    uint64_t sig_saved_rip;
    uint64_t sig_saved_rsp;
    uint64_t sig_saved_rflags;

    // === futex 等待 ===
    list_node_t futex_node;     // 挂在 futex hash bucket 链表上
    uint64_t futex_uaddr;       // 等待的用户态地址

    // === FS_BASE (TLS) ===
    uint64_t fs_base;           // 保存的 FS_BASE 值
};

#define MAX_PROC 64

extern task_t tasks[MAX_PROC];
extern spinlock_t tasks_lock;
extern pid_t init_pid;
// current_task is per-CPU, accessed via macro in smp.h

extern "C" {
void proc_init();
task_t *process_create_elf(const uint8_t *elf_data, uint64_t elf_size);
void schedule();
void switch_to(task_t *prev, task_t *next);
void process_entry();
void idle_entry();
task_t *create_idle_process(int cpu_id);
void proc_reap(task_t *task);
}

// mm_t helpers
mm_t *mm_create();
void mm_release(mm_t *mm);
void mm_release_pages(mm_t *mm);
void mm_put(mm_t *mm);

// Fork helpers
void copy_fd_table(mm_t *dst, mm_t *src);
mmap_region *copy_mmap_regions(mmap_region *src);
uint64_t copy_page_table(uint64_t src_pml4_phys, mm_t *src_mm);

// switch_to restore frame: callee-saved registers + return address
struct switch_frame_t {
    uint64_t rbx;
    uint64_t rbp;
    uint64_t r12;
    uint64_t r13;
    uint64_t r14;
    uint64_t r15;
    uint64_t ret_addr;
};

// CPU pick helper
int pick_cpu();

// SHM reference counting helpers
struct shm *shm_get(struct shm *shm);
void shm_put(struct shm *shm);

// Timer queue operations (must be called under scheduler_lock)
void timer_queue_insert(int cpu, task_t *task);
void timer_queue_remove(task_t *task);

#endif // KERNEL_PROC_H
