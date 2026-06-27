#include "kernel/pty.h"
#include "kernel/serial.h"
#include "kernel/mem/slab.h"
#include "kernel/inode.h"
#include "kernel/proc.h"
#include "kernel/trap.h"
#include "kernel/mem/kasan.h"
#include "common/errno.h"
#include "common/dev.h"
#include "common/signal.h"
#include "common/socket.h"
#include <stddef.h>

// ===================== Default termios =====================
const struct termios default_termios = {
    .c_iflag = ICRNL | IXON,
    .c_oflag = OPOST | ONLCR,
    .c_cflag = CS8 | CLOCAL,
    .c_lflag = ISIG | ICANON | ECHO | ECHOE | ECHOK | IEXTEN,
    .c_cc = {
        [VINTR]    = 0x03,   // Ctrl-C
        [VQUIT]    = 0x1C,   /* Ctrl-backslash */
        [VERASE]   = 0x7F,   // DEL
        [VKILL]    = 0x15,   // Ctrl-U
        [VEOF]     = 0x04,   // Ctrl-D
        [VTIME]    = 0,
        [VMIN]     = 1,
        [VSTART]   = 0x11,   // Ctrl-Q
        [VSTOP]    = 0x13,   // Ctrl-S
        [VSUSP]    = 0x1A,   // Ctrl-Z
    },
};

// ===================== Global PTY table =====================
struct pty *pty_table[MAX_PTY];
spinlock_t pty_alloc_lock = SPINLOCK_INIT;

// Cached ptmx inode pointer for master/slave identification
static struct inode *ptmx_inode;

// ===================== Ring buffer helpers =====================
int pty_ring_avail(uint32_t head, uint32_t tail) {
    return (int)((head - tail) % PTY_BUF_SIZE);
}

int pty_ring_space(uint32_t head, uint32_t tail) {
    return PTY_BUF_SIZE - 1 - pty_ring_avail(head, tail);
}

int pty_ring_write(uint8_t *buf, uint32_t *head, uint32_t tail,
                   const uint8_t *data, int len) {
    int space = pty_ring_space(*head, tail);
    int n = (len < space) ? len : space;
    for (int i = 0; i < n; i++) {
        buf[*head] = data[i];
        *head = (*head + 1) % PTY_BUF_SIZE;
    }
    return n;
}

int pty_ring_read(uint8_t *buf, uint32_t head, uint32_t *tail,
                  uint8_t *data, int len) {
    int avail = pty_ring_avail(head, *tail);
    int n = (len < avail) ? len : avail;
    for (int i = 0; i < n; i++) {
        data[i] = buf[*tail];
        *tail = (*tail + 1) % PTY_BUF_SIZE;
    }
    return n;
}

// ===================== Helpers =====================
static int pty_eintr_check(task_t *proc) {
    uint64_t pend = __atomic_load_n(&proc->sig.pending, __ATOMIC_ACQUIRE);
    uint64_t deliv = pend & ~proc->sig.blocked;
    deliv |= (pend & ((1ULL << SIGKILL) | (1ULL << SIGSTOP)));
    return deliv ? 1 : 0;
}

// Write a single byte to ring, return 1 on success, 0 if no space
static int pty_ring_write1(uint8_t *buf, uint32_t *head, uint32_t tail, uint8_t byte) {
    if (pty_ring_space(*head, tail) == 0) return 0;
    buf[*head] = byte;
    *head = (*head + 1) % PTY_BUF_SIZE;
    return 1;
}

// Check O_NONBLOCK for a PTY fd of given master/slave type in this proc
static int pty_is_nonblock(task_t *proc, struct pty *pty, int is_master) {
    files_t *files = proc->mm->files;
    for (int i = 0; i < MAX_FD; i++) {
        if (files->fd_table[i].type == FD_TTY && files->fd_table[i].pty == pty) {
            if (pty_fd_is_master(files, i) == is_master)
                return (files->fd_table[i].flags & O_NONBLOCK) ? 1 : 0;
        }
    }
    return 0;
}

