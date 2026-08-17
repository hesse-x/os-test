/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */
#include "kernel/bsd/procfs.h"

#include <stddef.h>

#include "arch/x64/apic.h" // sched_clock
#include "arch/x64/utils.h"
#include "kernel/bsd/devtmpfs.h" // devtmpfs_name_by_inode (FD_DEV fd-N readlink)
#include "kernel/bsd/fops.h"
#include "kernel/bsd/inode.h"
#include "kernel/bsd/mount.h"
#include "kernel/bsd/proc.h"
#include "kernel/bsd/pty.h"    // struct pty (f->pty->index, fd-N readlink)
#include "kernel/bsd/signal.h" // signal_struct (parent_pid, thread_count)
#include "kernel/bsd/types.h"
#include "kernel/kernel.h" // KERNEL_VERSION
#include "kernel/xcore/atomic.h"
#include "kernel/xcore/kpi.h"
#include "kernel/xcore/log.h"
#include "kernel/xcore/mem/alloc.h" // kernel_mem_stats, total_page_frames, bfc_free_page_nums
#include "kernel/xcore/mem/slab.h" // kernel_mem_stats
#include "kernel/xcore/mm_types.h" // mm, mmap_region (maps_show)
#include "kernel/xcore/spinlock.h"
#include "kernel/xcore/xtask.h" // tasks[], tasks_lock, MAX_PROC

#include <kernel/bsd/stat_abi.h>
#include <xos/dirent.h>
#include <xos/errno.h>
#include <xos/mman.h> // PROT_READ/WRITE/EXEC (maps_show)
#include <xos/syscall.h>

// ===== global state (modeled on sysfs.c:24-26) =====
static struct procfs_node *procfs_root;
static spinlock procfs_lock = SPINLOCK_INIT;  // protects the static tree
static uint32_t procfs_ino_counter = 0x20000; // static global node ino range

// ===== ino decode constants (procfs.md §3.6) =====
#define PROCFS_PID_BASE 0x21000u
#define PROCFS_PID_STRIDE 2048u
#define PROCFS_FD_BASE 0x300000u

// ===== pinfo side table (procfs.md §3.5; implemented in M5) =====
// exe/cmdline are not stored in struct proc (STATIC_ASSERT + driver mirror
// lock, §3.5); a procfs-owned per-pid side table is used instead. Strings carry
// their own refcount, decoupled from the tasks[]/proc_reap lock context:
// proc_reap is called without holding locks (tasks[pid] is still non-NULL,
// state is still ZOMBIE), and tasks_lock cannot prevent clear, so pinfo has its
// own lock + string refcounts — the writer atomically swaps the pointer and
// releases the lock; the old string is freed by the last reader via
// refcount_dec_and_test. A reader takes the pointer under the lock,
// refcount_inc, then reads after unlocking.
struct procfs_str {
  refcount_t rc;
  size_t len; // excludes the trailing NUL
  char data[];
};

static struct procfs_str *procfs_str_new(const char *s, size_t len) {
  struct procfs_str *p = (struct procfs_str *)kmalloc(sizeof(*p) + len + 1);
  if (!p)
    return NULL;
  refcount_set(&p->rc, 1);
  p->len = len;
  if (len)
    __memcpy(p->data, s, len);
  p->data[len] = '\0';
  return p;
}

// Drop one string reference: free when it reaches zero (called on reader exit).
static void procfs_str_put(struct procfs_str *p) {
  if (p && refcount_dec_and_test(&p->rc))
    kfree(p);
}

struct procfs_pinfo {
  refcount_t rc; // struct refcount: clear cannot kfree while a reader holds a
                 // reference (§3.5)
  spinlock lock;
  struct procfs_str *exe;
  struct procfs_str *cmdline;
};
static struct procfs_pinfo *pinfo_table[MAX_PROC]; // slots protected by
                                                   // tasks_lock

// Get the pinfo slot for pid (kmalloc if absent). Caller does not hold
// pinfo->lock.
static struct procfs_pinfo *procfs_pinfo_get(pid_t pid) {
  if (pid < 0 || pid >= MAX_PROC)
    return NULL;
  spin_lock(&tasks_lock);
  struct procfs_pinfo *pi = pinfo_table[pid];
  if (!pi) {
    pi = (struct procfs_pinfo *)kmalloc(sizeof(*pi));
    if (pi) {
      __memset(pi, 0, sizeof(*pi));
      refcount_set(&pi->rc, 1);
      pinfo_table[pid] = pi;
    }
  }
  spin_unlock(&tasks_lock);
  return pi;
}

// Atomically swap one string slot under pinfo->lock, returning the old string
// (caller puts it outside the lock).
static struct procfs_str *procfs_pinfo_swap(struct procfs_pinfo *pi,
                                            struct procfs_str **slot,
                                            struct procfs_str *nw) {
  spin_lock(&pi->lock);
  struct procfs_str *old = *slot;
  *slot = nw;
  spin_unlock(&pi->lock);
  return old;
}

// procfs_pinfo_set: execve/proc_create captures exe=argv[0], cmdline=argv
// joined by \0 delimiters. envp is not stored in this phase (§3.5, environ
// left as TODO). argv/envp may be NULL (init passes exe alone).
void procfs_pinfo_set(pid_t pid, const char *exe, char *const argv[],
                      char *const envp[]) {
  (void)envp; // environ left as TODO
  struct procfs_pinfo *pi = procfs_pinfo_get(pid);
  if (!pi)
    return;
  // exe: prefer argv[0] (Linux convention), else the exe parameter.
  const char *exe_str = (argv && argv[0]) ? argv[0] : exe;
  if (exe_str) {
    size_t el = __strlen(exe_str);
    struct procfs_str *nw = procfs_str_new(exe_str, el);
    if (nw)
      procfs_str_put(procfs_pinfo_swap(pi, &pi->exe, nw));
  }
  // cmdline: argv entries joined with \0 (trailing \0 too, matching Linux
  // /proc/[pid]/cmdline).
  if (argv) {
    size_t total = 0;
    for (int i = 0; argv[i]; i++)
      total += __strlen(argv[i]) + 1;
    struct procfs_str *nw =
        (struct procfs_str *)kmalloc(sizeof(*nw) + total + 1);
    if (nw) {
      refcount_set(&nw->rc, 1);
      nw->len = total;
      size_t off = 0;
      for (int i = 0; argv[i]; i++) {
        size_t l = __strlen(argv[i]);
        __memcpy(nw->data + off, argv[i], l);
        nw->data[off + l] = '\0';
        off += l + 1;
      }
      nw->data[total] = '\0';
      procfs_str_put(procfs_pinfo_swap(pi, &pi->cmdline, nw));
    }
  }
}

