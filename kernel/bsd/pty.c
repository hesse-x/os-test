/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#include "kernel/bsd/pty.h"

#include <stdbool.h>
#include <stddef.h>

#include "arch/x64/smp.h"
#include "arch/x64/utils.h"
#include "kernel/bsd/inode.h"
#include "kernel/bsd/kfcntl.h"
#include "kernel/bsd/proc.h"
#include "kernel/bsd/signal.h"
#include "kernel/bsd/types.h"
#include "kernel/driver/serial.h"
#include "kernel/xcore/list.h"
#include "kernel/xcore/log.h"
#include "kernel/xcore/mem/kasan.h"
#include "kernel/xcore/mem/slab.h"
#include "kernel/xcore/rcu.h"
#include "kernel/xcore/sched.h"
#include "kernel/xcore/sparse.h"

#include <xos/capability.h>
#include <xos/errno.h>
#include <xos/ioctl.h>
#include <xos/signal.h>
#include <xos/socket.h>

// ===================== Default termios =====================
const struct termios default_termios = {
    .c_iflag = ICRNL | IXON,
    .c_oflag = OPOST | ONLCR,
    .c_cflag = CS8 | CLOCAL,
    .c_lflag = ISIG | ICANON | ECHO | ECHOE | ECHOK | ECHOCTL | IEXTEN,
    .c_cc =
        {
            [VINTR] = 0x03,  // Ctrl-C
            [VQUIT] = 0x1C,  // Ctrl-backslash
            [VERASE] = 0x7F, // DEL
            [VKILL] = 0x15,  // Ctrl-U
            [VEOF] = 0x04,   // Ctrl-D
            [VTIME] = 0,
            [VMIN] = 1,
            [VSTART] = 0x11,   // Ctrl-Q
            [VSTOP] = 0x13,    // Ctrl-S
            [VSUSP] = 0x1A,    // Ctrl-Z
            [VREPRINT] = 0x12, // Ctrl-R
            [VWERASE] = 0x17,  // Ctrl-W
            [VLNEXT] = 0x16,   // Ctrl-V
        },
};

// ===================== Global PTY table =====================
struct pty *pty_table[MAX_PTY];
spinlock pty_alloc_lock = SPINLOCK_INIT;

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

int pty_ring_read(uint8_t *buf, uint32_t head, uint32_t *tail, uint8_t *data,
                  int len) {
  int avail = pty_ring_avail(head, *tail);
  int n = (len < avail) ? len : avail;
  for (int i = 0; i < n; i++) {
    data[i] = buf[*tail];
    *tail = (*tail + 1) % PTY_BUF_SIZE;
  }
  return n;
}

// ===================== Helpers =====================
static int pty_eintr_check(xtask *proc) { return signal_pending(proc) ? 1 : 0; }

// pty wq callback: __wake_up wakes the blocked reader/writer on pty->wq.
// wait_event is not consulted (queue-identity model: being on pty->wq means
// wake).
static void pty_wake_cb(wait_queue_t *wq, unsigned long flags) {
  xtask *target = (xtask *)wq->data;
  (void)flags;
  wake_wq_target(target);
}

// Enroll on the PTY wait queue and publish BLOCKED as one atomic wait
// preparation step.  Interrupts remain disabled until the caller has
// re-checked its condition and either schedules or cancels the wait.
static uint64_t pty_prepare_wait(struct pty *pty, wait_queue_t *wait,
                                 xtask *proc) {
  uint64_t flags;
  spin_lock_irqsave(&pty->wq->lock, &flags);
  list_push_back(&pty->wq->head, &wait->node);

  int cpu = proc->assigned_cpu;
  spin_lock(&cpu_locals[cpu].scheduler_lock);
  proc->state = BLOCKED;
  proc->wait_event = WAIT_NONE;
  spin_unlock(&cpu_locals[cpu].scheduler_lock);

  // Keep IRQs disabled until the caller completes the resource re-check, so
  // wait publication and validation remain one local critical section.
  spin_unlock(&pty->wq->lock);
  return flags;
}

static void pty_finish_wait_prepare(uint64_t flags) {
  __asm__ volatile("pushq %0; popfq" : : "r"(flags));
}

// Re-arm a task whose wait node is already linked. The caller must re-check
// the resource condition before restoring interrupts or scheduling.
static uint64_t pty_reprepare_wait(xtask *proc) {
  uint64_t flags;
  __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags));
  int cpu = proc->assigned_cpu;
  spin_lock(&cpu_locals[cpu].scheduler_lock);
  proc->state = BLOCKED;
  proc->wait_event = WAIT_NONE;
  spin_unlock(&cpu_locals[cpu].scheduler_lock);
  return flags;
}

// Write a single byte to ring, return 1 on success, 0 if no space
static int pty_ring_write1(uint8_t *buf, uint32_t *head, uint32_t tail,
                           uint8_t byte) {
  if (pty_ring_space(*head, tail) == 0)
    return 0;
  buf[*head] = byte;
  *head = (*head + 1) % PTY_BUF_SIZE;
  return 1;
}

// Check O_NONBLOCK for a PTY fd of given master/slave type in this proc
static int pty_is_nonblock(xtask *proc, struct pty *pty, int is_master) {
  files *files = proc->proc->files;
  rcu_read_lock();
  for (int i = 0; i < MAX_FD; i++) {
    struct file *f = fd_lookup(files, i);
    if (f && f->type == FD_TTY && f->pty == pty) {
      if (pty_is_master_inode(f->inode) == is_master) {
        rcu_read_unlock();
        return (f->flags & O_NONBLOCK) ? 1 : 0;
      }
    }
  }
  rcu_read_unlock();
  return 0;
}