// ===================== pty_init =====================
static struct dev_ops ptmx_ops;

void pty_init(void) {
    for (int i = 0; i < MAX_PTY; i++)
        pty_table[i] = NULL;
    pty_alloc_lock = SPINLOCK_INIT;

    ptmx_ops.driver_pid = 0;
    ptmx_ops.device_type = DEV_PTMX;
    ptmx_ops.open = ptmx_open;
    devtmpfs_create("ptmx", DEV_PTMX, &ptmx_ops);

    ptmx_inode = devtmpfs_lookup("ptmx");

    serial_printf("pty_init: /dev/ptmx registered\n");
}

// ===================== pty_alloc / pty_free =====================
struct pty *pty_alloc(int *out_index) {
    spin_lock(&pty_alloc_lock);
    int index = -1;
    for (int i = 0; i < MAX_PTY; i++) {
        if (pty_table[i] == NULL) { index = i; break; }
    }
    if (index < 0) {
        spin_unlock(&pty_alloc_lock);
        return NULL;
    }

    struct pty *pty = (struct pty *)kmalloc(sizeof(struct pty));
    if (!pty) {
        spin_unlock(&pty_alloc_lock);
        return NULL;
    }
    __memset(pty, 0, sizeof(struct pty));

    pty->t_termios = default_termios;
    pty->t_winsize.ws_row = 80;
    pty->t_winsize.ws_col = 25;
    pty->m_read_pid = -1;
    pty->m_write_pid = -1;
    pty->s_read_pid = -1;
    pty->s_write_pid = -1;
    pty->index = index;
    pty->master_refs = 0;
    pty->slave_refs = 0;
    pty->slave_opened = 0;
    pty->t_sid = 0;
    pty->t_pgid = 0;
    pty->pts_priv = NULL;

    pty_table[index] = pty;
    *out_index = index;
    spin_unlock(&pty_alloc_lock);
    return pty;
}

void pty_free(struct pty *pty) {
    spin_lock(&pty_alloc_lock);
    pty_table[pty->index] = NULL;
    spin_unlock(&pty_alloc_lock);
    kfree(pty);
}

// ===================== ptmx_open =====================
int ptmx_open(struct task_t *proc, int fd) {
    int index;
    struct pty *pty = pty_alloc(&index);
    if (!pty) return -ENOMEM;

    pty->master_refs = 1;

    proc->mm->files->fd_table[fd].type = FD_TTY;
    proc->mm->files->fd_table[fd].pty = pty;
    proc->mm->files->fd_table[fd].flags = O_RDWR;

    // Create pts_dev_priv for slave device
    struct pts_dev_priv *priv = (struct pts_dev_priv *)kmalloc(sizeof(struct pts_dev_priv));
    if (!priv) {
        pty->master_refs = 0;
        pty_free(pty);
        return -ENOMEM;
    }
    __memset(priv, 0, sizeof(struct pts_dev_priv));
    priv->ops.driver_pid = 0;
    priv->ops.device_type = DEV_PTS_SLAVE;
    priv->ops.open = pts_open;
    priv->pty = pty;
    pty->pts_priv = priv;

    // Register /dev/ptsN
    char name[16] = "pts";
    int pos = 3;
    if (index == 0) {
        name[pos++] = '0';
    } else {
        char tmp[8]; int tpos = 0; int n = index;
        while (n > 0) { tmp[tpos++] = '0' + (n % 10); n /= 10; }
        for (int i = tpos - 1; i >= 0; i--) name[pos++] = tmp[i];
    }
    name[pos] = '\0';
    devtmpfs_create(name, DEV_PTS_SLAVE, &priv->ops);

    serial_printf("pty: allocated pty%d, master fd=%d\n", index, fd);
    return 0;
}