// procfs_pinfo_clear: reap cleanup. Remove the table slot + put the strings (a
// reader holding a reference defers the free) + put the struct reference (the
// table slot holds one; each reader adds one more, so with no readers this
// reaches 0 here and kfrees).
void procfs_pinfo_clear(pid_t pid) {
  if (pid < 0 || pid >= MAX_PROC)
    return;
  spin_lock(&tasks_lock);
  struct procfs_pinfo *pi = pinfo_table[pid];
  pinfo_table[pid] = NULL;
  spin_unlock(&tasks_lock);
  if (!pi)
    return;
  // Drop the string references first: the swapped-out old strings are given to
  // readers to put (or freed here if none). Safe without holding pi->lock —
  // after clear nobody swaps in again (set goes to a new slot by pid).
  procfs_str_put(procfs_pinfo_swap(pi, &pi->exe, NULL));
  procfs_str_put(procfs_pinfo_swap(pi, &pi->cmdline, NULL));
  if (refcount_dec_and_test(&pi->rc))
    kfree(pi); // no readers → free immediately
}

// Reader helper: take the struct reference (+rc) under tasks_lock, then the
// string reference (+rc) under pinfo->lock, and release the struct reference;
// returns the string reference (caller uses procfs_str_put when done). Returns
// NULL if the process is dead / slot empty / field empty. The struct refcount
// guarantees pi is not kfree'd during clear (§3.5).
static struct procfs_str *procfs_pinfo_ref(pid_t pid, int is_cmdline) {
  if (pid < 0 || pid >= MAX_PROC)
    return NULL;
  spin_lock(&tasks_lock);
  struct procfs_pinfo *pi = pinfo_table[pid];
  if (pi)
    refcount_inc(&pi->rc);
  spin_unlock(&tasks_lock);
  if (!pi)
    return NULL;
  spin_lock(&pi->lock);
  struct procfs_str *s = is_cmdline ? pi->cmdline : pi->exe;
  if (s)
    refcount_inc(&s->rc);
  spin_unlock(&pi->lock);
  if (refcount_dec_and_test(&pi->rc))
    kfree(pi); // clear already removed the slot and no other readers → free
  return s;
}

// ===== node allocation (modeled on sysfs.c node_alloc) =====
static struct procfs_node *pnode_alloc(const char *name, bool is_dir,
                                       enum procfs_node_kind kind) {
  struct procfs_node *n = kmalloc(sizeof(struct procfs_node));
  if (!n)
    return NULL;
  __memset(n, 0, sizeof(*n));
  __strncpy(n->name, name, 31);
  n->name[31] = '\0';
  n->is_dir = is_dir;
  n->kind = kind;
  n->ino = procfs_ino_counter++;
  return n;
}

static struct procfs_node *pnode_add(struct procfs_node *parent,
                                     const char *name, bool is_dir,
                                     enum procfs_node_kind kind,
                                     struct procfs_attr *attr) {
  spin_lock(&procfs_lock);
  for (struct procfs_node *c = parent->children; c; c = c->sibling) {
    if (__strcmp(c->name, name) == 0) {
      spin_unlock(&procfs_lock);
      return c;
    }
  }
  struct procfs_node *n = pnode_alloc(name, is_dir, kind);
  if (!n) {
    spin_unlock(&procfs_lock);
    return NULL;
  }
  n->attr = attr;
  n->parent = parent;
  n->sibling = parent->children;
  parent->children = n;
  spin_unlock(&procfs_lock);
  return n;
}

// ===== pid attribute attr_index (procfs.md §3.2.3 / §3.6) =====
// attr_index starts at 1: the pid directory occupies attr_index==0
// (PID_BASE+pid*2048), and no file attr may collide with it — otherwise
// inode_get_or_create would reuse the same inode and the pid directory would be
// read as a status file (i_priv semantic confusion). procfs.md §3.6 omitted
// this constraint; corrected here.
enum {
  ATTR_PIDDIR = 0,
  ATTR_STATUS = 1,
  ATTR_STAT,
  ATTR_COMM,
  ATTR_CMDLINE,
  ATTR_MAPS,
  ATTR_CWD,
  ATTR_EXE,
  ATTR_FD,
  ATTR_PID_MAX
};

static ssize_t status_show(char *buf, size_t len, pid_t pid);
static ssize_t stat_show(char *buf, size_t len, pid_t pid);
static ssize_t comm_show(char *buf, size_t len, pid_t pid);
static ssize_t cmdline_show(char *buf, size_t len, pid_t pid);
static ssize_t maps_show(char *buf, size_t len, pid_t pid);

// pid_attrs is indexed by the ATTR_* enum values: index 0 is reserved (the pid
// directory occupies attr_index==0), ATTR_STATUS=1..ATTR_FD=8 map to the real
// attrs. lookup uses the array index i directly as attr_index (i.e. the enum
// value), and readlink decodes (ino-PID_BASE)%STRIDE to recover the attr and
// match ATTR_CWD/ATTR_EXE — so the index must equal the enum value and must not
// be shifted.
static const struct procfs_attr pid_attrs[] = {
    {NULL, NULL},              // [ATTR_PIDDIR=0] placeholder: pid dir, no attr
    {"status", status_show},   // ATTR_STATUS=1
    {"stat", stat_show},       // ATTR_STAT=2
    {"comm", comm_show},       // ATTR_COMM=3
    {"cmdline", cmdline_show}, // ATTR_CMDLINE=4
    {"maps", maps_show},       // ATTR_MAPS=5
    {"cwd", NULL},             // ATTR_CWD=6, lnk, M4 readlink
    {"exe", NULL},             // ATTR_EXE=7, lnk, M5 readlink
    {"fd", NULL},              // ATTR_FD=8, dir, M4
    {NULL, NULL}};

