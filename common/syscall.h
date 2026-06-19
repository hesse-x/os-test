#ifndef COMMON_SYSCALL_H
#define COMMON_SYSCALL_H

#include <stdint.h>
#include "arch/x64/utils.h"

// ===================== Syscall numbers =====================
#define SYS_GETPID       0
#define SYS_YIELD        1
#define SYS_RECV         2
#define SYS_REQ          3
#define SYS_RESP         4
#define SYS_IRQ_BIND     5
#define SYS_EXIT         6
#define SYS_WAITPID      7
#define SYS_SPAWN        8
#define SYS_MMAP         9
#define SYS_MUNMAP       10
#define SYS_SERIAL_WRITE 11
#define SYS_FB_INFO      12
#define SYS_SHM_CREATE   13
#define SYS_SHM_ATTACH   14
#define SYS_PIPE         15
#define SYS_WRITE        16
#define SYS_READ         17
#define SYS_CLOSE        18
#define SYS_LOAD_DEV     19
#define SYS_DEV_MSG      20
#define SYS_NOTIFY       21
#define SYS_GETTIME      22
#define SYS_CLOCK        23
#define SYS_MSG          24
#define SYS_MSG_RESP     25
#define SYS_IOPERM       26
#define SYS_DUP2         27
#define SYS_FCNTL        28
#define SYS_DMA_ALLOC    29
#define SYS_DMA_FREE     30
#define SYS_PCI_DEV_INFO 31
#define SYS_BLOCK_READ   32
#define SYS_BLOCK_WRITE  33
#define SYS_BLOCK_ASYNC  34
#define SYS_OPEN_DEV     35
#define SYS_INSTALL_FD   36

// ===================== Syscall helpers (arch-specific) =====================
// Defined in arch/x64/utils.h as __syscall0, __syscall1, etc.

// ===================== Syscall return convention =====================
// 0 = success (for status-only syscalls: recv/req/resp/notify/exit/irq_bind/munmap)
// positive errno = error (for status-only syscalls)
// For value-returning syscalls: 0 = failure, nonzero = success
//   sys_mmap: returns mapped address on success, NULL on failure
//   sys_spawn/sys_waitpid: returns pid on success, 0 on failure
//   sys_getpid: always succeeds (returns pid >= 1)

// ===================== recv_msg (shared between kernel and user) =====================
#define RECV_IRQ    0
#define RECV_REQ     1
#define RECV_NOTIFY 2
#define RECV_MSG    3

struct recv_msg {
    uint32_t type;       // RECV_IRQ / RECV_REQ / RECV_NOTIFY / RECV_MSG
    uint32_t src;        // IRQ number or sender PID
    union {
        uint8_t  data[56];       // RECV_IRQ / RECV_REQ / RECV_NOTIFY
        struct {                  // RECV_MSG only
            void  *kmaddr;       // kernel kmalloc buffer (opaque to user)
            size_t len;          // data length
        } msg;
    };
};

// ===================== Semantic wrappers =====================
static inline int64_t sys_getpid() {
    return __syscall0(SYS_GETPID);
}

static inline void sys_yield() {
    __syscall0(SYS_YIELD);
}

static inline int sys_recv(void *buf, void *data_buf, size_t data_buf_len,
                            uint32_t timeout_ms) {
    return (int)__syscall4(SYS_RECV, (int64_t)(uintptr_t)buf,
        (int64_t)(uintptr_t)data_buf, (int64_t)data_buf_len, (int64_t)timeout_ms);
}

static inline int sys_req(int32_t pid, void *request, void *reply) {
    return (int)__syscall3(SYS_REQ, (int64_t)pid, (int64_t)(uintptr_t)request, (int64_t)(uintptr_t)reply);
}

static inline int sys_resp(void *reply) {
    return (int)__syscall1(SYS_RESP, (int64_t)(uintptr_t)reply);
}

