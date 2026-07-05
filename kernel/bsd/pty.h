/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef KERNEL_PTY_H
#define KERNEL_PTY_H

#include "kernel/bsd/devtmpfs.h"
#include "kernel/bsd/types.h"
#include "kernel/xcore/spinlock.h"
#include "kernel/xcore/trap.h" // wake_process
#include "kernel/xcore/xtask.h"
#include <stdint.h>
#include <xos/fcntl.h>

// ===================== PTY buffer size =====================
#define PTY_BUF_SIZE 4096

// ===================== struct termios (Linux x86-64 ABI) =====================
#define NCCS 19

typedef unsigned long tcflag_t;
typedef unsigned char cc_t;

struct termios {
  tcflag_t c_iflag; // input flags
  tcflag_t c_oflag; // output flags
  tcflag_t c_cflag; // control flags
  tcflag_t c_lflag; // local flags
  cc_t c_cc[NCCS];  // special characters
};

// c_cc indices (Linux x86-64)
#define VINTR 0
#define VQUIT 1
#define VERASE 2
#define VKILL 3
#define VEOF 4
#define VTIME 5
#define VMIN 6
#define VSWTC 7
#define VSTART 8
#define VSTOP 9
#define VSUSP 10
#define VEOL 11
#define VREPRINT 12
#define VDISCARD 13
#define VWERASE 14
#define VLNEXT 15
#define VEOL2 16

// Input flags
#define IGNBRK 0x0001
#define BRKINT 0x0002
#define IGNPAR 0x0004
#define PARMRK 0x0008
#define INPCK 0x0010
#define ISTRIP 0x0020
#define INLCR 0x0040
#define IGNCR 0x0080
#define ICRNL 0x0100
#define IXON 0x0400
#define IXOFF 0x1000

// Output flags
#define OPOST 0x0001
#define ONLCR 0x0004

// Control flags
#define CS8 0x0060
#define CLOCAL 0x0800

// Local flags
#define ISIG 0x0001
#define ICANON 0x0002
#define ECHO 0x0008
#define ECHOE 0x0010
#define ECHOK 0x0020
#define NOFLSH 0x0080
#define TOSTOP 0x0100
#define IEXTEN 0x0200

// Default termios
extern const struct termios default_termios;

// ===================== ioctl commands (Linux x86-64) =====================
#define TCGETS 0x5401
#define TCSETS 0x5402
#define TCSETSW 0x5403
#define TCSETSF 0x5404
#define TIOCGPTN 0x5406
#define TIOCSPTLCK 0x5407
#define TIOCSCTTY 0x540E
#define TIOCGPGRP 0x540F
#define TIOCSPGRP 0x5410
#define TIOCGWINSZ 0x5413
#define TIOCSWINSZ 0x5414

// ===================== struct winsize (Linux ABI) =====================
struct winsize {
  unsigned short ws_row;
  unsigned short ws_col;
  unsigned short ws_xpixel; // unused, set to 0
  unsigned short ws_ypixel; // unused, set to 0
};

// ===================== struct pts_dev_priv =====================
// Wrapper around dev_ops so pts_open can find the pty via container_of
struct pts_dev_priv {
  struct dev_ops ops; // must be first for cast compatibility
  struct pty *pty;
};

// ===================== struct pty =====================
#define MAX_PTY 16

struct pty {
  // Bidirectional ring buffers
  uint8_t m_to_s_buf[PTY_BUF_SIZE]; // master->slave (keyboard input)
  uint32_t m_to_s_head, m_to_s_tail;
  uint8_t s_to_m_buf[PTY_BUF_SIZE]; // slave->master (shell output)
  uint32_t s_to_m_head, s_to_m_tail;

  // Blocking wait PIDs (-1 = no waiter)
  pid_t m_read_pid;       // master read blocked
  pid_t m_write_pid;      // master write blocked
  pid_t s_read_pid;       // slave read blocked
  pid_t s_write_pid;      // slave write blocked
  pid_t master_owner_pid; // process holding master fd (for wakeup)

  // termios (kernel stored, user reads via TCGETS)
  struct termios t_termios;

  // Window size
  struct winsize t_winsize;

  // PTY index and reference counts
  int index;        // PTY number (corresponds to /dev/ptsN)
  int master_refs;  // master fd reference count
  int slave_refs;   // slave fd reference count
  int slave_opened; // has the slave side been opened?

  // Session / foreground pgid (reserved for future job control)
  pid_t t_sid;     // session ID (0 = no session)
  pid_t t_pgid;    // foreground process group ID
  int eof_pending; // Ctrl-D EOF flag: master write len=0 → slave read returns 0

  // Pointer to slave device priv (for cleanup)
  struct pts_dev_priv *pts_priv;
};

// ===================== Global PTY table =====================
extern struct pty *pty_table[MAX_PTY];
extern spinlock pty_alloc_lock;

// ===================== PTY functions =====================
void pty_init(void);
struct pty *pty_alloc(int *out_index);
void pty_free(struct pty *pty);

// Ring buffer helpers
int pty_ring_write(uint8_t *buf, uint32_t *head, uint32_t tail,
                   const uint8_t *data, int len);
int pty_ring_read(uint8_t *buf, uint32_t head, uint32_t *tail, uint8_t *data,
                  int len);
int pty_ring_avail(uint32_t head, uint32_t tail);
int pty_ring_space(uint32_t head, uint32_t tail);

// /dev/ptmx open callback
int ptmx_open(xtask *proc, int fd);

// /dev/ptsN slave open callback
int pts_open(xtask *proc, int fd);

// PTY read/write
int64_t pty_master_read(struct pty *pty, xtask *proc, void *buf, size_t len);
int64_t pty_master_write(struct pty *pty, xtask *proc, const void *buf,
                         size_t len);
int64_t pty_slave_read(struct pty *pty, xtask *proc, void *buf, size_t len);
int64_t pty_slave_write(struct pty *pty, xtask *proc, const void *buf,
                        size_t len);

// PTY ioctl
long pty_ioctl(struct pty *pty, uint32_t cmd, void *arg);

// PTY poll
uint32_t pty_poll(struct pty *pty, int is_master, uint32_t events);

// Determine if fd is master or slave side
int pty_fd_is_master(files *files, int fd);

// Check if inode is the ptmx master inode (for pty_close_file/pty_dup_file)
int pty_is_master_inode(struct inode *inode);

// Wake blocked pipe peers based on fd direction flags
static inline void wake_pipe_peers(pipe *p, int fd_flags) {
  if (fd_flags & (O_WRONLY | O_RDWR)) {
    if (p->read_pid >= 0)
      wake_process(p->read_pid);
  }
  if (fd_flags & (O_RDONLY | O_RDWR)) {
    if (p->write_pid >= 0)
      wake_process(p->write_pid);
  }
}

#endif // KERNEL_PTY_H