// ===== global static show callbacks (procfs.md §3.3 table) =====
static ssize_t meminfo_show(char *buf, size_t len, pid_t pid) {
  (void)pid;
  // kernel_mem_stats has 6 fields (slab.c:24): total_pages/used_pages/
  // slab_used_bytes/slab_peak_bytes/kmalloc_calls/kfree_calls;
  // total_page_frames (alloc.h:76); bfc_free_page_nums() (alloc.h:64)
  int total = memstat_read(&kernel_mem_stats.total_pages);
  int used = memstat_read(&kernel_mem_stats.used_pages);
  int slab = memstat_read(&kernel_mem_stats.slab_used_bytes);
  size_t free_pages = (size_t)(total - used) + bfc_free_page_nums();
  return snprintf(buf, len,
                  "MemTotal:      %lu kB\n"
                  "MemFree:       %lu kB\n"
                  "MemAvailable:  %lu kB\n"
                  "Slab:          %lu kB\n",
                  (unsigned long)total * 4, (unsigned long)free_pages * 4,
                  (unsigned long)free_pages * 4, (unsigned long)(slab / 1024));
}

static ssize_t uptime_show(char *buf, size_t len, pid_t pid) {
  (void)pid;
  uint64_t ns = sched_clock(); // apic.c:30
  return snprintf(buf, len, "%llu.%02llu 0.00\n",
                  (unsigned long long)(ns / 1000000000ULL),
                  (unsigned long long)((ns / 10000000ULL) % 100));
}

static ssize_t version_show(char *buf, size_t len, pid_t pid) {
  (void)pid;
  return snprintf(buf, len, "%s\n", KERNEL_VERSION);
}

// cpuinfo (procfs.md §3.3 table / M6 Step 42): vendor(cpuid 0) + brand string
// (cpuid 0x80000002-4) + cpu MHz (from tsc_freq) + bogomips placeholder. One
// section per core, matching Linux.
static void cpuid_leaf(uint32_t leaf, uint32_t *a, uint32_t *b, uint32_t *c,
                       uint32_t *d) {
  __asm__ volatile("cpuid"
                   : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
                   : "a"(leaf));
}

static ssize_t cpuinfo_show(char *buf, size_t len, pid_t pid) {
  (void)pid;
  extern int ncpu;          // smp.c:24
  extern uint64_t tsc_freq; // apic.h:121, TSC ticks/sec
  // vendor (cpuid 0): ebx:edx:ecx → 12-char string.
  uint32_t a, b, c, d;
  cpuid_leaf(0, &a, &b, &c, &d);
  char vendor[13];
  *(uint32_t *)(vendor + 0) = b;
  *(uint32_t *)(vendor + 4) = d;
  *(uint32_t *)(vendor + 8) = c;
  vendor[12] = '\0';
  // brand string (cpuid 0x80000002-4): 3 leaves × 16 bytes = 48 chars, trailing
  // NUL.
  char brand[49];
  brand[0] = '\0';
  if (a >= 0x80000004u) {
    uint32_t *p = (uint32_t *)brand;
    uint32_t ea, eb, ec, ed;
    cpuid_leaf(0x80000002u, &ea, &eb, &ec, &ed);
    p[0] = ea;
    p[1] = eb;
    p[2] = ec;
    p[3] = ed;
    cpuid_leaf(0x80000003u, &ea, &eb, &ec, &ed);
    p[4] = ea;
    p[5] = eb;
    p[6] = ec;
    p[7] = ed;
    cpuid_leaf(0x80000004u, &ea, &eb, &ec, &ed);
    p[8] = ea;
    p[9] = eb;
    p[10] = ec;
    p[11] = ed;
    brand[48] = '\0';
  }
  unsigned cpu_mhz = tsc_freq ? (unsigned)(tsc_freq / 1000000ULL) : 0;
  int n = 0;
  for (int i = 0; i < ncpu; i++) {
    n += snprintf(buf + n, (n < (int)len ? len - (size_t)n : 0),
                  "processor\t: %d\n"
                  "vendor_id\t: %s\n"
                  "model name\t: %s\n"
                  "cpu MHz\t\t: %u.%03u\n"
                  "bogomips\t: %u.%02u\n\n",
                  i, vendor, brand[0] ? brand : "unknown", cpu_mhz,
                  (unsigned)((tsc_freq / 1000ULL) % 1000000u) / 1000u, cpu_mhz,
                  (unsigned)(cpu_mhz / 100u) % 100u);
  }
  return n;
}

// ===== per-pid show callbacks (procfs.md §3.3 table / §3.3.1 / §3.3.2) =====
static ssize_t status_show(char *buf, size_t len, pid_t pid) {
  if (pid < 0 || pid >= MAX_PROC)
    return -ENOENT;
  spin_lock(&tasks_lock);
  xtask *t = tasks[pid];
  if (!t || t->state == ZOMBIE || t->state == REAPING || !t->proc) {
    spin_unlock(&tasks_lock);
    return -ENOENT;
  }
  proc *bp = t->proc;
  const char *st = t->state == RUNNING   ? "R"
                   : t->state == ZOMBIE  ? "Z"
                   : t->state == STOPPED ? "T"
                                         : "S";
  int n = snprintf(buf, len,
                   "Name:\t%s\nState:\t%s\nUid:\t%u\nGid:\t%u\nPPid:\t%d\nSid:"
                   "\t%d\nPgid:\t%d\n",
                   t->comm, st, bp->uid, bp->gid, (int)bp->signal->parent_pid,
                   (int)bp->sid, (int)bp->pgid);
  spin_unlock(&tasks_lock);
  return n < 0 ? -EIO : n;
}

static ssize_t comm_show(char *buf, size_t len, pid_t pid) {
  if (pid < 0 || pid >= MAX_PROC)
    return -ENOENT;
  spin_lock(&tasks_lock);
  xtask *t = tasks[pid];
  if (!t || t->state == ZOMBIE || t->state == REAPING) {
    spin_unlock(&tasks_lock);
    return -ENOENT;
  }
  int n = snprintf(buf, len, "%s\n", t->comm); // xtask.comm[16] (xtask.h:211)
  spin_unlock(&tasks_lock);
  return n < 0 ? -EIO : n;
}