static inline int sys_msg(int32_t target_pid, void *msg_buf, size_t msg_len,
                           void *reply_buf, size_t reply_len) {
    return (int)__syscall5(SYS_MSG, (int64_t)target_pid,
        (int64_t)(uintptr_t)msg_buf, (int64_t)msg_len,
        (int64_t)(uintptr_t)reply_buf, (int64_t)reply_len);
}

static inline int sys_msg_resp(void *resp_buf, size_t resp_len) {
    return (int)__syscall2(SYS_MSG_RESP, (int64_t)(uintptr_t)resp_buf,
        (int64_t)resp_len);
}

static inline int sys_irq_bind(int irq) {
    return (int)__syscall1(SYS_IRQ_BIND, (int64_t)irq);
}

static inline void sys_exit(int32_t exit_code) {
    __syscall1(SYS_EXIT, (int64_t)exit_code);
    // does not return
}

static inline int64_t sys_waitpid(int32_t pid, int32_t *exit_code) {
    return __syscall2(SYS_WAITPID, (int64_t)pid, (int64_t)(uintptr_t)exit_code);
}

static inline int64_t sys_spawn(const void *elf_data, uint64_t elf_size) {
    return __syscall2(SYS_SPAWN, (int64_t)(uintptr_t)elf_data, (int64_t)elf_size);
}

static inline void *sys_mmap(void *addr, size_t size, int prot, int flags, int fd, uint64_t offset) {
    return (void *)__syscall6(SYS_MMAP, (int64_t)(uintptr_t)addr, (int64_t)size,
        (int64_t)prot, (int64_t)flags, (int64_t)fd, (int64_t)offset);
}

static inline int sys_munmap(void *addr, size_t size) {
    return (int)__syscall2(SYS_MUNMAP, (int64_t)(uintptr_t)addr, (int64_t)size);
}

static inline int sys_serial_write(const char *buf, size_t len) {
    return (int)__syscall2(SYS_SERIAL_WRITE, (int64_t)(uintptr_t)buf, (int64_t)len);
}

static inline int sys_fb_info(void *buf) {
    return (int)__syscall1(SYS_FB_INFO, (int64_t)(uintptr_t)buf);
}

static inline int sys_shm_create(size_t size) {
    return (int)__syscall1(SYS_SHM_CREATE, (int64_t)size);
}

static inline int sys_shm_attach(int32_t id, int mode) {
    return (int)__syscall2(SYS_SHM_ATTACH, (int64_t)id, (int64_t)mode);
}

static inline int sys_pipe(int *fd_ptr) {
    return (int)__syscall1(SYS_PIPE, (int64_t)(uintptr_t)fd_ptr);
}

static inline int64_t sys_write(int fd, const void *buf, size_t len) {
    return __syscall3(SYS_WRITE, (int64_t)fd, (int64_t)(uintptr_t)buf, (int64_t)len);
}

static inline int64_t sys_read(int fd, void *buf, size_t len) {
    return __syscall3(SYS_READ, (int64_t)fd, (int64_t)(uintptr_t)buf, (int64_t)len);
}

static inline int sys_close(int fd) {
    return (int)__syscall1(SYS_CLOSE, (int64_t)fd);
}

static inline int sys_load_dev(int32_t pid, int dev_type) {
    return (int)__syscall2(SYS_LOAD_DEV, (int64_t)pid, (int64_t)dev_type);
}

static inline int sys_dev_msg(int fd, void *msg_buf, size_t msg_len,
                               void *reply_buf, size_t reply_len) {
    return (int)__syscall5(SYS_DEV_MSG, (int64_t)fd,
        (int64_t)(uintptr_t)msg_buf, (int64_t)msg_len,
        (int64_t)(uintptr_t)reply_buf, (int64_t)reply_len);
}

static inline int sys_notify(int32_t pid) {
    return (int)__syscall1(SYS_NOTIFY, (int64_t)pid);
}

static inline uint64_t sys_gettime() {
    return (uint64_t)__syscall0(SYS_GETTIME);
}