// ===================== pts_open =====================
int pts_open(struct task_t *proc, int fd) {
    struct inode *ip = proc->mm->files->fd_table[fd].inode;
    if (!ip || !ip->i_priv) return -ENODEV;

    struct dev_ops *ops = (struct dev_ops *)ip->i_priv;
    struct pts_dev_priv *priv = (struct pts_dev_priv *)ops;
    struct pty *pty = priv->pty;
    if (!pty) return -ENODEV;

    if (pty->slave_opened) return -EBUSY;

    // Change fd to FD_TTY, keep inode pointing to ptsN
    proc->mm->files->fd_table[fd].type = FD_TTY;
    proc->mm->files->fd_table[fd].pty = pty;
    proc->mm->files->fd_table[fd].flags = O_RDWR;

    pty->slave_refs = 1;
    pty->slave_opened = 1;

    serial_printf("pty: opened slave pty%d, fd=%d\n", pty->index, fd);
    return 0;
}

// ===================== pty_fd_is_master =====================
int pty_fd_is_master(files_t *files, int fd) {
    return (files->fd_table[fd].inode == ptmx_inode) ? 1 : 0;
}

// ===================== pty_master_read =====================
int64_t pty_master_read(struct pty *pty, struct task_t *proc,
                        void *buf, size_t len) {
    while (pty_ring_avail(pty->s_to_m_head, pty->s_to_m_tail) == 0) {
        if (pty->slave_refs == 0) return 0;  // EOF
        if (pty_is_nonblock(proc, pty, 1)) return -EAGAIN;

        pty->m_read_pid = proc->pid;
        proc->state = BLOCKED;
        proc->wait_event = WAIT_PIPE;
        schedule();
        pty->m_read_pid = -1;
        if (pty_eintr_check(proc)) return -EINTR;
    }

    int nread = pty_ring_read(pty->s_to_m_buf, pty->s_to_m_head,
                               &pty->s_to_m_tail, (uint8_t *)buf, (int)len);
    if (pty->s_write_pid >= 0) wake_process(pty->s_write_pid);
    return (int64_t)nread;
}

// ===================== pty_master_write =====================
int64_t pty_master_write(struct pty *pty, struct task_t *proc,
                         const void *buf, size_t len) {
    size_t written = 0;

    while (written < len) {
        int n = pty_ring_write(pty->m_to_s_buf, &pty->m_to_s_head,
                                pty->m_to_s_tail,
                                (const uint8_t *)buf + written,
                                (int)(len - written));
        if (n > 0) {
            written += n;
            if (pty->s_read_pid >= 0) wake_process(pty->s_read_pid);
            continue;
        }

        // Buffer full
        if (pty_is_nonblock(proc, pty, 1)) {
            if (written > 0) break;
            return -EAGAIN;
        }

        pty->m_write_pid = proc->pid;
        proc->state = BLOCKED;
        proc->wait_event = WAIT_PIPE;
        schedule();
        pty->m_write_pid = -1;
        if (pty_eintr_check(proc)) {
            if (written > 0) break;
            return -EINTR;
        }
    }

    return (int64_t)written;
}

// ===================== pty_slave_read =====================
int64_t pty_slave_read(struct pty *pty, struct task_t *proc,
                        void *buf, size_t len) {
    if (pty->master_refs == 0) return -EPIPE;

    while (pty_ring_avail(pty->m_to_s_head, pty->m_to_s_tail) == 0) {
        if (pty->master_refs == 0) return 0;  // EOF
        if (pty_is_nonblock(proc, pty, 0)) return -EAGAIN;

        pty->s_read_pid = proc->pid;
        proc->state = BLOCKED;
        proc->wait_event = WAIT_PIPE;
        schedule();
        pty->s_read_pid = -1;
        if (pty_eintr_check(proc)) return -EINTR;
        if (pty->master_refs == 0) return 0;
    }

    int nread = pty_ring_read(pty->m_to_s_buf, pty->m_to_s_head,
                               &pty->m_to_s_tail, (uint8_t *)buf, (int)len);
    if (pty->m_write_pid >= 0) wake_process(pty->m_write_pid);
    return (int64_t)nread;
}