static ssize_t stat_show(char *buf, size_t len, pid_t pid) {
  if (pid < 0 || pid >= MAX_PROC)
    return -ENOENT;
  spin_lock(&tasks_lock);
  xtask *t = tasks[pid];
  if (!t || t->state == ZOMBIE || t->state == REAPING || !t->proc) {
    spin_unlock(&tasks_lock);
    return -ENOENT;
  }
  proc *bp = t->proc;
  char state_c = t->state == RUNNING   ? 'R'
                 : t->state == ZOMBIE  ? 'Z'
                 : t->state == STOPPED ? 'T'
                                       : 'S';
  int ppid = (int)bp->signal->parent_pid;
  int pgrp = (int)bp->pgid, sess = (int)bp->sid;
  unsigned long utime =
      (unsigned long)(t->cpu_time_ns / 10000000ULL); // clock ticks
  unsigned long stime = 0;
  int prio = t->sched_priority, nice = 0;
  int nthreads =
      (int)atomic_read(&bp->signal->thread_count); // atomic.h uses atomic_read
  int n = snprintf(
      buf, len,
      "%d (%s) %c %d %d %d 0 0 0 0 0 0 %lu %lu %d %d %d 0 0 0 0 0 0 0\n",
      (int)pid, t->comm, state_c, ppid, pgrp, sess, utime, stime, prio, nice,
      nthreads);
  spin_unlock(&tasks_lock);
  return n < 0 ? -EIO : n;
}

static ssize_t maps_show(char *buf, size_t len, pid_t pid) {
  if (pid < 0 || pid >= MAX_PROC)
    return -ENOENT;
  // 1. tasks_lock validation + take mm reference (manual refcount_inc, no
  // mm_get) (procfs.md §3.3.2)
  spin_lock(&tasks_lock);
  xtask *t = tasks[pid];
  if (!t || t->state == ZOMBIE || t->state == REAPING || !t->mm) {
    spin_unlock(&tasks_lock);
    return -ENOENT;
  }
  mm *m = t->mm;
  refcount_inc(&m->m_count); // mirrors proc.c:1090, the only other ref point
  spin_unlock(&tasks_lock);
  // 2. Walk the VMAs holding mmap_lock (not embedded in tasks_lock)
  size_t pos = 0;
  uint64_t flags;
  spin_lock_irqsave(&m->mmap_lock, &flags);
  for (mmap_region *mr = m->mmap_regions; mr && pos + 1 < len; mr = mr->next) {
    char r = (mr->prot & PROT_READ) ? 'r' : '-';
    char w = (mr->prot & PROT_WRITE) ? 'w' : '-';
    char x = (mr->prot & PROT_EXEC) ? 'x' : '-';
    int n = snprintf(buf + pos, len - pos, "%lx-%lx %c%c%cp %08lx 00:00 0\n",
                     (unsigned long)mr->vaddr,
                     (unsigned long)(mr->vaddr + mr->size), r, w, x,
                     (unsigned long)mr->offset);
    if (n < 0)
      break;
    pos += (size_t)n;
  }
  spin_unlock_irqrestore(&m->mmap_lock, flags);
  // 3. Release the mm reference (mm_put, proc.c:734)
  mm_put(m);
  return (ssize_t)pos;
}

static ssize_t cmdline_show(char *buf, size_t len, pid_t pid) {
  // /proc/[pid]/cmdline: argv joined by \0 (procfs.md §3.3.1). Read the pinfo
  // side-table string; hold no global lock while the reference is held (§3.5
  // sync policy). Returns 0 if the process is dead or has no cmdline.
  struct procfs_str *s = procfs_pinfo_ref(pid, 1);
  if (!s)
    return 0;
  size_t n = s->len;
  if (n > len)
    n = len;
  if (n)
    __memcpy(buf, s->data, n);
  procfs_str_put(s);
  return (ssize_t)n;
}

static struct procfs_attr meminfo_attr = {"meminfo", meminfo_show};
static struct procfs_attr cpuinfo_attr = {"cpuinfo", cpuinfo_show};
static struct procfs_attr uptime_attr = {"uptime", uptime_show};
static struct procfs_attr version_attr = {"version", version_show};

// ===== inode synthesis (modeled on sysfs.c sysfs_node_to_inode) =====
static const struct inode_operations procfs_dir_iop;
static const struct inode_operations procfs_file_iop;
static const struct inode_operations procfs_lnk_iop;
static const struct inode_operations procfs_fddir_iop;
static struct super_block procfs_sb;

// pid directory inode synthesis: ino=0x21000+pid*2048, i_priv points at the
// pid directory metadata
static struct inode *procfs_piddir_iget(int pid) {
  uint32_t ino = PROCFS_PID_BASE + (uint32_t)pid * PROCFS_PID_STRIDE;
  struct inode *ip = inode_get_or_create(&procfs_sb, ino, INODE_DIR, 0);
  if (!ip)
    return NULL;
  ip->mode = 0040755;
  ip->i_op = &procfs_dir_iop;
  // i_priv uses procfs_root as the pid directory metadata placeholder (M3
  // lookup dispatches child nodes by the attr table)
  ip->i_priv = (void *)procfs_root;
  return ip;
}

// /proc/self magic inode: readlink synthesizes /proc/[current_pid] (procfs.md
// §3.4)
static struct inode *procfs_magic_self_iget(void) {
  // self is in procfs_root->children (pre-built in M1); take its static ino
  spin_lock(&procfs_lock);
  struct procfs_node *s = NULL;
  for (struct procfs_node *c = procfs_root->children; c; c = c->sibling)
    if (__strcmp(c->name, "self") == 0) {
      s = c;
      break;
    }
  spin_unlock(&procfs_lock);
  if (!s)
    return NULL;
  if (s->ip) {
    s->ip->i_op = &procfs_lnk_iop;
    return inode_get(s->ip);
  }
  struct inode *ip = inode_create(&procfs_sb, s->ino, INODE_LNK, 0);
  if (!ip)
    return NULL;
  ip->mode = 0100777;
  ip->i_priv = (void *)s;
  ip->i_op = &procfs_lnk_iop;
  s->ip = inode_get(ip);
  return ip;
}

static struct inode *procfs_node_to_inode(struct procfs_node *n) {
  ASSERT(n);
  if (n->ip) {
    n->ip->i_op = n->is_dir ? &procfs_dir_iop : &procfs_file_iop;
    return inode_get(n->ip);
  }
  int type = n->is_dir ? INODE_DIR : INODE_REGULAR;
  struct inode *ip = inode_create(&procfs_sb, n->ino, type, 0);
  if (!ip)
    return NULL;
  ip->mode = n->is_dir ? 0040755 : 0100444; // modeled on sysfs.c:165
  ip->i_priv = n->is_dir ? (void *)n : (void *)n->attr;
  ip->i_op = n->is_dir ? &procfs_dir_iop : &procfs_file_iop;
  ip->i_fop = type == INODE_REGULAR ? &procfs_fops : NULL;
  n->ip = inode_get(ip);
  ASSERT(ip->i_op != NULL);
  return ip;
}