// ===================== pty_init =====================
static struct dev_ops ptmx_ops;

void pty_init(void) {
  for (int i = 0; i < MAX_PTY; i++)
    pty_table[i] = NULL;
  pty_alloc_lock = SPINLOCK_INIT;

  ptmx_ops.driver_pid = 0;
  ptmx_ops.is_block = false;
  ptmx_ops.open = ptmx_open;
  devtmpfs_create("ptmx", &ptmx_ops, NULL);

  ptmx_inode = devtmpfs_lookup("ptmx");
  // ptmx_inode is kept only for pointer-identity comparisons
  // (pty_fd_is_master / pty_is_master_inode). devtmpfs_lookup now returns a
  // +1 reference; we don't need to own one — the dev_list entry keeps the
  // inode alive for the system's lifetime (ptmx is a kernel device, never
  // removed). Drop the lookup ref so we don't leak it.
  inode_put(ptmx_inode);

  printk(LOG_INFO, "pty_init: /dev/ptmx registered\n");
}

// ===================== pty_alloc / pty_free =====================
struct pty *pty_alloc(int *out_index) {
  spin_lock(&pty_alloc_lock);
  int index = -1;
  for (int i = 0; i < MAX_PTY; i++) {
    if (pty_table[i] == NULL) {
      index = i;
      break;
    }
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
  pty->t_winsize.ws_row = 0;
  pty->t_winsize.ws_col = 0;
  pty->t_winsize.ws_xpixel = 0;
  pty->t_winsize.ws_ypixel = 0;
  pty->index = index;
  pty->s_to_m_lock = SPINLOCK_INIT;
  pty->master_refs = 0;
  pty->slave_refs = 0;
  pty->slave_opened = 0;
  pty->eof_pending = 0;
  pty->t_sid = 0;
  pty->t_pgid = 0;
  pty->pts_priv = NULL;

  // Eager-allocate wq: the pty close path (proc.c pty_close_file) must call
  // __wake_up on pty->wq to wake the peer (§5.2). OOM fallback: free pty,
  // return NULL.
  pty->wq = (wait_queue_head *)kmalloc(sizeof(wait_queue_head));
  if (!pty->wq) {
    spin_unlock(&pty_alloc_lock);
    kfree(pty);
    return NULL;
  }
  init_wait_queue_head(pty->wq);

  pty_table[index] = pty;
  *out_index = index;
  spin_unlock(&pty_alloc_lock);
  return pty;
}

void pty_free(struct pty *pty) {
  spin_lock(&pty_alloc_lock);
  pty_table[pty->index] = NULL;
  spin_unlock(&pty_alloc_lock);
  // If the slave side was never opened, the slave-close branch in
  // pty_close_file never ran, so /dev/pts/N is still registered and
  // pts_priv still owns a dangling pty pointer. Unregister + free here so a
  // later ptmx_open can't land on a stale node (open("/dev/pts/N") would read
  // a freed pty via priv->pty → __wake_up(NULL) panic). devtmpfs_remove is
  // idempotent for a node the slave-close branch already removed.
  if (pty->pts_priv) {
    char name[16];
    pty_slave_name(pty->index, name);
    devtmpfs_remove(name);
    kfree(pty->pts_priv);
    pty->pts_priv = NULL;
  }
  if (pty->wq)
    kfree(pty->wq);
  kfree(pty);
}

// ===================== pty_slave_name =====================
void pty_slave_name(int index, char out[16]) {
  int pos = 0;
  out[pos++] = 'p';
  out[pos++] = 't';
  out[pos++] = 's';
  out[pos++] = '/';
  if (index == 0) {
    out[pos++] = '0';
  } else {
    char tmp[8];
    int tpos = 0;
    int n = index;
    while (n > 0) {
      tmp[tpos++] = '0' + (n % 10);
      n /= 10;
    }
    for (int i = tpos - 1; i >= 0; i--)
      out[pos++] = tmp[i];
  }
  out[pos] = '\0';
}

// ===================== ptmx_open =====================
int ptmx_open(xtask *proc, int fd) {
  int index;
  struct pty *pty = pty_alloc(&index);
  if (!pty)
    return -ENOMEM;

  // Create pts_dev_priv for slave device
  struct pts_dev_priv *priv =
      (struct pts_dev_priv *)kmalloc(sizeof(struct pts_dev_priv));
  if (!priv) {
    pty_free(pty);
    return -ENOMEM;
  }

  // All allocations succeeded — commit state
  pty->master_refs = 1;

  __memset(priv, 0, sizeof(struct pts_dev_priv));
  priv->ops.driver_pid = 0;
  priv->ops.is_block = false;
  priv->ops.open = pts_open;
  priv->pty = pty;
  pty->pts_priv = priv;

  // Mutate the FD_DEV file (installed by devtmpfs_open) into FD_TTY master.
  // Keep its inode (ptmx_inode) so pty_fd_is_master / pty_close_file identify
  // this fd as master. The inode ref held by devtmpfs_open is released on
  // close via file_put(FD_TTY) → inode_put.
  struct file *f = fd_lookup(proc->proc->files, fd);
  f->type = FD_TTY;
  f->pty = pty;
  f->flags = O_RDWR;

  // Register /dev/pts/N (devtmpfs subdirectory, same shape as dri/card0)
  char name[16];
  pty_slave_name(index, name);
  devtmpfs_create(name, &priv->ops, NULL);

  return 0;
}

// ===================== pts_open =====================
int pts_open(xtask *proc, int fd) {
  struct file *f = fd_lookup(proc->proc->files, fd);
  if (!f)
    return -EBADF;
  struct inode *ip = f->inode;
  if (!ip || !ip->i_priv)
    return -ENODEV;

  struct dev_ops *ops = (struct dev_ops *)ip->i_priv;
  struct pts_dev_priv *priv = (struct pts_dev_priv *)ops;
  struct pty *pty = priv->pty;
  if (!pty)
    return -ENODEV;

  if (pty->slave_opened)
    return -EBUSY;

  // Mutate the FD_DEV file (installed by devtmpfs_open) into FD_TTY slave.
  // Keep its inode (pts_slave inode); the ref held by devtmpfs_open is
  // released on close via file_put(FD_TTY) → inode_put.
  f->type = FD_TTY;
  f->pty = pty;
  f->flags = O_RDWR;

  pty->slave_refs = 1;
  pty->slave_opened = 1;

  // Wake a master blocked in pty_master_read waiting for slave to open,
  // so it re-checks slave_refs and proceeds to block for data.
  __wake_up(pty->wq, POLLIN);

  return 0;
}

// ===================== pty_fd_is_master =====================
int pty_fd_is_master(files *files, int fd) {
  rcu_read_lock();
  struct file *f = fd_lookup(files, fd);
  int result = 0;
  if (f)
    result = (f->inode == ptmx_inode) ? 1 : 0;
  rcu_read_unlock();
  return result;
}

// Check if inode is the ptmx master inode
int pty_is_master_inode(struct inode *inode) {
  return (inode == ptmx_inode) ? 1 : 0;
}

// ===================== pty_master_read =====================
int64_t pty_master_read(struct pty *pty, xtask *proc, void *buf, size_t len) {
  wait_queue_t wait;
  wait.func = pty_wake_cb;
  wait.data = proc;
  wait.exclusive = 0;
  list_init(&wait.node);
  add_wait_queue(pty->wq, &wait);
  for (;;) {
    uint64_t wait_flags = pty_reprepare_wait(proc);
    spin_lock(&pty->s_to_m_lock);
    int avail = pty_ring_avail(pty->s_to_m_head, pty->s_to_m_tail);
    spin_unlock(&pty->s_to_m_lock);
    if (avail != 0) {
      remove_wait_queue(pty->wq, &wait);
      sched_cancel_spurious_wake(proc);
      pty_finish_wait_prepare(wait_flags);
      break;
    }

    // EOF only after slave was opened and then closed.
    // If slave has never been opened, avoid false EOF that triggers
    // premature re-fork in terminal — block or return EAGAIN instead.
    if (pty->slave_refs == 0) {
      if (pty->slave_opened) {
        printk(LOG_INFO, "pty_master_read: EOF pty=%d (slave closed)\n",
               pty->index);
        remove_wait_queue(pty->wq, &wait);
        sched_cancel_spurious_wake(proc);
        pty_finish_wait_prepare(wait_flags);
        return 0; // real EOF
      }
      // Slave not yet opened: block or EAGAIN
      if (pty_is_nonblock(proc, pty, 1)) {
        remove_wait_queue(pty->wq, &wait);
        sched_cancel_spurious_wake(proc);
        pty_finish_wait_prepare(wait_flags);
        return -EAGAIN;
      }
      if (pty_eintr_check(proc)) {
        remove_wait_queue(pty->wq, &wait);
        sched_cancel_spurious_wake(proc);
        pty_finish_wait_prepare(wait_flags);
        return -ERESTART;
      }
      schedule();
      pty_finish_wait_prepare(wait_flags);
      if (pty_eintr_check(proc)) {
        remove_wait_queue(pty->wq, &wait);
        sched_cancel_spurious_wake(proc);
        return -ERESTART;
      }
      continue; // re-check conditions after wake
    }
    if (pty_is_nonblock(proc, pty, 1)) {
      remove_wait_queue(pty->wq, &wait);
      sched_cancel_spurious_wake(proc);
      pty_finish_wait_prepare(wait_flags);
      return -EAGAIN;
    }

    if (pty_eintr_check(proc)) {
      remove_wait_queue(pty->wq, &wait);
      sched_cancel_spurious_wake(proc);
      pty_finish_wait_prepare(wait_flags);
      return -ERESTART;
    }
    schedule();
    pty_finish_wait_prepare(wait_flags);
    if (pty_eintr_check(proc)) {
      remove_wait_queue(pty->wq, &wait);
      sched_cancel_spurious_wake(proc);
      return -ERESTART;
    }
  }
  spin_lock(&pty->s_to_m_lock);
  int nread = pty_ring_read(pty->s_to_m_buf, pty->s_to_m_head,
                            &pty->s_to_m_tail, (uint8_t *)buf, (int)len);
  spin_unlock(&pty->s_to_m_lock);
  __wake_up(pty->wq, POLLOUT);
  return (int64_t)nread;
}

// ===================== N_TTY line discipline (input direction) =============
// Minimal kernel N_TTY (terminal/step1.md §3.4): ICANON line buffering with
// VERASE/VKILL/VEOF/VWERASE/VREPRINT/VLNEXT editing,
// ECHO/ECHOE/ECHOK/ECHOCTL, ICRNL mapping, and ISIG
// signal characters delivered to the foreground pgid. Applies to
// master->slave input only; output (slave->master) keeps the existing
// OPOST/ONLCR path. Deliberately not implemented (recorded in todo.md):
// INLCR/IGNCR, IXON/IXOFF, VMIN/VTIME timers, NOFLSH, and VEOL/EOL2.

// Committed bytes readable by the slave.
static int ldisc_canon_avail(struct pty *pty) {
  return pty_ring_avail(pty->canon_commit, pty->canon_tail);
}

// Flush all input state (TCSETSF, ICANON transitions, signal characters).
static void ldisc_flush_input(struct pty *pty) {
  pty->canon_tail = pty->canon_commit = pty->canon_head = 0;
  pty->m_to_s_head = pty->m_to_s_tail = 0;
  pty->eof_pending = 0;
  pty->lnext_pending = 0;
}

// Echo goes through the slave_write output path so OPOST/ONLCR applies
// uniformly to program output and echo. May block on a full output ring,
// same as program output.
static void ldisc_echo(struct pty *pty, xtask *proc, const char *s, int n) {
  pty_slave_write(pty, proc, s, (size_t)n);
}

// Caret notation for control bytes ("^C") when ECHOCTL is active.
static void ldisc_echo_caret(struct pty *pty, xtask *proc, uint8_t b) {
  char caret[2] = {'^', (char)(b == 0x7F ? '?' : b + '@')};
  ldisc_echo(pty, proc, caret, 2);
}

static int ldisc_echoes_as_caret(tcflag_t lflag, uint8_t b) {
  return (lflag & ECHOCTL) && (b < ' ' || b == 0x7F) && b != '\t' && b != '\n';
}

static void ldisc_echo_byte(struct pty *pty, xtask *proc, tcflag_t lflag,
                            uint8_t b) {
  if (ldisc_echoes_as_caret(lflag, b))
    ldisc_echo_caret(pty, proc, b);
  else
    ldisc_echo(pty, proc, (const char *)&b, 1);
}

static void ldisc_echo_erase(struct pty *pty, xtask *proc, tcflag_t lflag,
                             uint8_t b) {
  int columns = ldisc_echoes_as_caret(lflag, b) ? 2 : 1;
  while (columns-- > 0)
    ldisc_echo(pty, proc, "\b \b", 3);
}

static int ldisc_word_space(uint8_t b) { return b == ' ' || b == '\t'; }

// Match a signal character. c_cc value 0 is _POSIX_VDISABLE — never matches.
static int ldisc_sig_char(struct termios *t, uint8_t b, int *sig) {
  if (b != 0 && b == t->c_cc[VINTR]) {
    *sig = SIGINT;
    return 1;
  }
  if (b != 0 && b == t->c_cc[VQUIT]) {
    *sig = SIGQUIT;
    return 1;
  }
  if (b != 0 && b == t->c_cc[VSUSP]) {
    *sig = SIGTSTP;
    return 1;
  }
  return 0;
}

// Process one input byte from the master side. Returns 0 if consumed, 1 if
// the raw-mode input ring is full and the caller must apply its
// block/EAGAIN policy (canonical input never blocks: a full queue drops the
// byte and echoes BEL).
static int ldisc_input(struct pty *pty, xtask *proc, uint8_t b) {
  struct termios *t = &pty->t_termios;
  tcflag_t lflag = t->c_lflag;
  int sig;
  int literal = pty->lnext_pending;

  if (literal)
    pty->lnext_pending = 0;

  // 1. Signal characters (canonical and raw alike). With no foreground
  // process group there is nobody to signal: fall through and treat the
  // byte as ordinary input rather than dropping it.
  if (!literal && (lflag & ISIG) && ldisc_sig_char(t, b, &sig) &&
      pty->t_pgid > 0) {
    ldisc_flush_input(pty);
    if (lflag & ECHO)
      ldisc_echo_byte(pty, proc, lflag, b);
    pgsignal(pty->t_pgid, sig);
    return 0;
  }

  // 2. Input mapping.
  if (!literal && (t->c_iflag & ICRNL) && b == '\r')
    b = '\n';

  // 3. Raw/cbreak mode: straight into the master->slave ring. Only the
  // VMIN=1/VTIME=0 semantics (any byte is immediately readable); full
  // VMIN/VTIME timers are M3.
  if (!(lflag & ICANON)) {
    if (!pty_ring_write1(pty->m_to_s_buf, &pty->m_to_s_head, pty->m_to_s_tail,
                         b))
      return 1;
    if (lflag & ECHO)
      ldisc_echo_byte(pty, proc, lflag, b);
    __wake_up(pty->wq, POLLIN);
    return 0;
  }

  // 4. Canonical editing characters (c_cc==0 disables each, as above).
  if (!literal && b != 0 && b == t->c_cc[VEOF]) {
    if (pty->canon_head == pty->canon_commit)
      pty->eof_pending = 1; // empty line: one-shot EOF for the next read
    else
      pty->canon_commit = pty->canon_head; // commit without trailing newline
    if (lflag & ECHO)
      ldisc_echo_byte(pty, proc, lflag, b);
    __wake_up(pty->wq, POLLIN);
    return 0;
  }
  if (!literal && b != 0 && b == t->c_cc[VERASE]) {
    if (pty->canon_head != pty->canon_commit) {
      pty->canon_head = (pty->canon_head + PTY_BUF_SIZE - 1) % PTY_BUF_SIZE;
      if ((lflag & (ECHO | ECHOE)) == (ECHO | ECHOE))
        ldisc_echo_erase(pty, proc, lflag, pty->canon_buf[pty->canon_head]);
    }
    return 0;
  }
  if (!literal && b != 0 && b == t->c_cc[VKILL]) {
    if (pty->canon_head != pty->canon_commit) {
      pty->canon_head = pty->canon_commit;
      if ((lflag & (ECHO | ECHOK)) == (ECHO | ECHOK))
        ldisc_echo(pty, proc, "\n", 1);
    }
    return 0;
  }
  if (!literal && (lflag & IEXTEN) && b != 0 && b == t->c_cc[VWERASE]) {
    while (pty->canon_head != pty->canon_commit) {
      uint32_t prev = (pty->canon_head + PTY_BUF_SIZE - 1) % PTY_BUF_SIZE;
      if (!ldisc_word_space(pty->canon_buf[prev]))
        break;
      pty->canon_head = prev;
      if ((lflag & (ECHO | ECHOE)) == (ECHO | ECHOE))
        ldisc_echo_erase(pty, proc, lflag, pty->canon_buf[prev]);
    }
    while (pty->canon_head != pty->canon_commit) {
      uint32_t prev = (pty->canon_head + PTY_BUF_SIZE - 1) % PTY_BUF_SIZE;
      if (ldisc_word_space(pty->canon_buf[prev]))
        break;
      pty->canon_head = prev;
      if ((lflag & (ECHO | ECHOE)) == (ECHO | ECHOE))
        ldisc_echo_erase(pty, proc, lflag, pty->canon_buf[prev]);
    }
    return 0;
  }
  if (!literal && (lflag & IEXTEN) && b != 0 && b == t->c_cc[VREPRINT]) {
    if (lflag & ECHO) {
      ldisc_echo_byte(pty, proc, lflag, b);
      ldisc_echo(pty, proc, "\n", 1);
      for (uint32_t pos = pty->canon_commit; pos != pty->canon_head;
           pos = (pos + 1) % PTY_BUF_SIZE)
        ldisc_echo_byte(pty, proc, lflag, pty->canon_buf[pos]);
    }
    return 0;
  }
  if (!literal && (lflag & IEXTEN) && b != 0 && b == t->c_cc[VLNEXT]) {
    pty->lnext_pending = 1;
    if (lflag & ECHO) {
      if (lflag & ECHOCTL)
        ldisc_echo(pty, proc, "^\b", 2);
      else
        ldisc_echo(pty, proc, (const char *)&b, 1);
    }
    return 0;
  }

  // 5. Ordinary canonical byte. A full queue drops the byte and echoes BEL —
  // the terminal's write path must never block on line-discipline state.
  if (pty_ring_space(pty->canon_head, pty->canon_tail) == 0) {
    if (lflag & ECHO)
      ldisc_echo(pty, proc, "\a", 1);
    return 0;
  }
  pty->canon_buf[pty->canon_head] = b;
  pty->canon_head = (pty->canon_head + 1) % PTY_BUF_SIZE;
  if (lflag & ECHO)
    ldisc_echo_byte(pty, proc, lflag, b);
  if (b == '\n') {
    pty->canon_commit = pty->canon_head;
    __wake_up(pty->wq, POLLIN);
  }
  return 0;
}

// ===================== pty_master_write =====================
int64_t pty_master_write(struct pty *pty, xtask *proc, const void *buf,
                         size_t len) {
  // Every byte flows through the line discipline (ICANON/ECHO/ISIG/ICRNL).
  // The len==0 → EOF hack is gone: Ctrl-D arrives as the VEOF byte and is
  // handled by the ldisc.
  size_t written = 0;

  while (written < len) {
    if (ldisc_input(pty, proc, ((const uint8_t *)buf)[written]) == 0) {
      written++;
      continue;
    }

    // Raw-mode input ring full
    if (pty_is_nonblock(proc, pty, 1)) {
      if (written > 0)
        break;
      return -EAGAIN;
    }

    wait_queue_t wait;
    wait.func = pty_wake_cb;
    wait.data = proc;
    wait.exclusive = 0;
    list_init(&wait.node);
    uint64_t wait_flags = pty_prepare_wait(pty, &wait, proc);

    // Close the registration race exactly as the slave-output path does: if
    // the reader freed space just before enrollment, consume the byte now.
    if (ldisc_input(pty, proc, ((const uint8_t *)buf)[written]) == 0) {
      remove_wait_queue(pty->wq, &wait);
      sched_cancel_spurious_wake(proc);
      pty_finish_wait_prepare(wait_flags);
      written++;
      continue;
    }
    if (pty_eintr_check(proc)) {
      remove_wait_queue(pty->wq, &wait);
      sched_cancel_spurious_wake(proc);
      pty_finish_wait_prepare(wait_flags);
      if (written > 0)
        break;
      return -ERESTART;
    }
    schedule();
    remove_wait_queue(pty->wq, &wait);
    pty_finish_wait_prepare(wait_flags);
    if (pty_eintr_check(proc)) {
      if (written > 0)
        break;
      return -ERESTART;
    }
  }

  return (int64_t)written;
}

// ===================== M2-A: foreground access gate =====================
// Returns 0 = allow the slave access, -EIO = fail with EIO, -ERESTART = a
// job-control signal was sent to the caller's pgrp and the syscall should
// restart after the signal is processed (a SIGTTIN/SIGTTOU default-stop leaves
// the caller stopped; on SIGCONT the syscall re-enters and re-evaluates).
//
// Only slave-side access is gated; master I/O is never subject to job control
// (the terminal emulator owns the master). A caller that does not hold this
// pty as its controlling terminal, or whose session does not match t_sid, is
// treated as ordinary I/O — redirected-into-the-pty processes are NOT
// background jobs.
static int pty_fg_gate(struct pty *pty, xtask *t, enum tty_access_op op) {
  proc *p = t->proc;
  if (!p || !p->ctty || p->ctty != pty)
    return 0;
  if (pty->t_sid == 0 || p->sid != pty->t_sid)
    return 0;

  // Snapshot foreground pgid + orphan status under tasks_lock so the gate
  // decision is consistent with a concurrent tcsetpgrp. pgsignal is deferred
  // to after the unlock (tasks_lock → scheduler_lock order; signal delivery
  // takes scheduler_lock in wake paths).
  pid_t fg;
  bool orphaned;
  spin_lock(&tasks_lock);
  fg = pty->t_pgid;
  orphaned =
      (fg != 0 && p->pgid != fg) ? pgrp_is_orphaned(p->pgid, p->sid) : false;
  spin_unlock(&tasks_lock);

  if (fg == 0 || p->pgid == fg)
    return 0; // foreground (or no fg set yet) → allow

  int sig = (op == TTY_READ) ? SIGTTIN : SIGTTOU;
  // Disposition of the *calling* process (Linux uses the caller's action).
  bool ign_blocked =
      (p->sig_blocked & (1ULL << (sig - 1))) ||
      (p->signal->action[sig].__sigaction_handler._sa_handler == SIG_IGN);

  if (op == TTY_READ) {
    // Background read: SIGTTIN; ignored/blocked or orphaned group → EIO.
    if (ign_blocked || orphaned)
      return -EIO;
    pgsignal(p->pgid, SIGTTIN);
    return -ERESTART;
  }

  // Write: only TOSTOP makes background writes signal. ioctl always signals
  // (state-changing ioctls are gated by the caller; reads like TCGETS never
  // reach here).
  if (op == TTY_WRITE && !(pty->t_termios.c_lflag & TOSTOP))
    return 0;

  // TOSTOP write, or state-changing ioctl: SIGTTOU. An ignored/blocked
  // SIGTTOU permits the operation even for an orphaned group; job-control
  // shells rely on this exception to reclaim the terminal with tcsetpgrp().
  if (ign_blocked)
    return 0;
  if (orphaned)
    return -EIO;
  pgsignal(p->pgid, SIGTTOU);
  return -ERESTART;
}

// ===================== pty_slave_read =====================
int64_t pty_slave_read(struct pty *pty, xtask *proc, void *buf, size_t len) {
  // M2-A: foreground access gate. Must run before entering the wait queue
  // and re-run after every wake (a SIGTTIN that stops the caller may be
  // followed by a tcsetpgrp making it foreground before SIGCONT resumes).
  int gate = pty_fg_gate(pty, proc, TTY_READ);
  if (gate)
    return (int64_t)gate;

  if (pty->master_refs == 0)
    return -EPIPE;

  int canon = (pty->t_termios.c_lflag & ICANON) != 0;

  // One-shot EOF (ldisc VEOF on an empty line)
  if (pty->eof_pending) {
    pty->eof_pending = 0;
    return 0;
  }

  wait_queue_t wait;
  wait.func = pty_wake_cb;
  wait.data = proc;
  wait.exclusive = 0;
  list_init(&wait.node);
  add_wait_queue(pty->wq, &wait);
  for (;;) {
    uint64_t wait_flags = pty_reprepare_wait(proc);
    // Re-evaluate the foreground gate after a wake: terminal ownership may
    // have changed while we were blocked.
    gate = pty_fg_gate(pty, proc, TTY_READ);
    if (gate) {
      remove_wait_queue(pty->wq, &wait);
      sched_cancel_spurious_wake(proc);
      pty_finish_wait_prepare(wait_flags);
      return (int64_t)gate;
    }
    int avail = canon ? ldisc_canon_avail(pty)
                      : pty_ring_avail(pty->m_to_s_head, pty->m_to_s_tail);
    if (avail > 0) {
      remove_wait_queue(pty->wq, &wait);
      sched_cancel_spurious_wake(proc);
      pty_finish_wait_prepare(wait_flags);
      break;
    }
    if (pty->eof_pending) { // VEOF arrived while blocked
      pty->eof_pending = 0;
      remove_wait_queue(pty->wq, &wait);
      sched_cancel_spurious_wake(proc);
      pty_finish_wait_prepare(wait_flags);
      return 0;
    }
    if (pty->master_refs == 0) {
      remove_wait_queue(pty->wq, &wait);
      sched_cancel_spurious_wake(proc);
      pty_finish_wait_prepare(wait_flags);
      return 0; // EOF
    }
    if (pty_is_nonblock(proc, pty, 0)) {
      remove_wait_queue(pty->wq, &wait);
      sched_cancel_spurious_wake(proc);
      pty_finish_wait_prepare(wait_flags);
      return -EAGAIN;
    }

    if (pty_eintr_check(proc)) {
      remove_wait_queue(pty->wq, &wait);
      sched_cancel_spurious_wake(proc);
      pty_finish_wait_prepare(wait_flags);
      return -ERESTART;
    }
    schedule();
    pty_finish_wait_prepare(wait_flags);
    if (pty_eintr_check(proc)) {
      remove_wait_queue(pty->wq, &wait);
      sched_cancel_spurious_wake(proc);
      return -ERESTART;
    }
  }
  int nread = canon
                  ? pty_ring_read(pty->canon_buf, pty->canon_commit,
                                  &pty->canon_tail, (uint8_t *)buf, (int)len)
                  : pty_ring_read(pty->m_to_s_buf, pty->m_to_s_head,
                                  &pty->m_to_s_tail, (uint8_t *)buf, (int)len);
  __wake_up(pty->wq, POLLOUT);
  return (int64_t)nread;
}

// ===================== pty_slave_write (with OPOST+ONLCR)
// =====================
int64_t pty_slave_write(struct pty *pty, xtask *proc, const void *buf,
                        size_t len) {
  // M2-A: foreground access gate (TOSTOP only). Must run before writing the
  // first byte and re-run after a blocking wake.
  int gate = pty_fg_gate(pty, proc, TTY_WRITE);
  if (gate)
    return (int64_t)gate;

  if (pty->master_refs == 0)
    return -EPIPE;

  int do_opost =
      (pty->t_termios.c_oflag & OPOST) && (pty->t_termios.c_oflag & ONLCR);
  size_t written = 0;
  char serial_buf[256];
  size_t serial_len = 0;

  while (written < len) {
    // Re-evaluate gate after a wake: foreground may have changed.
    gate = pty_fg_gate(pty, proc, TTY_WRITE);
    if (gate) {
      if (written > 0)
        break;
      return (int64_t)gate;
    }
    const uint8_t ch = ((const uint8_t *)buf)[written];
    uint8_t output[2] = {ch, 0};
    size_t output_len = 1;
    if (do_opost && ch == '\n') {
      output[0] = '\r';
      output[1] = '\n';
      output_len = 2;
    }
    int wrote = 0;

    // Transform once, then feed the exact same post-OPOST bytes to the PTY
    // master and the serial console mirror.
    spin_lock(&pty->s_to_m_lock);
    if (pty_ring_space(pty->s_to_m_head, pty->s_to_m_tail) >= (int)output_len) {
      for (size_t i = 0; i < output_len; i++)
        pty_ring_write1(pty->s_to_m_buf, &pty->s_to_m_head, pty->s_to_m_tail,
                        output[i]);
      wrote = 1;
    }
    spin_unlock(&pty->s_to_m_lock);

    if (wrote) {
      for (size_t i = 0; i < output_len; i++) {
        if (serial_len == sizeof(serial_buf)) {
          serial_write(serial_buf, serial_len);
          serial_len = 0;
        }
        serial_buf[serial_len++] = (char)output[i];
      }
      written++;
      __wake_up(pty->wq, POLLIN);
      continue;
    }

    // No space
    if (pty_is_nonblock(proc, pty, 0)) {
      if (written > 0)
        break;
      return -EAGAIN;
    }

    wait_queue_t wait;
    wait.func = pty_wake_cb;
    wait.data = proc;
    wait.exclusive = 0;
    list_init(&wait.node);
    uint64_t wait_flags = pty_prepare_wait(pty, &wait, proc);

    // The master may have drained the ring after the failed write but before
    // this waiter was visible. Re-check after publishing BLOCKED so either the
    // condition is observed here or a later drain wakes us.
    spin_lock(&pty->s_to_m_lock);
    int space_ready =
        pty_ring_space(pty->s_to_m_head, pty->s_to_m_tail) >= (int)output_len;
    spin_unlock(&pty->s_to_m_lock);
    if (space_ready) {
      remove_wait_queue(pty->wq, &wait);
      sched_cancel_spurious_wake(proc);
      pty_finish_wait_prepare(wait_flags);
      continue;
    }
    if (pty_eintr_check(proc)) {
      remove_wait_queue(pty->wq, &wait);
      sched_cancel_spurious_wake(proc);
      pty_finish_wait_prepare(wait_flags);
      if (written > 0)
        break;
      return -ERESTART;
    }
    schedule();
    remove_wait_queue(pty->wq, &wait);
    pty_finish_wait_prepare(wait_flags);
    if (pty_eintr_check(proc)) {
      if (written > 0)
        break;
      return -ERESTART;
    }
  }

  if (serial_len != 0)
    serial_write(serial_buf, serial_len);
  return (int64_t)written;
}

// ===================== pty_ioctl =====================
long pty_ioctl(struct pty *pty, uint32_t cmd, void *arg) {
  switch (cmd) {
  case TCGETS: // 0x5401
    if (copy_to_user((void __user *)arg, &pty->t_termios,
                     sizeof(struct termios)))
      return -EFAULT;
    return 0;

  case TCSETS:  // 0x5402
  case TCSETSW: // 0x5403 — no output drain (kept identical to TCSETS)
  {
    // M2-A: background state-changing ioctl → SIGTTOU. Foreground (or a
    // process not holding this pty as ctty) is allowed through.
    int gate = pty_fg_gate(pty, current_task, TTY_IOCTL);
    if (gate)
      return gate;
    struct termios nt;
    if (copy_from_user(&nt, (const void __user *)arg, sizeof(struct termios)))
      return -EFAULT;
    // Leaving ICANON: the uncommitted edit line has no canonical home, so it
    // is dropped (committed bytes stay readable). Entering ICANON from raw:
    // raw bytes already in the master->slave ring remain readable via the
    // ring path (they bypass the canonical queue). Matches Linux's
    // "pending unread data is flushed on mode switch" approximation.
    if ((pty->t_termios.c_lflag & ICANON) && !(nt.c_lflag & ICANON))
      pty->canon_head = pty->canon_commit = pty->canon_tail;
    pty->t_termios = nt;
    return 0;
  }

  case TCSETSF: // 0x5404 — set termios + flush unread input
  {
    int gate = pty_fg_gate(pty, current_task, TTY_IOCTL);
    if (gate)
      return gate;
    struct termios nt;
    if (copy_from_user(&nt, (const void __user *)arg, sizeof(struct termios)))
      return -EFAULT;
    ldisc_flush_input(pty);
    pty->t_termios = nt;
    return 0;
  }

  case TIOCGPGRP: // 0x540F
  {
    // M2-A: fd must be the caller's controlling terminal.
    if (!current_proc->ctty || current_proc->ctty != pty || pty->t_sid == 0 ||
        pty->t_sid != current_proc->sid)
      return -ENOTTY;
    pid_t pgid;
    spin_lock(&tasks_lock);
    pgid = pty->t_pgid;
    spin_unlock(&tasks_lock);
    if (copy_to_user((void __user *)arg, &pgid, sizeof(pid_t)))
      return -EFAULT;
    return 0;
  }

  case TIOCSPGRP: // 0x5410
  {
    // M2-A: fd must be the caller's controlling terminal; the target group
    // must exist in the caller's session. Background caller → SIGTTOU. The
    // old "pgid == sid direct pass" bypass is removed — even the session
    // leader must name a real group.
    if (!current_proc->ctty || current_proc->ctty != pty || pty->t_sid == 0 ||
        pty->t_sid != current_proc->sid)
      return -ENOTTY;
    int gate = pty_fg_gate(pty, current_task, TTY_IOCTL);
    if (gate)
      return gate;
    pid_t pgid;
    if (copy_from_user(&pgid, (const void __user *)arg, sizeof(pid_t)))
      return -EFAULT;
    if (pgid <= 0)
      return -EINVAL;

    spin_lock(&tasks_lock);
    bool found = false;
    if (pgid == current_proc->pgid) {
      found = true; // joining/confirming own group as foreground
    } else {
      for (int p = 0; p < MAX_PROC; p++) {
        xtask *t = tasks[p];
        if (t && t->pid == p && t->proc && t->proc->pgid == pgid &&
            t->proc->sid == current_proc->sid) {
          found = true;
          break;
        }
      }
    }
    if (!found) {
      spin_unlock(&tasks_lock);
      return -EPERM;
    }
    pty->t_pgid = pgid;
    spin_unlock(&tasks_lock);
    return 0;
  }

  case TIOCGWINSZ: // 0x5413
    if (copy_to_user((void __user *)arg, &pty->t_winsize,
                     sizeof(struct winsize)))
      return -EFAULT;
    return 0;

  case TIOCSWINSZ: // 0x5414
  {
    int gate = pty_fg_gate(pty, current_task, TTY_IOCTL);
    if (gate)
      return gate;
    struct winsize old_ws = pty->t_winsize;
    if (copy_from_user(&pty->t_winsize, (const void __user *)arg,
                       sizeof(struct winsize)))
      return -EFAULT;
    // If size changed and session exists, send SIGWINCH to foreground pgid.
    // pgid is system-unique, so pgsignal's pgid-only match implies the
    // session; no separate t_sid filter is needed.
    if ((old_ws.ws_row != pty->t_winsize.ws_row ||
         old_ws.ws_col != pty->t_winsize.ws_col) &&
        pty->t_sid != 0)
      pgsignal(pty->t_pgid, SIGWINCH);
    return 0;
  }

  case TIOCGPTN: // 0x80045430 — get PTY index
  {
    int idx = pty->index;
    if (copy_to_user((void __user *)arg, &idx, sizeof(int)))
      return -EFAULT;
    return 0;
  }

  case TIOCSPTLCK: // 0x40045431 — lock/unlock slave (stub)
    return 0;

  case TIOCSCTTY: // 0x540E — set controlling terminal
  {
    // M2-A: caller must be a session leader. Idempotent if already holding
    // this tty. A tty owned by another session may only be stolen with
    // force+privilege (CAP_SYS_ADMIN); the old session's ctty refs are then
    // cleared. The force arg is not an unconditional license.
    if (current_proc->sid != current_task->pid)
      return -EPERM;
    int force = (int)(uintptr_t)arg;

    spin_lock(&tasks_lock);
    if (current_proc->ctty == pty) {
      // Idempotent: already controlling this tty. Refresh fg to caller's
      // group (matches the first-acquisition path).
      pty->t_sid = current_proc->sid;
      pty->t_pgid = current_proc->pgid;
      spin_unlock(&tasks_lock);
      return 0;
    }
    // Tty already owned by a different session?
    if (pty->t_sid != 0 && pty->t_sid != current_proc->sid) {
      if (!(force && capable(CAP_SYS_ADMIN))) {
        spin_unlock(&tasks_lock);
        return -EPERM;
      }
      // Steal: clear the old session members' ctty refs + ownership.
      pid_t old_sid = pty->t_sid;
      for (int p = 0; p < MAX_PROC; p++) {
        xtask *t = tasks[p];
        if (t && t->pid >= 0 && t->proc && t->proc->sid == old_sid &&
            t->proc->ctty == pty)
          t->proc->ctty = NULL;
      }
      pty->t_sid = 0;
      pty->t_pgid = 0;
    } else if (current_proc->ctty && current_proc->ctty != pty) {
      // Caller already controls a different tty: refuse unless force.
      if (!force) {
        spin_unlock(&tasks_lock);
        return -EINVAL;
      }
    }
    current_proc->ctty = pty;
    pty->t_sid = current_proc->sid;
    pty->t_pgid = current_proc->pgid;
    spin_unlock(&tasks_lock);
    return 0;
  }

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
      // Data source mirrors pty_slave_read: canonical queue in ICANON mode,
      // raw ring otherwise.
      int avail = (pty->t_termios.c_lflag & ICANON)
                      ? pty_ring_avail(pty->canon_commit, pty->canon_tail)
                      : pty_ring_avail(pty->m_to_s_head, pty->m_to_s_tail);
      if (avail > 0)
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