static inline uint64_t sys_clock() {
    return (uint64_t)__syscall0(SYS_CLOCK);
}

static inline int sys_ioperm(unsigned long from, unsigned long num, int turn_on) {
    return (int)__syscall3(SYS_IOPERM, (int64_t)from, (int64_t)num, (int64_t)turn_on);
}

static inline int sys_dup2(int old_fd, int new_fd) {
    return (int)__syscall2(SYS_DUP2, (int64_t)old_fd, (int64_t)new_fd);
}

static inline int sys_fcntl(int fd, int cmd, int arg) {
    return (int)__syscall3(SYS_FCNTL, (int64_t)fd, (int64_t)cmd, (int64_t)arg);
}

static inline int sys_dma_alloc(size_t size, void **vaddr, uint64_t *paddr) {
    return (int)__syscall3(SYS_DMA_ALLOC, (int64_t)size,
        (int64_t)(uintptr_t)vaddr, (int64_t)(uintptr_t)paddr);
}

static inline int sys_dma_free(void *vaddr) {
    return (int)__syscall1(SYS_DMA_FREE, (int64_t)(uintptr_t)vaddr);
}

// ===================== PCI device info =====================
struct pci_dev_info_bar {
    uint64_t phys;
    uint64_t size;
    uint8_t  type;   // 0=MMIO32, 1=IO, 2=MMIO64
};

struct pci_dev_info {
    uint16_t vendor_id;
    uint16_t device_id;
    uint16_t class_code;
    uint8_t  irq_pin;
    uint8_t  irq_line;
    uint8_t  num_bars;
    struct pci_dev_info_bar bars[6];
};

static inline int sys_pci_dev_info(uint8_t bus, uint8_t dev, uint8_t func,
                                    struct pci_dev_info *out) {
    return (int)__syscall4(SYS_PCI_DEV_INFO, (int64_t)bus, (int64_t)dev,
        (int64_t)func, (int64_t)(uintptr_t)out);
}

static inline int sys_block_read(uint32_t lba, void *buf, uint32_t count) {
    return (int)__syscall3(SYS_BLOCK_READ, (int64_t)lba,
        (int64_t)(uintptr_t)buf, (int64_t)count);
}

static inline int sys_block_write(uint32_t lba, const void *buf, uint32_t count) {
    return (int)__syscall3(SYS_BLOCK_WRITE, (int64_t)lba,
        (int64_t)(uintptr_t)buf, (int64_t)count);
}

// Async block I/O: returns cookie (>0) on success, -errno on error.
// Completion delivered via RECV_NOTIFY with cookie+result+lba+count in data.
static inline int sys_block_async(uint32_t lba, void *buf, uint32_t count, uint8_t dir) {
    return (int)__syscall4(SYS_BLOCK_ASYNC, (int64_t)lba,
        (int64_t)(uintptr_t)buf, (int64_t)count, (int64_t)dir);
}

// sys_open_dev(dev_type) — syscall 35 (open device node)
// Returns: (fd | target_pid << 32) on success, negative errno on failure
// Caller: fd = (int32_t)(result & 0xFFFFFFFF), pid = (pid_t)(result >> 32)
static inline uint64_t sys_open_dev(int dev_type) {
    return (uint64_t)__syscall1(SYS_OPEN_DEV, (int64_t)dev_type);
}

// sys_install_fd(fs_pid, fs_fd, offset, flags, file_size) — syscall 36
// Register an FD_FILE fd in the kernel fd_table.
// Called by libc open() after getting fs_fd from fs_driver.
// Returns: fd (>=3) on success, negative errno on failure
static inline int sys_install_fd(int32_t fs_pid, int32_t fs_fd,
                                  uint64_t offset, int flags,
                                  uint64_t file_size) {
    return (int)__syscall5(SYS_INSTALL_FD, (int64_t)fs_pid, (int64_t)fs_fd,
        (int64_t)offset, (int64_t)flags, (int64_t)file_size);
}

#endif // COMMON_SYSCALL_H