// ===================== pty_slave_write (with OPOST+ONLCR) =====================
int64_t pty_slave_write(struct pty *pty, struct task_t *proc,
                         const void *buf, size_t len) {
    if (pty->master_refs == 0) return -EPIPE;

    int do_opost = (pty->t_termios.c_oflag & OPOST) && (pty->t_termios.c_oflag & ONLCR);
    size_t written = 0;

    while (written < len) {
        const uint8_t ch = ((const uint8_t *)buf)[written];
        int wrote = 0;

        if (do_opost && ch == '\n') {
            // Need 2 bytes of ring space for \r\n
            if (pty_ring_space(pty->s_to_m_head, pty->s_to_m_tail) >= 2) {
                pty_ring_write1(pty->s_to_m_buf, &pty->s_to_m_head,
                                pty->s_to_m_tail, '\r');
                pty_ring_write1(pty->s_to_m_buf, &pty->s_to_m_head,
                                pty->s_to_m_tail, '\n');
                wrote = 1;
            }
        } else {
            wrote = pty_ring_write1(pty->s_to_m_buf, &pty->s_to_m_head,
                                     pty->s_to_m_tail, ch);
        }

        if (wrote) {
            written++;
            if (pty->m_read_pid >= 0) wake_process(pty->m_read_pid);
            continue;
        }

        // No space
        if (pty_is_nonblock(proc, pty, 0)) {
            if (written > 0) break;
            return -EAGAIN;
        }

        pty->s_write_pid = proc->pid;
        proc->state = BLOCKED;
        proc->wait_event = WAIT_PIPE;
        schedule();
        pty->s_write_pid = -1;
        if (pty_eintr_check(proc)) {
            if (written > 0) break;
            return -EINTR;
        }
    }

    return (int64_t)written;
}

// ===================== pty_close_fd =====================
void pty_close_fd(struct task_t *proc, int fd) {
    files_t *files = proc->mm->files;
    struct pty *pty = files->fd_table[fd].pty;
    if (!pty) return;

    int is_master = pty_fd_is_master(files, fd);

    if (is_master) {
        pty->master_refs--;
        if (pty->master_refs == 0) {
            // Wake slave read/write — they get EPIPE
            if (pty->s_read_pid >= 0) wake_process(pty->s_read_pid);
            if (pty->s_write_pid >= 0) wake_process(pty->s_write_pid);
        }
    } else {
        pty->slave_refs--;
        if (pty->slave_refs == 0) {
            // Clear m_to_s buffer so master read won't get stale data
            pty->m_to_s_head = 0;
            pty->m_to_s_tail = 0;
            // Wake master read — it gets EOF
            if (pty->m_read_pid >= 0) wake_process(pty->m_read_pid);
            pty->slave_opened = 0;

            // Remove /dev/ptsN from devtmpfs
            char name[16] = "pts";
            int pos = 3;
            int idx = pty->index;
            if (idx == 0) {
                name[pos++] = '0';
            } else {
                char tmp[8]; int tpos = 0; int n = idx;
                while (n > 0) { tmp[tpos++] = '0' + (n % 10); n /= 10; }
                for (int i = tpos - 1; i >= 0; i--) name[pos++] = tmp[i];
            }
            name[pos] = '\0';
            devtmpfs_remove(name);

            if (pty->pts_priv) {
                kfree(pty->pts_priv);
                pty->pts_priv = NULL;
            }
        }
    }

    // Free pty when both sides are fully closed
    if (pty->master_refs == 0 && pty->slave_refs == 0) {
        pty_free(pty);
    }
}