// ===== fops.read (modeled on sysfs.c sysfs_file_read) =====
static ssize_t procfs_file_read(struct xtask *p, struct file *f, void *buf,
                                size_t count) {
  (void)p;
  struct inode *ip = f->inode;
  if (!ip || !ip->i_priv)
    return -ENODEV;
  struct procfs_attr *attr = (struct procfs_attr *)ip->i_priv;
  if (!attr || !attr->show)
    return 0;
  if (count == 0)
    return 0;

  char kbuf[4096];
  ssize_t n =
      attr->show(kbuf, sizeof(kbuf),
                 (pid_t)((ip->ino - PROCFS_PID_BASE) / PROCFS_PID_STRIDE));
  if (n < 0)
    return n;
  if (n > (ssize_t)sizeof(kbuf))
    n = (ssize_t)sizeof(kbuf);
  if (f->offset >= (uint64_t)n)
    return 0;

  size_t available = (size_t)n - (size_t)f->offset;
  if (count > available)
    count = available;
  if (copy_to_user(buf, kbuf + f->offset, count))
    return -EFAULT;
  f->offset += count;
  return (ssize_t)count;
}

const struct file_operations procfs_fops = {.read = procfs_file_read};

// ===== iop stubs (filled in M2/M3/M4; M1 left empty lookup/getattr to keep
// compiling) =====
static int procfs_getattr(struct inode *ip, struct kstat *ks) {
  __memset(ks, 0, sizeof(*ks));
  ks->st_ino = ip->ino;
  ks->st_mode = ip->mode;
  ks->st_uid = ip->uid;
  ks->st_gid = ip->gid;
  ks->st_nlink = 1;
  ks->st_size = 0;
  ks->st_blksize = 4096;
  return 0;
}

// M4 fd link helpers (implemented in the M4 section below):
static files *procfs_get_files(int pid);
static int procfs_fd_readlink(int pid, int fd, char *buf, size_t bufsiz);

static struct inode *procfs_pidattr_lookup(int pid, const char *name) {
  for (int i = ATTR_PIDDIR + 1; i < ATTR_PID_MAX; i++) { // skip placeholder 0
    if (!pid_attrs[i].name)
      continue;
    if (__strcmp(pid_attrs[i].name, name) != 0)
      continue;
    if (i == ATTR_CWD || i == ATTR_EXE) {
      uint32_t ino =
          PROCFS_PID_BASE + (uint32_t)pid * PROCFS_PID_STRIDE + (uint32_t)i;
      struct inode *ip = inode_get_or_create(&procfs_sb, ino, INODE_LNK, 0);
      if (!ip)
        return NULL;
      ip->mode = 0100777;
      ip->i_op = &procfs_lnk_iop;
      ip->i_priv = (void *)&pid_attrs[i];
      return ip;
    }
    if (i == ATTR_FD) {
      uint32_t ino =
          PROCFS_PID_BASE + (uint32_t)pid * PROCFS_PID_STRIDE + (uint32_t)i;
      struct inode *ip = inode_get_or_create(&procfs_sb, ino, INODE_DIR, 0);
      if (!ip)
        return NULL;
      ip->mode = 0040755;
      ip->i_op = &procfs_fddir_iop;
      ip->i_priv = (void *)procfs_root;
      return ip;
    }
    uint32_t ino =
        PROCFS_PID_BASE + (uint32_t)pid * PROCFS_PID_STRIDE + (uint32_t)i;
    struct inode *ip = inode_get_or_create(&procfs_sb, ino, INODE_REGULAR, 0);
    if (!ip)
      return NULL;
    ip->mode = 0100444;
    ip->i_op = &procfs_file_iop;
    ip->i_fop = &procfs_fops;
    ip->i_priv = (void *)&pid_attrs[i];
    return ip;
  }
  return NULL;
}

// fd directory lookup of "N": parse fd → verify this pid's fd_table[N] is still
// open → build the fd-N lnk inode (ino = FD_BASE + pid*MAX_FD + fd). A closed
// fd yields NULL (ENOENT).
static struct inode *procfs_fdlink_lookup(int pid, const char *name) {
  if (!name[0])
    return NULL;
  int fd = 0;
  for (int i = 0; name[i]; i++) {
    if (name[i] < '0' || name[i] > '9')
      return NULL;
    fd = fd * 10 + (name[i] - '0');
  }
  if (fd < 0 || fd >= MAX_FD)
    return NULL;
  files *fl = procfs_get_files(pid);
  if (!fl)
    return NULL;
  spin_lock(&fl->fd_lock);
  struct file *f = fl->fd_table[fd];
  spin_unlock(&fl->fd_lock);
  if (!f)
    return NULL;
  uint32_t ino = PROCFS_FD_BASE + (uint32_t)pid * MAX_FD + (uint32_t)fd;
  struct inode *ip = inode_get_or_create(&procfs_sb, ino, INODE_LNK, 0);
  if (!ip)
    return NULL;
  ip->mode = 0100777;
  ip->i_op = &procfs_lnk_iop;
  ip->i_priv = NULL; // pid/fd are encoded in ino; readlink decodes by ino
  return ip;
}

