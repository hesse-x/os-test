/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */
#include "kernel/bsd/procfs.h"

#include <stddef.h>

#include "arch/x64/apic.h" // sched_clock
#include "arch/x64/utils.h"
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

/* ===== 全局状态(仿 sysfs.c:24-26) ===== */
static struct procfs_node *procfs_root;
static spinlock procfs_lock = SPINLOCK_INIT;  /* 保护静态树结构 */
static uint32_t procfs_ino_counter = 0x20000; /* 静态全局节点 ino 段 */

/* ===== ino 反解常量(procfs.md §3.6) ===== */
#define PROCFS_PID_BASE 0x21000u
#define PROCFS_PID_STRIDE 2048u
#define PROCFS_FD_BASE 0x300000u

/* ===== pinfo 侧表(procfs.md §3.5;M5 实装) =====
 * exe/cmdline 不存进 struct proc(STATIC_ASSERT + 驱动镜像锁定,§3.5),用 procfs
 * 自有按 pid 索引的侧表。字符串自带 refcount,与 tasks[]/proc_reap 锁上下文解耦:
 * proc_reap 在无锁下被调(tasks[pid] 仍非空、state 仍 ZOMBIE),tasks_lock 防不住
 * clear,故 pinfo 自带 lock + 字符串引用计数——写者原子换指针+放锁,旧串交给最后
 * 一个读者 refcount_dec_and_test 释放;读者 lock 下取指针 refcount_inc
 * 放锁后读。 */