// ===================== pty_dup_fd =====================
void pty_dup_fd(files_t *files, int fd) {
    struct pty *pty = files->fd_table[fd].pty;
    if (!pty) return;

    int is_master = pty_fd_is_master(files, fd);
    if (is_master)
        pty->master_refs++;
    else
        pty->slave_refs++;
}

// ===================== pty_ioctl =====================
long pty_ioctl(struct pty *pty, uint32_t cmd, void *arg) {
    switch (cmd) {
    case TCGETS:   // 0x5401
        if (copy_to_user((void __user *)arg, &pty->t_termios, sizeof(struct termios)))
            return -EFAULT;
        return 0;

    case TCSETS:   // 0x5402
        if (copy_from_user(&pty->t_termios, (const void __user *)arg, sizeof(struct termios)))
            return -EFAULT;
        return 0;

    case TCSETSW:  // 0x5403 — same as TCSETS (no output drain)
        if (copy_from_user(&pty->t_termios, (const void __user *)arg, sizeof(struct termios)))
            return -EFAULT;
        return 0;

    case TCSETSF:  // 0x5404 — same as TCSETS + flush ring buffers
        if (copy_from_user(&pty->t_termios, (const void __user *)arg, sizeof(struct termios)))
            return -EFAULT;
        pty->m_to_s_head = pty->m_to_s_tail = 0;
        pty->s_to_m_head = pty->s_to_m_tail = 0;
        return 0;

    case TIOCGPGRP: // 0x540F
        {
            pid_t pgid = pty->t_pgid;
            if (copy_to_user((void __user *)arg, &pgid, sizeof(pid_t)))
                return -EFAULT;
            return 0;
        }

    case TIOCSPGRP: // 0x5410
        {
            pid_t pgid;
            if (copy_from_user(&pgid, (const void __user *)arg, sizeof(pid_t)))
                return -EFAULT;
            if (pgid < 0) return -EINVAL;
            pty->t_pgid = pgid;
            return 0;
        }

    case TIOCGWINSZ: // 0x5413
        if (copy_to_user((void __user *)arg, &pty->t_winsize, sizeof(struct winsize)))
            return -EFAULT;
        return 0;

    case TIOCSWINSZ: // 0x5414
        {
            struct winsize old_ws = pty->t_winsize;
            if (copy_from_user(&pty->t_winsize, (const void __user *)arg, sizeof(struct winsize)))
                return -EFAULT;
            // Future: if size changed and t_sid!=0, send SIGWINCH to session
            (void)old_ws;
            return 0;
        }

    case TIOCGPTN:  // 0x5406 — get PTY index
        {
            int idx = pty->index;
            if (copy_to_user((void __user *)arg, &idx, sizeof(int)))
                return -EFAULT;
            return 0;
        }

    case TIOCSPTLCK: // 0x5407 — lock/unlock slave (stub)
        return 0;

    default:
        return -ENOTTY;
    }
}

// ===================== pty_poll =====================
uint32_t pty_poll(struct pty *pty, int is_master, uint32_t events) {
    uint32_t revents = 0;
    if (is_master) {
        if (events & POLLIN) {
            if (pty_ring_avail(pty->s_to_m_head, pty->s_to_m_tail) > 0)
                revents |= POLLIN;
            if (pty->slave_refs == 0)
                revents |= POLLHUP | POLLIN;
        }
        if (events & POLLOUT) {
            if (pty_ring_space(pty->m_to_s_head, pty->m_to_s_tail) > 0)
                revents |= POLLOUT;
        }
    } else {
        if (events & POLLIN) {
            if (pty_ring_avail(pty->m_to_s_head, pty->m_to_s_tail) > 0)
                revents |= POLLIN;
            if (pty->master_refs == 0)
                revents |= POLLHUP | POLLIN;
        }
        if (events & POLLOUT) {
            if (pty_ring_space(pty->s_to_m_head, pty->s_to_m_tail) > 0)
                revents |= POLLOUT;
        }
    }
    return revents;
}