static struct inode *procfs_dir_lookup(struct inode *dir, const char *name) {
  struct procfs_node *parent = (struct procfs_node *)dir->i_priv;
  // 1. self magic (only at /proc root)
  if (__strcmp(name, "self") == 0)
    return procfs_magic_self_iget();
  // 2. static global nodes (meminfo/...)
  spin_lock(&procfs_lock);
  struct procfs_node *found = NULL;
  if (parent) {
    for (struct procfs_node *c = parent->children; c; c = c->sibling) {
      if (__strcmp(c->name, name) == 0) {
        found = c;
        break;
      }
    }
  }
  spin_unlock(&procfs_lock);
  if (found)
    return procfs_node_to_inode(found);
  // fd directory (/proc/[pid]/fd): lookup "N" parses fd, verifies it's still
  // open, then builds the fd-N lnk inode. Must dispatch before pidattr: the fd
  // directory ino also falls in the [PID_BASE, FD_BASE) range (ATTR_FD is
  // encoded in the per-pid range), but "N" is a number, not an attr name.
  if (dir->ino >= PROCFS_PID_BASE && dir->ino < PROCFS_FD_BASE) {
    int attr = (int)((dir->ino - PROCFS_PID_BASE) % PROCFS_PID_STRIDE);
    if (attr == ATTR_FD) {
      int piddir_pid = (int)((dir->ino - PROCFS_PID_BASE) / PROCFS_PID_STRIDE);
      return procfs_fdlink_lookup(piddir_pid, name);
    }
    // pid directory: dispatch child nodes by the attr table (status/maps/.../
    // fd). Must precede numeric-pid parsing: name may be a non-numeric attr
    // like "status"/"maps".
    int piddir_pid = (int)((dir->ino - PROCFS_PID_BASE) / PROCFS_PID_STRIDE);
    return procfs_pidattr_lookup(piddir_pid, name);
  }
  // 3. pid directories (procfs.md §3.2.2)
  if (!name[0])
    return NULL;
  int pid = 0;
  for (int i = 0; name[i]; i++) {
    if (name[i] < '0' || name[i] > '9')
      return NULL; // non-numeric → -ENOENT (NULL)
    pid = pid * 10 + (name[i] - '0');
  }
  if (pid < 0 || pid >= MAX_PROC)
    return NULL;
  spin_lock(&tasks_lock);
  xtask *t = tasks[pid];
  if (!t || t->state == ZOMBIE || t->state == REAPING) {
    spin_unlock(&tasks_lock);
    return NULL;
  }
  spin_unlock(&tasks_lock);
  return procfs_piddir_iget(pid);
}

// ===== M4: fd magic links (/proc/[pid]/fd/N) =====
// ino = FD_BASE + pid*MAX_FD + fd (procfs.md §3.4 / §3.6). pid/fd are fully
// encoded in ino, not stored in i_priv — readlink/getdents decode by ino and
// re-validate with fd_lookup.

// Get pid's files (take the proc reference under tasks_lock, files_get +1).
// NULL = process not alive. Caller must not long-block while holding
// tasks_lock; use files_put to return files after leaving the critical section.
static files *procfs_get_files(int pid) {
  if (pid < 0 || pid >= MAX_PROC)
    return NULL;
  spin_lock(&tasks_lock);
  xtask *t = tasks[pid];
  if (!t || t->state == ZOMBIE || t->state == REAPING || !t->proc ||
      !t->proc->files) {
    spin_unlock(&tasks_lock);
    return NULL;
  }
  files *fl = t->proc->files;
  // files has no independent refcount to take; while tasks_lock is held the
  // process cannot reap (files belong to proc); after unlocking use it promptly
  // and return without crossing a sleep.
  spin_unlock(&tasks_lock);
  return fl;
}

// fd-N readlink target synthesis (by file->type, procfs.md §3.4.1 table).
static int procfs_fd_readlink(int pid, int fd, char *buf, size_t bufsiz) {
  files *fl = procfs_get_files(pid);
  if (!fl)
    return -ENOENT;
  if (fd < 0 || fd >= MAX_FD)
    return -EBADF;
  // fd_lookup uses RCU (types.h:156); for simplicity hold fd_lock to take a
  // stable reference (readlink is transient)
  spin_lock(&fl->fd_lock);
  struct file *f = fl->fd_table[fd];
  if (!f) {
    spin_unlock(&fl->fd_lock);
    return -ENOENT;
  }
  int type = f->type;
  int n;
  switch (type) {
  case FD_TTY:
    if (f->pty)
      n = snprintf(buf, bufsiz, "/dev/pts/%d", f->pty->index);
    else
      n = snprintf(buf, bufsiz, "/dev/ttyS0"); // serial tty has no pty
    break;
  case FD_DEV: {
    // Char/block device fd: resolve the real /dev/<name> path (Linux
    // readlink /proc/self/fd/N on a device returns the device path, e.g.
    // /dev/serial, /dev/console, /dev/dri/card0 — NOT an anon_inode magic
    // string, which is reserved for anon-inode-backed files). Without this
    // the serial console fd 0 resolves to anon_inode:[unknown], breaking
    // ttyname_r's stat(readlink) vs fstat(fd) dev/ino cross-check.
    const char *devname = f->inode ? devtmpfs_name_by_inode(f->inode) : NULL;
    if (devname)
      n = snprintf(buf, bufsiz, "/dev/%s", devname);
    else
      n = snprintf(buf, bufsiz, "anon_inode:[unknown]");
    break;
  }
  case FD_PIPE:
    n = snprintf(buf, bufsiz, "pipe:[%lu]", f->inode ? f->inode->ino : 0);
    break;
  case FD_SOCKET:
    n = snprintf(buf, bufsiz, "socket:[%lu]", f->inode ? f->inode->ino : 0);
    break;
  case FD_REGULAR:
  case FD_FILE:
    n = snprintf(buf, bufsiz, "anon_inode:[regular]");
    break;
  case FD_SHM:
    n = snprintf(buf, bufsiz, "anon_inode:[shm]");
    break;
  case FD_EPOLL:
    n = snprintf(buf, bufsiz, "anon_inode:[eventpoll]");
    break;
  case FD_EVENTFD:
    n = snprintf(buf, bufsiz, "anon_inode:[eventfd]");
    break;
  case FD_TIMERFD:
    n = snprintf(buf, bufsiz, "anon_inode:[timerfd]");
    break;
  case FD_SIGNALFD:
    n = snprintf(buf, bufsiz, "anon_inode:[signalfd]");
    break;
  case FD_NETLINK:
    n = snprintf(buf, bufsiz, "anon_inode:[netlink]");
    break;
  case FD_IPC:
    n = snprintf(buf, bufsiz, "anon_inode:[ipc]");
    break;
  case FD_SYNC_FILE:
    n = snprintf(buf, bufsiz, "anon_inode:[sync_file]");
    break;
  default: // FD_NONE/DEV/DIR/unknown
    n = snprintf(buf, bufsiz, "anon_inode:[unknown]");
    break;
  }
  spin_unlock(&fl->fd_lock);
  return n;
}