struct procfs_str {
  refcount_t rc;
  size_t len; /* 不含末尾 NUL */
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

/* 放一个字符串引用:归零则 free(读者退出时调)。 */
static void procfs_str_put(struct procfs_str *p) {
  if (p && refcount_dec_and_test(&p->rc))
    kfree(p);
}

struct procfs_pinfo {
  refcount_t rc; /* 结构体引用计数:读者持引用期间 clear 不 kfree(§3.5) */
  spinlock lock;
  struct procfs_str *exe;
  struct procfs_str *cmdline;
};
static struct procfs_pinfo *pinfo_table[MAX_PROC]; /* tasks_lock 保护表槽 */

/* 取 pid 的 pinfo 槽(不存在则 kmalloc 建)。调用者不持 pinfo->lock。 */
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

/* 在 pinfo->lock 下原子换某字符串槽,返回旧串(调用者 lock 外 put)。 */
static struct procfs_str *procfs_pinfo_swap(struct procfs_pinfo *pi,
                                            struct procfs_str **slot,
                                            struct procfs_str *nw) {
  spin_lock(&pi->lock);
  struct procfs_str *old = *slot;
  *slot = nw;
  spin_unlock(&pi->lock);
  return old;
}

/* procfs_pinfo_set:execve/proc_create 抓 exe=argv[0]、cmdline=argv 拼接(\0
 * 分隔)。 envp 本期不存(§3.5 environ 留 TODO)。argv/envp 可为 NULL(init 单独传
 * exe)。 */
void procfs_pinfo_set(pid_t pid, const char *exe, char *const argv[],
                      char *const envp[]) {
  (void)envp; /* environ 留 TODO */
  struct procfs_pinfo *pi = procfs_pinfo_get(pid);
  if (!pi)
    return;
  /* exe:优先 argv[0](Linux 约定),否则用 exe 形参。 */
  const char *exe_str = (argv && argv[0]) ? argv[0] : exe;
  if (exe_str) {
    size_t el = __strlen(exe_str);
    struct procfs_str *nw = procfs_str_new(exe_str, el);
    if (nw)
      procfs_str_put(procfs_pinfo_swap(pi, &pi->exe, nw));
  }
  /* cmdline:argv 各项 \0 拼接(末尾也带 \0,对齐 Linux /proc/[pid]/cmdline)。 */
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

/* procfs_pinfo_clear:reap 清理。摘表槽 + put 字符串(读者持引用则延迟 free)
 * + 放结构体引用(表槽占 1 份;读者另 +1,故无读者时此处归零 kfree)。 */
void procfs_pinfo_clear(pid_t pid) {
  if (pid < 0 || pid >= MAX_PROC)
    return;
  spin_lock(&tasks_lock);
  struct procfs_pinfo *pi = pinfo_table[pid];
  pinfo_table[pid] = NULL;
  spin_unlock(&tasks_lock);
  if (!pi)
    return;
  /* 先摘字符串引用:换出后旧串交给读者 put(若有),否则此处归零 free。
   * 不持 pi->lock 也安全——clear 后无人再 swap 进来(set 按 pid 走新槽)。 */
  procfs_str_put(procfs_pinfo_swap(pi, &pi->exe, NULL));
  procfs_str_put(procfs_pinfo_swap(pi, &pi->cmdline, NULL));
  if (refcount_dec_and_test(&pi->rc))
    kfree(pi); /* 无读者 → 立即 free */
}

/* 读者辅助:tasks_lock 下取结构体引用(+rc),再 pinfo->lock 下取字符串引用(+rc),
 * 放结构体引用;返回字符串引用(调用者用完
 * procfs_str_put)。进程不存活/槽空/字段空 返回 NULL。结构体引用计数保证 clear
 * 期间 pi 不会被 kfree(§3.5)。 */
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
    kfree(pi); /* clear 已摘槽且无其他读者 → free */
  return s;
}

/* ===== 节点分配(仿 sysfs.c node_alloc) ===== */
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

/* ===== pid 属性 attr_index(procfs.md §3.2.3 / §3.6) =====
 * attr_index 从 1 起:pid 目录占 attr_index==0(PID_BASE+pid*2048),任何文件
 * attr 不得与之冲突——否则 inode_get_or_create 复用同一 inode,pid 目录被当
 * 作 status 文件读(i_priv 语义混淆)。procfs.md §3.6 漏列此约束,此处补正。 */
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

/* pid_attrs 按 ATTR_* 枚举下标排列:下标 0 保留(pid 目录占 attr_index==0),
 * ATTR_STATUS=1..ATTR_FD=8 对应真属性。lookup 用数组下标 i 直接作 attr_index
 * (即枚举值),readlink 按 (ino-PID_BASE)%STRIDE 反解 attr 与 ATTR_CWD/ATTR_EXE
 * 匹配——故下标必须等于枚举值,不得错位。 */
static const struct procfs_attr pid_attrs[] = {
    {NULL, NULL},              /* [ATTR_PIDDIR=0] 占位:pid 目录,无 attr */
    {"status", status_show},   /* ATTR_STATUS=1 */
    {"stat", stat_show},       /* ATTR_STAT=2 */
    {"comm", comm_show},       /* ATTR_COMM=3 */
    {"cmdline", cmdline_show}, /* ATTR_CMDLINE=4 */
    {"maps", maps_show},       /* ATTR_MAPS=5 */
    {"cwd", NULL},             /* ATTR_CWD=6, lnk,M4 readlink */
    {"exe", NULL},             /* ATTR_EXE=7, lnk,M5 readlink */
    {"fd", NULL},              /* ATTR_FD=8, dir,M4 */
    {NULL, NULL}};

/* ===== 全局静态 show 回调(procfs.md §3.3 表) ===== */
static ssize_t meminfo_show(char *buf, size_t len, pid_t pid) {
  (void)pid;
  /* kernel_mem_stats 6 字段(slab.c:24): total_pages/used_pages/slab_used_bytes/
   * slab_peak_bytes/kmalloc_calls/kfree_calls; total_page_frames(alloc.h:76);
   * bfc_free_page_nums()(alloc.h:64) */
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
  uint64_t ns = sched_clock(); /* apic.c:30 */
  return snprintf(buf, len, "%llu.%02llu 0.00\n",
                  (unsigned long long)(ns / 1000000000ULL),
                  (unsigned long long)((ns / 10000000ULL) % 100));
}

static ssize_t version_show(char *buf, size_t len, pid_t pid) {
  (void)pid;
  return snprintf(buf, len, "%s\n", KERNEL_VERSION);
}

/* cpuinfo(procfs.md §3.3 表 / M6 Step 42):vendor(cpuid 0) + brand string
 * (cpuid 0x80000002-4) + cpu MHz(由 tsc_freq) + bogomips 占位。每核一节,对齐
 * Linux。 */
static void cpuid_leaf(uint32_t leaf, uint32_t *a, uint32_t *b, uint32_t *c,
                       uint32_t *d) {
  __asm__ volatile("cpuid"
                   : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
                   : "a"(leaf));
}

static ssize_t cpuinfo_show(char *buf, size_t len, pid_t pid) {
  (void)pid;
  extern int ncpu;          /* smp.c:24 */
  extern uint64_t tsc_freq; /* apic.h:121,TSC ticks/sec */
  /* vendor(cpuid 0):ebx:edx:ecx → 12 字符串。 */
  uint32_t a, b, c, d;
  cpuid_leaf(0, &a, &b, &c, &d);
  char vendor[13];
  *(uint32_t *)(vendor + 0) = b;
  *(uint32_t *)(vendor + 4) = d;
  *(uint32_t *)(vendor + 8) = c;
  vendor[12] = '\0';
  /* brand string(cpuid 0x80000002-4):3 leaf × 16 字节 = 48 字符,末尾 NUL。 */
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

/* ===== per-pid show 回调(procfs.md §3.3 表 / §3.3.1 / §3.3.2) ===== */
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
  int n =
      snprintf(buf, len, "%s\n", t->comm); /* xtask.comm[16] (xtask.h:211) */
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
      (unsigned long)(t->cpu_time_ns / 10000000ULL); /* clock ticks */
  unsigned long stime = 0;
  int prio = t->sched_priority, nice = 0;
  int nthreads =
      (int)atomic_read(&bp->signal->thread_count); /* atomic.h 用 atomic_read */
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
  /* 1. tasks_lock 校验 + 取 mm 引用(手动 refcount_inc,无 mm_get)(procfs.md
   * §3.3.2) */
  spin_lock(&tasks_lock);
  xtask *t = tasks[pid];
  if (!t || t->state == ZOMBIE || t->state == REAPING || !t->mm) {
    spin_unlock(&tasks_lock);
    return -ENOENT;
  }
  mm *m = t->mm;
  refcount_inc(&m->m_count); /* 仿 proc.c:1090 唯一现存额外引用点 */
  spin_unlock(&tasks_lock);
  /* 2. 持 mmap_lock 遍历 VMA(不嵌 tasks_lock) */
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
  /* 3. 放 mm 引用(mm_put,proc.c:734) */
  mm_put(m);
  return (ssize_t)pos;
}

static ssize_t cmdline_show(char *buf, size_t len, pid_t pid) {
  /* /proc/[pid]/cmdline:argv \0 拼接(procfs.md §3.3.1)。读 pinfo 侧表字符串,
   * 持引用期间不持任何全局锁(§3.5 同步策略)。进程不存活/无 cmdline 返回 0。 */
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

/* ===== inode 合成(仿 sysfs.c sysfs_node_to_inode) ===== */
static const struct inode_operations procfs_dir_iop;
static const struct inode_operations procfs_file_iop;
static const struct inode_operations procfs_lnk_iop;
static const struct inode_operations procfs_fddir_iop;

/* pid 目录 inode 合成:ino=0x21000+pid*2048,i_priv 指向 pid 目录元数据 */
static struct inode *procfs_piddir_iget(int pid) {
  uint32_t ino = PROCFS_PID_BASE + (uint32_t)pid * PROCFS_PID_STRIDE;
  struct inode *ip = inode_get_or_create(ino, INODE_DIR, 0, 0, 0, 0);
  if (!ip)
    return NULL;
  ip->mode = 0040755;
  ip->i_op = &procfs_dir_iop;
  /* i_priv 用 procfs_root 作为 pid 目录元数据占位(M3 lookup 子节点按 attr
   * 表分发) */
  ip->i_priv = (void *)procfs_root;
  return ip;
}

/* /proc/self 魔幻 inode:readlink 合成 /proc/[current_pid](procfs.md §3.4) */
static struct inode *procfs_magic_self_iget(void) {
  /* self 在 procfs_root->children 中(M1 预建),取其静态 ino */
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
  struct inode *ip = inode_create(s->ino, INODE_LNK, 0, 0, 0, 0);
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
  struct inode *ip = inode_create(n->ino, type, 0, 0, 0, 0);
  if (!ip)
    return NULL;
  ip->mode = n->is_dir ? 0040755 : 0100444; /* 仿 sysfs.c:165 */
  ip->i_priv = n->is_dir ? (void *)n : (void *)n->attr;
  ip->i_op = n->is_dir ? &procfs_dir_iop : &procfs_file_iop;
  n->ip = inode_get(ip);
  ASSERT(ip->i_op != NULL);
  return ip;
}

/* ===== fops.read(仿 sysfs.c sysfs_file_read) ===== */
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

/* ===== iop 桩(M2/M3/M4 填实;M1 先空 lookup/getattr 保证编译) ===== */
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

/* M4 fd 链接辅助(实现在下方 M4 段): */
static files *procfs_get_files(int pid);
static int procfs_fd_readlink(int pid, int fd, char *buf, size_t bufsiz);

static struct inode *procfs_pidattr_lookup(int pid, const char *name) {
  for (int i = ATTR_PIDDIR + 1; i < ATTR_PID_MAX; i++) { /* 跳过占位下标 0 */
    if (!pid_attrs[i].name)
      continue;
    if (__strcmp(pid_attrs[i].name, name) != 0)
      continue;
    if (i == ATTR_CWD || i == ATTR_EXE) {
      uint32_t ino =
          PROCFS_PID_BASE + (uint32_t)pid * PROCFS_PID_STRIDE + (uint32_t)i;
      struct inode *ip = inode_get_or_create(ino, INODE_LNK, 0, 0, 0, 0);
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
      struct inode *ip = inode_get_or_create(ino, INODE_DIR, 0, 0, 0, 0);
      if (!ip)
        return NULL;
      ip->mode = 0040755;
      ip->i_op = &procfs_fddir_iop;
      ip->i_priv = (void *)procfs_root;
      return ip;
    }
    uint32_t ino =
        PROCFS_PID_BASE + (uint32_t)pid * PROCFS_PID_STRIDE + (uint32_t)i;
    struct inode *ip = inode_get_or_create(ino, INODE_REGULAR, 0, 0, 0, 0);
    if (!ip)
      return NULL;
    ip->mode = 0100444;
    ip->i_op = &procfs_file_iop;
    ip->i_priv = (void *)&pid_attrs[i];
    ip->mount = NULL; /* f_op 接线靠 ip->mount->fs->name;由 sys_open 惰性设 */
    return ip;
  }
  return NULL;
}

/* fd 目录内 lookup "N":解析 fd → 校验该 pid 的 fd_table[N] 仍开 → 建 fd-N lnk
 * inode(ino = FD_BASE + pid*MAX_FD + fd)。fd 已 close 则 NULL(ENOENT)。 */
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
  struct inode *ip = inode_get_or_create(ino, INODE_LNK, 0, 0, 0, 0);
  if (!ip)
    return NULL;
  ip->mode = 0100777;
  ip->i_op = &procfs_lnk_iop;
  ip->i_priv = NULL; /* pid/fd 编进 ino,readlink 按 ino 反解 */
  return ip;
}

static struct inode *procfs_dir_lookup(struct inode *dir, const char *name) {
  struct procfs_node *parent = (struct procfs_node *)dir->i_priv;
  /* 1. self 魔幻(仅 /proc 根) */
  if (__strcmp(name, "self") == 0)
    return procfs_magic_self_iget();
  /* 2. 静态全局节点(meminfo/...) */
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
  /* fd 目录(/proc/[pid]/fd):lookup "N" 解析 fd,校验仍开 → 建 fd-N lnk inode。
   * 须在 pidattr 分发前:fd 目录 ino 也落 [PID_BASE, FD_BASE) 段(ATTR_FD 编码
   * 进 per-pid 段),但 "N" 是数字非 attr 名。 */
  if (dir->ino >= PROCFS_PID_BASE && dir->ino < PROCFS_FD_BASE) {
    int attr = (int)((dir->ino - PROCFS_PID_BASE) % PROCFS_PID_STRIDE);
    if (attr == ATTR_FD) {
      int piddir_pid = (int)((dir->ino - PROCFS_PID_BASE) / PROCFS_PID_STRIDE);
      return procfs_fdlink_lookup(piddir_pid, name);
    }
    /* pid 目录:按 attr 表分发子节点(status/maps/.../fd)。
     * 必须在数字 pid 解析之前:name 可能是 "status"/"maps" 等非数字 attr。 */
    int piddir_pid = (int)((dir->ino - PROCFS_PID_BASE) / PROCFS_PID_STRIDE);
    return procfs_pidattr_lookup(piddir_pid, name);
  }
  /* 3. pid 目录(procfs.md §3.2.2) */
  if (!name[0])
    return NULL;
  int pid = 0;
  for (int i = 0; name[i]; i++) {
    if (name[i] < '0' || name[i] > '9')
      return NULL; /* 非数字 -> -ENOENT(NULL) */
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

/* ===== M4: fd 魔幻链接(/proc/[pid]/fd/N) =====
 * ino = FD_BASE + pid*MAX_FD + fd(procfs.md §3.4 / §3.6)。pid/fd 全编进 ino,
 * i_priv 不另存——readlink/getdents 时按 ino 反解后用 fd_lookup 重新校验。 */

/* 取 pid 的 files(持 tasks_lock 取 proc 引用,files_get +1)。NULL = 进程不存活。
 * 调用者持 tasks_lock 期间不得 long-block;files 退出临界后用 files_put 还。 */
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
  /* files 无独立 refcount 取法,持 tasks_lock 期间进程不会 reap(files 归 proc);
   * 退出锁后立即使用并尽快返回,不跨睡眠。 */
  spin_unlock(&tasks_lock);
  return fl;
}

/* fd-N readlink 目标合成(按 file->type,procfs.md §3.4.1 表)。 */
static int procfs_fd_readlink(int pid, int fd, char *buf, size_t bufsiz) {
  files *fl = procfs_get_files(pid);
  if (!fl)
    return -ENOENT;
  if (fd < 0 || fd >= MAX_FD)
    return -EBADF;
  /* fd_lookup 走 RCU(types.h:156);为简单持 fd_lock 取稳定引用(readlink 瞬时) */
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
      n = snprintf(buf, bufsiz, "/dev/pts%d", f->pty->index);
    else
      n = snprintf(buf, bufsiz, "/dev/ttyS0"); /* 串口 tty 无 pty */
    break;
  case FD_PIPE:
    n = snprintf(buf, bufsiz, "pipe:[%u]", f->inode ? f->inode->ino : 0);
    break;
  case FD_SOCKET:
    n = snprintf(buf, bufsiz, "socket:[%u]", f->inode ? f->inode->ino : 0);
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
  default: /* FD_NONE/DEV/DIR/未知 */
    n = snprintf(buf, bufsiz, "anon_inode:[unknown]");
    break;
  }
  spin_unlock(&fl->fd_lock);
  return n;
}

/* fd 目录 getdents:扫 files->fd_table,为每个已开 fd 合成 "N" 条目。 */
static ssize_t procfs_fddir_getdents(struct inode *dir,
                                     struct dir_context *ctx) {
  int pid = (int)((dir->ino - PROCFS_PID_BASE) / PROCFS_PID_STRIDE);
  files *fl = procfs_get_files(pid);
  if (!fl)
    return 0;
  if (ctx->pos == (uint64_t)-1)
    return 0;
  size_t cur_pos = 0;
  /* dot entries */
  uint16_t rdot = (uint16_t)((sizeof(struct dirent64) + 1 + 1 + 7) & ~7);
  if (cur_pos >= ctx->pos && !dir_emit(ctx, ".", 1, cur_pos, dir->ino, DT_DIR))
    return (ssize_t)ctx->written;
  cur_pos += rdot;
  uint16_t rdotdot = (uint16_t)((sizeof(struct dirent64) + 2 + 1 + 7) & ~7);
  uint64_t parent_ino =
      PROCFS_PID_BASE + (uint32_t)pid * PROCFS_PID_STRIDE; /* 父=pid 目录 */
  if (cur_pos >= ctx->pos &&
      !dir_emit(ctx, "..", 2, cur_pos, parent_ino, DT_DIR))
    return (ssize_t)ctx->written;
  cur_pos += rdotdot;
  /* fd 扫描:持 fd_lock 遍历 fd_table,dir_emit 是纯内核 buf 操作(types.h 下安全)
   */
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
  ctx->pos = (uint64_t)-1; /* EOF */
done:
  return (ssize_t)ctx->written;
}

/* pid 目录 getdents:列出 attr
 * 子节点(status/stat/comm/cmdline/maps/cwd/exe/fd)。 复用 pid_attrs
 * 表(下标=ATTR_* 枚举=attr_index),跳过占位下标 0。 */
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
  /* 父= /proc 根(用根 ino;procfs_root->ino)。 */
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
    /* cwd/exe/fd-N 是 lnk;fd 是 dir;其余 reg。 */
    unsigned dt = (i == ATTR_CWD || i == ATTR_EXE)
                      ? DT_LNK
                      : (i == ATTR_FD ? DT_DIR : DT_REG);
    if (!dir_emit(ctx, pid_attrs[i].name, (int)nl, cur_pos, ino, dt))
      return (ssize_t)ctx->written;
    cur_pos += r;
  }
  ctx->pos = (uint64_t)-1; /* EOF */
  return (ssize_t)ctx->written;
}