// fd directory getdents: scan files->fd_table, synthesize a "N" entry per open
// fd.
static ssize_t procfs_fddir_getdents(struct inode *dir,
                                     struct dir_context *ctx) {
  int pid = (int)((dir->ino - PROCFS_PID_BASE) / PROCFS_PID_STRIDE);
  files *fl = procfs_get_files(pid);
  if (!fl)
    return 0;
  if (ctx->pos == (uint64_t)-1)
    return 0;
  size_t cur_pos = 0;
  // dot entries
  uint16_t rdot = (uint16_t)((sizeof(struct dirent64) + 1 + 1 + 7) & ~7);
  if (cur_pos >= ctx->pos && !dir_emit(ctx, ".", 1, cur_pos, dir->ino, DT_DIR))
    return (ssize_t)ctx->written;
  cur_pos += rdot;
  uint16_t rdotdot = (uint16_t)((sizeof(struct dirent64) + 2 + 1 + 7) & ~7);
  uint64_t parent_ino =
      PROCFS_PID_BASE + (uint32_t)pid * PROCFS_PID_STRIDE; // parent = pid dir
  if (cur_pos >= ctx->pos &&
      !dir_emit(ctx, "..", 2, cur_pos, parent_ino, DT_DIR))
    return (ssize_t)ctx->written;
  cur_pos += rdotdot;
  // fd scan: walk fd_table holding fd_lock; dir_emit is a pure kernel-buffer
  // operation (safe under types.h)
  spin_lock(&fl->fd_lock);
  for (int fd = 0; fd < MAX_FD; fd++) {
    if (!fl->fd_table[fd])
      continue;
    char name[16];
    int nl = 0;
    int v = fd;
    if (v == 0) {
      name[0] = '0';
      nl = 1;
    } else {
      char tmp[16];
      int tl = 0;
      while (v) {
        tmp[tl++] = '0' + (v % 10);
        v /= 10;
      }
      while (tl)
        name[nl++] = tmp[--tl];
    }
    name[nl] = '\0';
    uint16_t r = (uint16_t)((sizeof(struct dirent64) + nl + 1 + 7) & ~7);
    if (cur_pos < ctx->pos) {
      cur_pos += r;
      continue;
    }
    uint32_t ino = PROCFS_FD_BASE + (uint32_t)pid * MAX_FD + (uint32_t)fd;
    if (!dir_emit(ctx, name, nl, cur_pos, ino, DT_LNK)) {
      spin_unlock(&fl->fd_lock);
      goto done;
    }
    cur_pos += r;
  }
  spin_unlock(&fl->fd_lock);
  ctx->pos = (uint64_t)-1; // EOF
done:
  return (ssize_t)ctx->written;
}

// pid directory getdents: list the attr child nodes (status/stat/comm/cmdline/
// maps/cwd/exe/fd). Reuses the pid_attrs table (index = ATTR_* enum =
// attr_index), skipping the placeholder index 0.
static ssize_t procfs_piddir_getdents(int pid, struct dir_context *ctx) {
  if (ctx->pos == (uint64_t)-1)
    return 0;
  uint32_t dir_ino = PROCFS_PID_BASE + (uint32_t)pid * PROCFS_PID_STRIDE;
  size_t cur_pos = 0;
  uint16_t rdot = (uint16_t)((sizeof(struct dirent64) + 1 + 1 + 7) & ~7);
  if (cur_pos >= ctx->pos && !dir_emit(ctx, ".", 1, cur_pos, dir_ino, DT_DIR))
    return (ssize_t)ctx->written;
  cur_pos += rdot;
  uint16_t rdotdot = (uint16_t)((sizeof(struct dirent64) + 2 + 1 + 7) & ~7);
  // parent = /proc root (use root ino; procfs_root->ino).
  uint64_t parent_ino = procfs_root ? procfs_root->ino : dir_ino;
  if (cur_pos >= ctx->pos &&
      !dir_emit(ctx, "..", 2, cur_pos, parent_ino, DT_DIR))
    return (ssize_t)ctx->written;
  cur_pos += rdotdot;
  for (int i = ATTR_PIDDIR + 1; i < ATTR_PID_MAX; i++) {
    if (!pid_attrs[i].name)
      continue;
    size_t nl = __strlen(pid_attrs[i].name);
    uint16_t r = (uint16_t)((sizeof(struct dirent64) + nl + 1 + 7) & ~7);
    if (cur_pos < ctx->pos) {
      cur_pos += r;
      continue;
    }
    uint32_t ino = dir_ino + (uint32_t)i;
    // cwd/exe/fd-N are lnk; fd is a dir; the rest are reg.
    unsigned dt = (i == ATTR_CWD || i == ATTR_EXE)
                      ? DT_LNK
                      : (i == ATTR_FD ? DT_DIR : DT_REG);
    if (!dir_emit(ctx, pid_attrs[i].name, (int)nl, cur_pos, ino, dt))
      return (ssize_t)ctx->written;
    cur_pos += r;
  }
  ctx->pos = (uint64_t)-1; // EOF
  return (ssize_t)ctx->written;
}

static const struct inode_operations procfs_dir_iop = {
    .lookup = procfs_dir_lookup, .getattr = procfs_getattr};
static const struct inode_operations procfs_file_iop = {.getattr =
                                                            procfs_getattr};
static int procfs_lnk_readlink(struct inode *ip, char *buf, size_t bufsiz) {
  // self magic
  struct procfs_node *n = (struct procfs_node *)ip->i_priv;
  if (n && __strcmp(n->name, "self") == 0) {
    pid_t pid = current_xtask->pid; // proc.h:142
    return snprintf(buf, bufsiz, "/proc/%d", (int)pid);
  }
  // pid attribute lnk (cwd/exe): decode pid + attr_index from ino
  if (ip->ino >= PROCFS_PID_BASE && ip->ino < PROCFS_FD_BASE) {
    int pid = (int)((ip->ino - PROCFS_PID_BASE) / PROCFS_PID_STRIDE);
    int attr = (int)((ip->ino - PROCFS_PID_BASE) % PROCFS_PID_STRIDE);
    if (attr == ATTR_CWD) {
      if (pid < 0 || pid >= MAX_PROC)
        return -ENOENT;
      spin_lock(&tasks_lock);
      xtask *t = tasks[pid];
      if (!t || t->state == ZOMBIE || t->state == REAPING || !t->proc) {
        spin_unlock(&tasks_lock);
        return -ENOENT;
      }
      int r = snprintf(buf, bufsiz, "%s",
                       t->proc->cwd); // proc.cwd[256] (proc.h:77)
      spin_unlock(&tasks_lock);
      return r;
    }
    if (attr == ATTR_EXE) {
      struct procfs_str *s = procfs_pinfo_ref(pid, 0);
      if (!s)
        return -ENOENT;
      int r = snprintf(buf, bufsiz, "%s", s->data);
      procfs_str_put(s);
      return r;
    }
  }
  // fd link /proc/[pid]/fd/N: ino = FD_BASE + pid*MAX_FD + fd (procfs.md §3.4)
  if (ip->ino >= PROCFS_FD_BASE) {
    int pid = (int)((ip->ino - PROCFS_FD_BASE) / MAX_FD);
    int fd = (int)((ip->ino - PROCFS_FD_BASE) % MAX_FD);
    return procfs_fd_readlink(pid, fd, buf, bufsiz);
  }
  return -EINVAL;
}
static const struct inode_operations procfs_lnk_iop = {
    .readlink = procfs_lnk_readlink, .getattr = procfs_getattr};