static const struct inode_operations procfs_dir_iop = {
    .lookup = procfs_dir_lookup, .getattr = procfs_getattr};
static const struct inode_operations procfs_file_iop = {.getattr =
                                                            procfs_getattr};
static int procfs_lnk_readlink(struct inode *ip, char *buf, size_t bufsiz) {
  /* self 魔幻 */
  struct procfs_node *n = (struct procfs_node *)ip->i_priv;
  if (n && __strcmp(n->name, "self") == 0) {
    pid_t pid = current_xtask->pid; /* proc.h:142 */
    return snprintf(buf, bufsiz, "/proc/%d", (int)pid);
  }
  /* pid 属性 lnk(cwd/exe):ino 反解 pid + attr_index */
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
                       t->proc->cwd); /* proc.cwd[256] (proc.h:77) */
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
  /* fd 链接 /proc/[pid]/fd/N:ino = FD_BASE + pid*MAX_FD + fd(procfs.md §3.4) */
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

/* ===== fstype(仿 sysfs.c sysfs_fstype) ===== */
static struct inode *procfs_mount_root(struct mount_entry *m) {
  (void)m;
  return procfs_node_to_inode(procfs_root);
}

static ssize_t procfs_root_getdents(struct inode *dir,
                                    struct dir_context *ctx) {
  /* fd 目录(/proc/[pid]/fd):扫 files->fd_table,逐 fd 合成 lnk 条目。 */
  if (dir->ino >= PROCFS_PID_BASE && dir->ino < PROCFS_FD_BASE) {
    int attr = (int)((dir->ino - PROCFS_PID_BASE) % PROCFS_PID_STRIDE);
    if (attr == ATTR_FD)
      return procfs_fddir_getdents(dir, ctx);
    /* pid 目录:列出 attr 子节点(status/stat/.../fd)。 */
    int pid = (int)((dir->ino - PROCFS_PID_BASE) / PROCFS_PID_STRIDE);
    return procfs_piddir_getdents(pid, ctx);
  }
  struct procfs_node *n = (struct procfs_node *)dir->i_priv;
  if (!n || !n->is_dir)
    return 0;
  if (ctx->pos == (uint64_t)-1)
    return 0;
  size_t cur_pos = 0;
  /* 1. 合成 dot entries(仿 sysfs.c:257-272) */
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
  /* 2. 静态全局节点(meminfo/cpuinfo/uptime/version/self) */
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
  /* 3. 扫 tasks[] 列 pid 目录(procfs.md §3.2.1)。dir_emit 是纯内核 buffer
   *    操作(mount.c:192),持 tasks_lock 安全;copy_to_user 在 sys_getdents 锁外。
   */
  spin_lock(&tasks_lock);
  for (int pid = 0; pid < MAX_PROC; pid++) {
    xtask *t = tasks[pid];
    if (!t)
      continue;
    if (t->state == ZOMBIE || t->state == REAPING)
      continue; /* 存活判据用 state(procfs.md §3.2.1) */
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
  ctx->pos = (uint64_t)-1; /* EOF */
done:
  return (ssize_t)ctx->written;
}

struct fstype procfs_fstype = {
    .name = "procfs",
    .mount_root = procfs_mount_root,
    .getdents = procfs_root_getdents,
};

/* ===== 初始化(仿 sysfs.c sysfs_init) ===== */
void procfs_init(void) {
  if (procfs_root)
    return;
  procfs_root = pnode_alloc("", true, PROCFS_STATIC);
  if (!procfs_root) {
    printk(LOG_ERROR, "procfs_init: failed to alloc root\n");
    return;
  }
  /* 全局静态节点(procfs.md §3.1 树) */
  pnode_add(procfs_root, "meminfo", false, PROCFS_STATIC, &meminfo_attr);
  pnode_add(procfs_root, "cpuinfo", false, PROCFS_STATIC, &cpuinfo_attr);
  pnode_add(procfs_root, "uptime", false, PROCFS_STATIC, &uptime_attr);
  pnode_add(procfs_root, "version", false, PROCFS_STATIC, &version_attr);
  /* self 魔幻链接(M2/M4 接 readlink) */
  pnode_add(procfs_root, "self", false, PROCFS_MAGIC, NULL);
  printk(LOG_INFO, "[procfs] init root + static nodes\n");
}

struct procfs_node *procfs_root_node(void) { return procfs_root; }