static const struct inode_operations procfs_fddir_iop = {
    .lookup = procfs_dir_lookup, .getattr = procfs_getattr};

// ===== fstype (modeled on sysfs.c sysfs_fstype) =====
static struct inode *procfs_mount_root(struct mount_entry *m) {
  (void)m;
  return procfs_node_to_inode(procfs_root);
}

static ssize_t procfs_root_getdents(struct inode *dir,
                                    struct dir_context *ctx) {
  // fd directory (/proc/[pid]/fd): scan files->fd_table, synthesize a lnk entry
  // per fd.
  if (dir->ino >= PROCFS_PID_BASE && dir->ino < PROCFS_FD_BASE) {
    int attr = (int)((dir->ino - PROCFS_PID_BASE) % PROCFS_PID_STRIDE);
    if (attr == ATTR_FD)
      return procfs_fddir_getdents(dir, ctx);
    // pid directory: list attr child nodes (status/stat/.../fd).
    int pid = (int)((dir->ino - PROCFS_PID_BASE) / PROCFS_PID_STRIDE);
    return procfs_piddir_getdents(pid, ctx);
  }
  struct procfs_node *n = (struct procfs_node *)dir->i_priv;
  if (!n || !n->is_dir)
    return 0;
  if (ctx->pos == (uint64_t)-1)
    return 0;
  size_t cur_pos = 0;
  // 1. synthesize dot entries (modeled on sysfs.c:257-272)
  spin_lock(&procfs_lock);
  uint64_t parent_ino = n->parent ? n->parent->ino : n->ino;
  {
    uint16_t r = (uint16_t)((sizeof(struct dirent64) + 1 + 1 + 7) & ~7);
    if (cur_pos >= ctx->pos &&
        !dir_emit(ctx, ".", 1, cur_pos, n->ino, DT_DIR)) {
      spin_unlock(&procfs_lock);
      goto done;
    }
    cur_pos += r;
  }
  {
    uint16_t r = (uint16_t)((sizeof(struct dirent64) + 2 + 1 + 7) & ~7);
    if (cur_pos >= ctx->pos &&
        !dir_emit(ctx, "..", 2, cur_pos, parent_ino, DT_DIR)) {
      spin_unlock(&procfs_lock);
      goto done;
    }
    cur_pos += r;
  }
  // 2. static global nodes (meminfo/cpuinfo/uptime/version/self)
  for (struct procfs_node *c = n->children; c; c = c->sibling) {
    size_t nl = __strlen(c->name);
    uint16_t r = (uint16_t)((sizeof(struct dirent64) + nl + 1 + 7) & ~7);
    if (cur_pos < ctx->pos) {
      cur_pos += r;
      continue;
    }
    unsigned dt =
        c->kind == PROCFS_MAGIC ? DT_LNK : (c->is_dir ? DT_DIR : DT_REG);
    if (!dir_emit(ctx, c->name, (int)nl, cur_pos, c->ino, dt)) {
      spin_unlock(&procfs_lock);
      goto done;
    }
    cur_pos += r;
  }
  spin_unlock(&procfs_lock);
  // 3. scan tasks[] to list pid directories (procfs.md §3.2.1). dir_emit is a
  //    pure kernel-buffer operation (mount.c:192), safe holding tasks_lock;
  //    copy_to_user happens in sys_getdents outside the lock.
  spin_lock(&tasks_lock);
  for (int pid = 0; pid < MAX_PROC; pid++) {
    xtask *t = tasks[pid];
    if (!t)
      continue;
    if (t->state == ZOMBIE || t->state == REAPING)
      continue; // liveness judged by state (procfs.md §3.2.1)
    char name[16];
    int nl = 0;
    int v = pid;
    if (v == 0) {
      name[0] = '0';
      nl = 1;
    } else {
      char tmp[16];
      int tl = 0;
      while (v) {
        tmp[tl++] = '0' + (v % 10);
        v /= 10;
      }
      while (tl)
        name[nl++] = tmp[--tl];
    }
    name[nl] = '\0';
    uint16_t r = (uint16_t)((sizeof(struct dirent64) + nl + 1 + 7) & ~7);
    if (cur_pos < ctx->pos) {
      cur_pos += r;
      continue;
    }
    if (!dir_emit(ctx, name, nl, cur_pos,
                  PROCFS_PID_BASE + (uint32_t)pid * PROCFS_PID_STRIDE,
                  DT_DIR)) {
      spin_unlock(&tasks_lock);
      goto done;
    }
    cur_pos += r;
  }
  spin_unlock(&tasks_lock);
  ctx->pos = (uint64_t)-1; // EOF
done:
  return (ssize_t)ctx->written;
}

struct fstype procfs_fstype = {
    .name = "procfs",
    .mount_root = procfs_mount_root,
    .getdents = procfs_root_getdents,
};

// ===== initialization (modeled on sysfs.c sysfs_init) =====
void procfs_init(void) {
  if (procfs_root)
    return;
  procfs_root = pnode_alloc("", true, PROCFS_STATIC);
  if (!procfs_root) {
    printk(LOG_ERROR, "procfs_init: failed to alloc root\n");
    return;
  }
  // Global static nodes (procfs.md §3.1 tree)
  pnode_add(procfs_root, "meminfo", false, PROCFS_STATIC, &meminfo_attr);
  pnode_add(procfs_root, "cpuinfo", false, PROCFS_STATIC, &cpuinfo_attr);
  pnode_add(procfs_root, "uptime", false, PROCFS_STATIC, &uptime_attr);
  pnode_add(procfs_root, "version", false, PROCFS_STATIC, &version_attr);
  // self magic link (readlink wired in M2/M4)
  pnode_add(procfs_root, "self", false, PROCFS_MAGIC, NULL);
  printk(LOG_INFO, "[procfs] init root + static nodes\n");
}

struct procfs_node *procfs_root_node(void) { return procfs_root; }
