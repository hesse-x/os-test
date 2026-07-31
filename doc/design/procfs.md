# procfs 设计方案

> 文档定位：os2 微内核 `/proc` 进程文件系统的工程落地方案。所有结构体、回调签名、行号引用均基于实盘代码（2026-07-31 调研），不脑补。配套设计文档：`doc/design/kernel/mount.md`（挂载框架）、`doc/design/sysfs.md`（伪文件系统文本模板）、`doc/design/kernel/proc.md`（进程结构）、`doc/design/todo.md`（路线图与债务登记）。

---

## 1 项目概述

### 1.1 背景与目标

os2 当前**没有 pid 发现机制**，`doc/design/todo.md:249` 明确记载"当前 OS 无 pid 发现机制（无 `/proc`、udevd 不写 pid 文件……）"。`todo.md` 另有三处把 procfs 列为依赖解锁路径：`:311`（ttyname/ttyname_r）、`:331`（procfs 上线后下掉 repo 版 ttyname）、`:343`（realpath）。这造成两类实际阻塞：

- **musl libc 依赖**：`ttyname`/`ttyname_r` 硬依赖 `/proc/self/fd/N` 魔幻符号链接来反查 tty 设备路径并做 `stat`/`fstat` dev+ino 交叉校验（`src/unistd/ttyname_r.c:9`）；`pthread_getname_np`/`pthread_setname_np` 读写 `/proc/self/task/%d/comm`；动态链接器 `$ORIGIN` 解析软依赖 `readlink("/proc/self/exe")`（`ldso/dynlink.c:926`，ENOENT 时优雅回退）。
  - **范围澄清**：`isatty` 和 `dladdr` 在上游 musl **不**触碰 `/proc`（isatty 只调 `ioctl(TIOCGWINSZ)`；dladdr 是 `return 0` stub）。且 musl 的 `ttyname`/`ttyname_r`/`isatty` 当前被 `build_script/third_party/musl/modules/unistd.cmake` 从构建中 REMOVE，由 `user/lib/file.cc:250,283` 的 repo 版本替代。因此**当前没有 live 硬依赖**——依赖只在放弃这些 REMOVE、改用上游 `ttyname_r` 时才出现（这正是 `todo.md:331` 的解锁目标）。
- **procps 工具链**：`ps`/`top`/`free`/`pidof` 等调试与运维工具全部通过 `/proc` 读取进程与内核状态，是"在 OS 上构建 gcc"后续调试基础设施的前提。

**procfs 目标**：以最小内核改动，提供一个只读的、Linux 兼容的 `/proc`，覆盖进程级信息（`status`/`stat`/`maps`/`cmdline`/`fd`/`cwd`/`exe`/`comm`）、内核全局统计（`meminfo`/`cpuinfo`/`uptime`/`version`）与魔幻符号链接（`self`/`self/fd/N`），使 musl 依赖路径与 procps 基本工具链可用。

### 1.2 产品定位与使用场景

| 场景 | 消费路径 | 优先级 |
|------|----------|--------|
| musl `ttyname_r` | `/proc/self/fd/N` → readlink → stat/fstat dev+ino 交叉校验 | P0（gcc 里程碑前置） |
| musl `$ORIGIN` 解析 | `/proc/self/exe` → readlink（软依赖，ENOENT 可回退） | P0 |
| musl `pthread_*name_np` | `/proc/self/task/%d/comm` → 读/写 | P2（线程级，见 M6 后续） |
| `ps`/`top` 进程列表 | `/proc/` getdents + `/proc/[pid]/stat`（~52 字段） | P1 |
| `free` 内存 | `/proc/meminfo` | P1 |
| 调试 `/proc/[pid]/maps` | `mm->mmap_regions` VMA 链 | P1 |
| `uptime`/`cat /proc/version` | `sched_clock()` / `KERNEL_VERSION` | P2 |

### 1.3 核心技术指标与约束条件

- **只读**：procfs 不提供 `create`/`mkdir`/`unlink`/`rmdir`/`write` 回调（`i_op` 留 NULL，`fops` 仅 `read`），防止用户态篡改内核状态。少数写入项（如 `/proc/sys/`、`/proc/self/task/%d/comm`）本期不做。
- **不动核心结构体**：`struct proc`（`proc.h:21-88`）由 `STATIC_ASSERT(sizeof(proc)==536)`（`proc.h:106`）锁定，且驱动侧 `kernel/driver/bsd_types.h:97-137` 维护字节级镜像（`DRV_STATIC_ASSERT(sizeof(proc)==536)` 于 `bsd_types.h:149`）。**procfs 缺口字段（exe/cmdline/environ/root）一律存于 procfs 自有侧表，不扩 `struct proc`**，规避二进制布局风险。同理不扩 `struct inode`（仅单个 `i_priv` 槽，`inode.h:80`），pid 改编码进 ino（见 3.1/3.6）。
- **ino 独立段**：procfs 用三段——静态全局 `0x20000+`、per-pid `0x21000+`（stride 2048，上限 `0x211000`）、fd 链接 `0x300000+`——避开 sysfs `0x10000+`（`sysfs.c:26`）与 devtmpfs `0x80000000+`（`inode.c:17`），段内无冲突且能从 ino 干净反解 pid/attr/fd（per-pid 段上限 `0x211000` < fd 段起点 `0x300000`）。
- **零预建 per-pid 子树**：`pid == tasks[] 数组下标`（`xtask.h:231` 注释"Retains pid == array-index semantics"），per-pid 子树在 lookup/getdents 时从 `tasks[pid]` 实时合成，进程 reap 时无需清理 procfs 树。
- **稳定性**：所有外部输入（pid、fd、偏移）做边界校验；持锁遍历遵循既有锁序（`tasks_lock`、`mm->mmap_lock`、`files->fd_lock`、procfs 自有 `pinfo` 锁），不引入跨锁回边。

### 1.4 架构选型理由

procfs 是**内核态内存伪文件系统**，挂在已有 VFS mount 框架下。选型对齐 os2 既有三层：

- **硬件驱动下沉内核态**：不适用——procfs 不碰硬件。
- **机制在内核、策略在用户态**：procfs 只暴露原始状态文本，所有解读（ps 排序、top 刷新）在用户态完成。
- **复用 sysfs 模板**：sysfs 已验证"属性树 + fops + 按需建 inode"模式（`sysfs.c`），procfs 照搬，新增代码量最小。详见 §2.2。

---

## 2 总体架构设计

### 2.1 分层架构图

```
┌─────────────────────────── 用户态 ───────────────────────────┐
│  musl libc (ttyname_r/$ORIGIN/pthread_*name)  │  procps (ps/top/free)  │
│         ↓ read/getdents/readlink         ↓                    │
└───────────────────────────────────────────────────────────────┘
                            ↑ syscall
┌─────────────────────────── BSD 层 ───────────────────────────┐
│  VFS: sys_open/sys_read/sys_getdents/sys_readlink            │
│   ├─ mount 最长前缀匹配 → /proc 命中 procfs_fstype           │
│   ├─ sys_open: 按 ip->mount->fs->name == "procfs" 接 f_op    │
│   └─ fd-I/O 分发: f_op->read / m->fs->getdents               │
│                                                               │
│  procfs.c (新):                                              │
│   ├─ procfs_fstype {mount_root, getdents}                    │
│   ├─ procfs_node 树 (静态全局节点 + per-pid 惰性合成)        │
│   ├─ procfs_fops.read → attr->show(buf, len, pid)            │
│   ├─ procfs_dir_iop {lookup, getattr}                        │
│   └─ procfs_lnk_iop {readlink, getattr}  (魔幻 + fd 链接)    │
│                                                               │
│  数据来源 (现有,只读):                                        │
│   tasks[]/tasks_lock  xtask/proc  mm->mmap_regions  files    │
│   kernel_mem_stats  ncpu/g_madt  sched_clock()  KERNEL_VERSION│
└───────────────────────────────────────────────────────────────┘
                            ↑ 仅引用,不修改
┌─────────────────────────── Xcore 层 ─────────────────────────┐
│  调度器 tasks[] 数组 │ 内存 VMA │ slab 统计 │ 时钟 │ SMP      │
└───────────────────────────────────────────────────────────────┘
```

### 2.2 模块拆分与边界划分

| 模块 | 职责 | 边界 |
|------|------|------|
| `procfs.c/h`（新） | procfs 自身：节点树、fstype、fops、iop、show 回调、惰性合成、pinfo 侧表 | 仅读 Xcore/BSD 数据，不修改（pinfo 侧表除外，自有生命周期） |
| `vfs.c`（改） | 挂载接入（`vfs_init`）+ f_op 接线（`sys_open` 与 `sys_openat` 两处 fstype-name 分支各加一条 "procfs" 分支） | 2 处 +1 分支 |
| `mount.c` | 已有 mount 框架，procfs 作为新 fstype 注册 | 不改 |
| `kernel.h`（改） | 新增 `KERNEL_VERSION` 宏 | 1 行 |
| `proc.c`（改） | execve 成功后 `procfs_pinfo_set`（hook 点在 `argv_strings` kfree 之前，`proc.c:1765` 附近）；`proc_reap`（`proc.c:150`）末尾 `procfs_pinfo_clear` | +2 处 hook |
| execve mm swap（改） | `proc.c:1824-1846` 换 `mmap_regions`/`cr3` 时持 `mm->mmap_lock`（procfs 暴露的既有跨线程隐患） | 1 处锁点（见 3.3） |
| Xcore 数据层 | `tasks[]`、VMA、`kernel_mem_stats`、`sched_clock()` | 不改，仅被读 |

### 2.3 内核态与用户态通信方案

procfs 走**标准文件 I/O**（open/read/getdents/readlink/close），不引入新 syscall、不引入新 ioctl。理由：

- `/proc` 内容是文本流，天然适配 `read`；魔幻符号链接适配 `readlink`。
- 现有 `sys_read` 已支持 `f_op->read` 分发（`syscall.c:2415-2417`），`sys_getdents` 已支持 `m->fs->getdents` 分发（`vfs.c:1456-1468`），`sys_readlink` 已支持 `i_op->readlink`（`do_readlinkat` `syscall.c:3229-3267`，末段 LNK 原样返回不跟随，POSIX）。procfs 仅需"接线"，零新 syscall。
- 与 sysfs/devtmpfs/tmpfs 完全一致，用户态 libc 无需任何适配即可 `open("/proc/self/status")`。

### 2.4 启动流程：procfs 在引导序列中的位置

`vfs_init`（`vfs.c:44-101`）现有序列：FAT32 注册（:63）→ 挂 `/`（:68）→ 建 `/dev` 挂 devtmpfs（:77）→ 建 `/sys` 挂 sysfs（:84）→ 建 `/run` 挂 tmpfs（:92）。procfs 插在 sysfs 之后：

```
1. register_fstype(&procfs_fstype)          // 与其它 fstype 并列
2. fat32_mkdir("/proc")                      // 宿主目录,供 getdents("/") 可见性
3. mount_internal(&procfs_fstype, "/proc", procfs_root_node())
   // 第 3 参 fs_data = procfs_root_node(),仿 sysfs (vfs.c:84)
```

> `mount_internal` 的 `fs_data` 挂在 `struct mount_entry`（`mount.h:82`），非 `struct fstype`（`mount.h:52-60` 只有 `name`/`mount_root`/`getdents`）。procfs 根节点经 `mount_root` 回调返回，与 sysfs 同模式。

挂载完成后 `/proc` 即可被访问，无需 init 进程介入。procfs 根节点树在 `procfs_init()`（由 `vfs_init` 调用）中预建全局静态节点（`meminfo`/`cpuinfo`/`uptime`/`version`/`self`）；per-pid 子树运行期惰性合成。

---

## 3 核心模块详细设计

> 本节是 procfs 内部模块展开（替代 skill 模板里通用的内存/调度/VFS/ELF/驱动/IPC/日志模块——那些 os2 已有，不在本方案范围）。

### 3.1 procfs 节点树与数据结构

照搬 sysfs `sysfs_node`（`sysfs.h:47-57`）模型，新增"动态合成节点"概念。

```c
/* kernel/bsd/procfs.h */
struct procfs_attr {
  const char *name;
  ssize_t (*show)(char *buf, size_t len, pid_t pid);  /* 生成文本,≤1页;pid 由 ino 反解 */
  /* 无 store:procfs 只读 */
};

enum procfs_node_kind {
  PROCFS_STATIC,   /* mount 时预建的全局节点(meminfo/cpuinfo/...) */
  PROCFS_PIDDIR,   /* /proc/[pid] 目录:lookup 时合成 */
  PROCFS_PIDATTR,  /* /proc/[pid]/xxx 文件:show 时从 tasks[pid] 读 */
  PROCFS_PIDFD,    /* /proc/[pid]/fd/N 符号链接:readlink 时合成 */
  PROCFS_MAGIC,    /* /proc/self 魔幻链接 */
};

struct procfs_node {
  char name[32];                 /* "meminfo"/"[pid]"/"status"... 对齐 sysfs_node.name[32] */
  enum procfs_node_kind kind;
  bool is_dir;
  struct procfs_node *parent, *children, *sibling;  /* 内存树,仿 sysfs */
  struct procfs_attr *attr;      /* 文件:属性;目录:NULL */
  struct inode *ip;              /* 关联 inode,lookup 时按需建 */
  uint32_t ino;
};
```

**全局状态**（仿 `sysfs.c:24-26`）：

```c
static struct procfs_node *procfs_root;
static spinlock procfs_lock = SPINLOCK_INIT;     /* 保护静态树结构 */
static uint32_t procfs_ino_counter = 0x20000;     /* 静态全局节点 ino 段 */
```

**全局静态节点**（`procfs_init` 预建）：

```
/proc
├── meminfo      (PROCFS_STATIC, show=meminfo_show)
├── cpuinfo      (PROCFS_STATIC, show=cpuinfo_show)
├── uptime       (PROCFS_STATIC, show=uptime_show)
├── version      (PROCFS_STATIC, show=version_show)
├── self         (PROCFS_MAGIC,  lnk → /proc/[current_pid])
└── [pid]/       (PROCFS_PIDDIR, lookup 时合成,见 3.2)
```

**inode 私有数据约定（关键）**：`struct inode` 只有单个 `void *i_priv`（`inode.h:80`），无第二槽。per-pid 文件需同时携带 (a) 是哪个 attr、(b) 哪个 pid。**方案：pid 编码进 ino**（`ino = 0x21000 + pid*2048 + attr_index`），`i_priv` 只存指向共享 `static const procfs_attr` 表项的指针；`show` 时从 ino 反解 `pid = (ino-0x21000)/2048`、`attr_index = (ino-0x21000)%2048`。零 per-lookup 分配，pid 复用时缓存 inode 的 `i_priv` 不变而内容自然刷新。详见 3.6。

### 3.2 per-pid 子树的惰性合成

**核心思路**：`pid == tasks[] 数组下标`（`xtask.h:231`），因此 `/proc/` 目录的 getdents 直接扫 `tasks[]`，`/proc/[pid]` 的 lookup 直接 `tasks[pid]` 取进程，**无需预建任何 per-pid 节点**。

#### 3.2.1 `/proc/` getdents（列进程）

```c
/* 伪代码:procfs_root_getdents */
spin_lock(&procfs_lock);            /* 保护静态节点遍历 */
emit(".", "..");                     /* 合成 dot entries,仿 sysfs.c:257-272 */
for (n = procfs_root->children; n; n = n->sibling)
    dir_emit(n->name, ...);          /* meminfo/cpuinfo/uptime/version/self */
spin_unlock(&procfs_lock);

spin_lock(&tasks_lock);             /* sched.c:73 */
for (int pid = 0; pid < MAX_PROC; pid++) {      /* MAX_PROC=1024, xtask.h:45 */
    xtask *t = tasks[pid];
    if (!t) continue;                           /* NULL 槽:从未用 */
    if (t->state == ZOMBIE || t->state == REAPING) continue;  /* 见下方存活判据 */
    dir_emit(itoa(pid), DT_DIR, synth_piddir_ino(pid));  /* ino=0x21000+pid*2048 */
}
spin_unlock(&tasks_lock);
```

**关键点**：
- `dir_emit`（`mount.c:192-211`）是**纯内核 buffer 操作**：写 `ctx->buf`（内核侧 `dirent64` 缓冲）、更新 `ctx->written`/`ctx->pos`、返回 bool。**无 `copy_to_user`、无锁获取、无页错误、无分配**。因此持 `tasks_lock` 期间调 `dir_emit` 安全；真正的 `copy_to_user` 在 `sys_getdents` 里 `m->fs->getdents` 返回之后（`vfs.c:1468` 之后），已在 `tasks_lock` 外。对齐 `mount.md` 设计决策 8。
- **存活判据用 `state`，不是 `pid<0`**：`sched_task_reap` 在**无锁**下先置 `proc->mm = NULL`（`sched.c:826`）并 `refcount_dec_and_test(&mm->m_count)`（`sched.c:827`），**之后**才在 `tasks_lock` 下置 `proc->pid = -1`（`sched.c:882`）和 `proc->state = REAPING`（`sched.c:883`）。因此 `tasks[pid]->pid < 0` 会漏判"已 reap 但 pid 尚未置 -1"的窗口。正确判据是 `state != ZOMBIE && state != REAPING`（且 `tasks[pid] != NULL`；REAPING 槽的 `tasks[i]` 仍非 NULL 指向死 xtask，直到 `xtask_alloc` 回收）。
- **竞态容忍**：getdents 快照期间进程可能退出，被列出的 pid 在后续 lookup 时可能已无效——lookup 必须重新校验（见 3.2.2），无效返回 `-ENOENT`。这是 procfs 的标准语义，Linux 同理。

#### 3.2.2 `/proc/[pid]` lookup（合成 pid 目录）

`procfs_dir_iop.lookup` 收到 `"123"` 时：

```c
/* 伪代码:procfs_root_lookup(dir, name) */
if (strcmp(name, "self") == 0)  return magic_self_inode();   /* 魔幻,见 3.4 */
if (is_static_global(name))     return static_node_inode(name);  /* meminfo/... */

pid = parse_int(name);                                       /* 非数字→-ENOENT */
if (pid < 0 || pid >= MAX_PROC)  return -ENOENT;

/* 校验有效:tasks_lock 下查 state,不取长期引用,只把 pid 数值编进 ino */
spin_lock(&tasks_lock);
xtask *t = tasks[pid];
if (!t || t->state == ZOMBIE || t->state == REAPING) {
    spin_unlock(&tasks_lock);
    return -ENOENT;
}
spin_unlock(&tasks_lock);

/* 建/复用 pid 目录 inode,ino=0x21000+pid*2048,i_priv 指向 pid 目录元数据 */
return procfs_piddir_iget(pid);
```

**引用安全（关键防呆）**：lookup 返回的 inode 在锁外被使用，期间进程可能被 reap。procfs **不持有任何 xtask/proc 指针，只持有 pid 数值**（编码进 ino），每次 `show`/`getdents` 重新 `tasks_lock` + `tasks[pid]` 取最新进程并校验 `state`。若已 reap，`show` 返回 `-ENOENT`/空。inode 本身可长期缓存（pid 可复用，但 inode 哈希按 ino 去重，pid 复用时内容自然更新）。**从根本上杜绝 UAF**——唯一例外是 `mm`，见 3.3。

#### 3.2.3 `/proc/[pid]/` 子节点（静态模板）

pid 目录的子节点是固定模板（每个进程都一样），在 `procfs_piddir_iop.lookup` 里用一张静态表按名字分发：

```c
static const struct procfs_attr pid_attrs[] = {
  {"status",  status_show},   /* show 的 pid 从 inode->ino 反解 */
  {"stat",    stat_show},
  {"comm",    comm_show},
  {"cmdline", cmdline_show},
  {"maps",    maps_show},
  {"cwd",     NULL},          /* lnk,readlink 合成 */
  {"exe",     NULL},          /* lnk,readlink 合成 */
  {"fd",      NULL},          /* dir,单独处理 */
  {NULL,}
};
```

lookup `"status"` → 找到表项 → 建 `INODE_REGULAR` inode，`ino = 0x21000 + pid*2048 + ATTR_STATUS`，`i_priv = &pid_attrs[ATTR_STATUS]`，`f_op = &procfs_fops`；lookup `"fd"` → 建 `INODE_DIR` inode，`i_op = procfs_fddir_iop`（其 getdents 扫 `files->fd_table`）；lookup `"cwd"`/`"exe"` → 建 `INODE_LNK` inode，`i_op = procfs_lnk_iop`。

### 3.3 文件 read：show 回调与数据来源

`procfs_fops.read` 仿 `sysfs_file_read`（`sysfs.c:354-374`）：

```c
static ssize_t procfs_file_read(xtask *p, file *f, void *buf, size_t count) {
  struct procfs_attr *attr = f->inode->i_priv;        /* 指向共享 const attr 表项 */
  pid_t pid = (pid_t)((f->inode->ino - 0x21000) / 2048);  /* 从 ino 反解 */
  char *kbuf = kmalloc(4096);
  if (!kbuf) return -ENOMEM;
  ssize_t n = attr->show(kbuf, min(count, 4096), pid);
  if (n > 0) copy_to_user(buf, kbuf, n);
  kfree(kbuf);
  return n;
}
```

> inode 只有单个 `i_priv`（`void*`）。pid 不占第二个槽，而是编码进 ino（见 3.1/3.6），`i_priv` 指向共享 `static const procfs_attr`。这与 sysfs 既有约定一致，无需改 inode 结构。

#### 各 show 回调的数据来源（全部现成，只读）

| 文件 | show 输出 | 数据来源（文件:行） |
|------|-----------|----------------------|
| `/proc/[pid]/status` | Name/State/Uid/Gid/PPid/Sid/Pgid | `xtask.comm`(`xtask.h:211`)、`xtask.state`(`xtask.h:53`)、`proc.uid/euid/suid/gid/egid/sgid`(`proc.h:57-63`)、`proc.signal->parent_pid`(`signal.h:32`)、`proc.sid/pgid`(`proc.h`) |
| `/proc/[pid]/stat` | pid comm state ppid pgrp session tty_nr tpgid flags minflt majflt cutime cstime priority nice num_threads itrealvalue starttime vsize rss rsslim ... (~52 字段) | `xtask.pid`(`xtask.h:52`)/`state`(`:53`)/`cpu_time_ns`(`:110`)、`sched_priority/policy`(`xtask.h:206-207`)、`signal->parent_pid`、`proc.cwd`/`files`/`mm`；**无法映射的字段（starttime/tty_nr/vsize/rss/cutime/cstime/...）填 0**，见 3.3.1 |
| `/proc/[pid]/comm` | comm[16] + `\n` | `xtask.comm`(`xtask.h:211`) |
| `/proc/[pid]/maps` | `vaddr-end prot offset dev inode pathname` 每行 | `mm->mmap_regions` 链(`mm_types.h:74`)，持 `mm->mmap_lock`(`mm_types.h:76`)；详见 3.3.2 |
| `/proc/[pid]/cwd` | readlink → `proc.cwd[256]` | `proc.cwd`(`proc.h:77`) |
| `/proc/[pid]/cmdline` | argv 以 `\0` 分隔 | procfs pinfo 侧表（见 3.5） |
| `/proc/[pid]/exe` | readlink → 可执行文件路径 | procfs pinfo 侧表（见 3.5） |
| `/proc/meminfo` | MemTotal/MemFree/MemAvailable/Slab/... | `kernel_mem_stats`(`slab.c:24`，6 字段: total_pages/used_pages/slab_used_bytes/slab_peak_bytes/kmalloc_calls/kfree_calls)、`total_page_frames`(`alloc.h:76`)、`bfc_free_page_nums()`(`alloc.h:64`) |
| `/proc/cpuinfo` | processor/coreid/model name per CPU | `ncpu`(`arch/x64/smp.c:24`)、`g_madt.apic_ids[4]`(`acpi.h:110`)、`cpu_locals[].tsc_offset`(`arch/x64/smp.h:65`); **brand string 需新增 cpuid 0x80000002-4 读取（无现存 helper，仿 `apic.c:61-66` 两段式 cpuid）** |
| `/proc/uptime` | `seconds.idle\n` | `sched_clock()`(`arch/x64/apic.c:30`) ns→s |
| `/proc/version` | `os2 0.1 (gcc...) #SMP\n` | `KERNEL_VERSION` 宏（新增，`kernel.h`） |

##### 3.3.1 `/proc/[pid]/stat` 字段映射（P1 = 上游 procps `ps aux`/`top`）

上游 procps 从 stat 读 ~52 字段。完整输出格式对齐 Linux `fs/proc/array.c`，但 os2 仅对**有数据源**的字段填真值，其余填 0（procps 对 0 字段容错，不影响 `ps aux` 的 PID/USER/STAT/%CPU/%MEM/TTY/TIME/CMD 列；`START` 依赖 starttime=0 会显示 epoch，列为已知缺口）：

| # | 字段 | os2 来源 | 无源填 |
|---|------|----------|--------|
| 1 | pid | `xtask.pid` | — |
| 2 | comm | `xtask.comm`（用 `()` 包裹） | — |
| 3 | state | `xtask.state` 映射为 `R/S/D/Z/T` | — |
| 4 | ppid | `signal->parent_pid` | — |
| 5 | pgrp | `proc.pgid` | — |
| 6 | session | `proc.sid` | — |
| 7 | tty_nr | （无 tty 设备号反查） | 0 |
| 8 | tpgid | | 0 |
| 9 | flags | | 0 |
| 10-11 | minflt/majflt | | 0 |
| 12-13 | cutime/cstime | （子进程累计时间未维护） | 0 |
| 14-15 | utime/stime | `xtask.cpu_time_ns` 折算（clock ticks） | — |
| 16-17 | priority/nice | `xtask.sched_priority`/静态 nice | — |
| 18 | num_threads | `signal->thread_count` | — |
| 19 | itrealvalue | | 0 |
| 20 | starttime | （未记录启动时钟） | 0 |
| 21-22 | vsize/rss | （可从 `mm->mmap_regions` 求和，本期可填 0） | 0 |
| 23 | rsslim | | 0 |
| 24+ | 其余 | | 0 |

> 决策依据：P1 目标是上游 procps `ps aux`/`top` 能解析不崩、关键字段（PID/STAT/%CPU/TTY/TIME/CMD）对齐；非关键字段填 0 是 Linux procfs 对无数据源的常规处理。

##### 3.3.2 `/proc/[pid]/maps` 实现要点（最复杂的 show，含 UAF 防护）

```c
static ssize_t maps_show(char *buf, size_t len, pid_t pid) {
  mm *m;
  /* 1. tasks_lock 下校验 + 取 mm 引用(手动 refcount_inc,无 mm_get) */
  spin_lock(&tasks_lock);
  xtask *t = tasks[pid];
  if (!t || t->state == ZOMBIE || t->state == REAPING || !t->mm) {
    spin_unlock(&tasks_lock);
    return -ENOENT;
  }
  m = t->mm;
  refcount_inc(&m->m_count);                 /* 仿 proc.c:1090 唯一现存额外引用点 */
  spin_unlock(&tasks_lock);

  /* 2. 持 mmap_lock 遍历 VMA(execve 已在该锁下换 mmap_regions,见 3.3.3) */
  uint64_t flags;
  spin_lock_irqsave(&m->mmap_lock, &flags);
  for (mmap_region *mr = m->mmap_regions; mr; mr = mr->next) {
    append(buf, "%lx-%lx %c%c%c%c %08lx %02d:%02d %lu %s\n",
           mr->vaddr, mr->vaddr + mr->size,
           prot_char(mr->prot), ..., mr->offset, 0, 0, mr->ino, "");
  }
  spin_unlock_irqrestore(&m->mmap_lock, flags);

  /* 3. 放 mm 引用(mm_put 存在,proc.c:734) */
  mm_put(m);
  return strlen(buf);
}
```

**三个关键事实（修文档原有错误）**：

1. **`task_get_locked` 不存在**：`task_get(pid)`（`xtask.h:260`）是裸 `return tasks[pid]`，无锁、无校验。maps_show 必须自行在 `tasks_lock` 下做完整校验，不能依赖一个不存在的"加锁版"helper。
2. **`mm_get` 不存在**：只有 `mm_put`（`proc.c:734`）。取 mm 引用须手动 `refcount_inc(&m->m_count)`，仿 `sys_clone` CLONE_VM 在 `proc.c:1090` 的唯一现存额外引用点。放引用用 `mm_put(m)`（`refcount_dec_and_test` 归零则 `mm_release` 内联 free，`proc.c:731`）。
3. **存活判据是 `state` + `mm!=NULL`，不是 `pid<0`**：`sched_task_reap` 无锁下先置 `proc->mm=NULL`（`sched.c:826`）再在 `tasks_lock` 下置 `pid=-1`（`sched.c:882`）。`tasks_lock` 下必须同时校验 `state != ZOMBIE/REAPING` **和** `t->mm != NULL`，二者缺一都会在"已 free mm 但 pid 未置 -1"的窗口 UAF。

**锁序**：`tasks_lock`（校验 + `refcount_inc`）→ 释放 → `mmap_lock`（遍历 VMA）。两把锁**不嵌套**，与既有 `vma.c` 调用约定一致（`vma.h:12` 注释"Callers must hold mm->mmap_lock"，不要求 tasks_lock）。`mmap_region.inode`（`mm_types.h:63`，字段名是 `inode` 非 `ino`）持 `inode_get` 引用（注释 `mm_types.h:58-62`），持 `mmap_lock` 期间安全读。

##### 3.3.3 前置修复：execve 换 mmap_regions 须持 mmap_lock

**procfs 暴露的既有隐患**：`sys_execve`（`proc.c:1379`）在 `proc.c:1824-1846` 换 `proc->mm->cr3` 与 `proc->mm->mmap_regions` 时**不持 `mm->mmap_lock`**：

```c
// proc.c:1824-1846 现状(无锁):
uint64_t old_cr3 = proc->mm->cr3;
mmap_region *old_regions = proc->mm->mmap_regions;
proc->mm->mmap_regions = NULL;      // ← 另一核 maps_show 可能读到 NULL
proc->mm->cr3 = pml4_phys;
...
proc->mm->mmap_regions = stack_region;
```

今天没有任何跨进程读 `mmap_regions` 的代码，所以不暴露；procfs maps_show 是**第一个跨进程读者**，会使该窗口产生 NULL/半构建链读。

**修复**：在 `proc.c:1824-1846` 这段 swap 前后加 `spin_lock_irqsave(&proc->mm->mmap_lock, ...)` / `spin_unlock_irqrestore(...)`。这是一处小局部改动，既让 maps_show 的 `mmap_lock` 遍历对 execve 真正 race-free，又顺带关闭既有的跨线程观测隐患（对齐 `vma.h:12` "caller holds mmap_lock" 契约）。**列为 procfs 的前置依赖**，在 M3 之前合入。

### 3.4 魔幻符号链接：self / self/fd/N

#### `/proc/self`

`PROCFS_MAGIC` 节点，`i_op = procfs_lnk_iop`，`readlink` 实现：

```c
static int procfs_self_readlink(inode *ip, char *buf, size_t bufsiz) {
  pid_t pid = current_xtask->pid;            /* proc.h:142, get_cpu_local()->_cur_proc */
  return snprintf(buf, bufsiz, "/proc/%d", pid);  /* 返回长度,POSIX readlink 不写 NUL */
}
```

path_walk 的 `follow_symlink`（`vfs.c:119-145`，`SYMLINK_MAX=40` 于 `vfs.c:111`）检测到绝对 target（`target[0]=='/'`）后**从根重启** `vfs_resolve`（`vfs.c:131-137`），最长前缀匹配重新命中 `/proc` mount，`path_walk` 再走 `[pid]` 目录。中间段 symlink 跟随在 `path_walk:187-193`（`has_more && dir->type==INODE_LNK`）。故 `/proc/self/status` 解析为：`self`(LNK, has_more)→follow→`/proc/[pid]`(DIR)→`status`(末段,原样返回)。

> **注意**：`follow_symlink` 的相对 target 分支（`vfs.c:140-144`）也走 `vfs_resolve`，而 `vfs_resolve` 只匹配以 `/` 开头的 mount 点，**相对 symlink target 实际不支持**（`vfs.c:138-139` 注释承认）。因此 procfs 的所有 readlink 目标（`self`/`cwd`/`exe`/`fd/N`）**必须返回绝对路径**。
> **注意**：`SYMLINK_MAX` 是 per-`path_walk` 级（`follow_symlink` 对绝对 target 递归重入 `path_walk`，新 `path_walk` 在 `vfs.c:154` 重置 `sym_depth=0`），非全局 40。自引用链靠栈深终止而非 ELOOP。对 `/proc/self` 正常用法无关；procfs 勿引入自引用绝对 symlink。

`sys_readlink`（`do_readlinkat` `syscall.c:3229-3267`）不跟随末段 LNK，直接调 `i_op->readlink`，POSIX 正确。

#### `/proc/self/fd/N` 与 `/proc/[pid]/fd/N`

`/proc/[pid]/fd/` 是 `PROCFS_PIDDIR` 下的特殊目录，`i_op = procfs_fddir_iop`：

- **getdents**：`tasks_lock` 下取 `tasks[pid]->proc->files` 并校验存活（同 3.2.2 判据），随后持 `files->fd_lock`（`types.h:121`），扫 `fd_table[0..MAX_FD]`（`MAX_FD=1024`，`types.h:32`），非 NULL 槽 emit `"N"`（`DT_LNK`）。
- **lookup `"N"`**：解析 fd → 持 `fd_lock` 取 `file *f = fd_lookup(files, fd)`（RCU，`types.h:156-158`），NULL 则 `-ENOENT`；建 `INODE_LNK` inode，`ino = 0x300000 + pid*MAX_FD + fd`（独立段，避开 per-pid attr 段 `0x21000–0x211000`），`i_priv` 存 `(pid, fd)`（此处需两值，用一个 `procfs_fdref{pid,fd}` 小结构或编进 ino；readlink 时用 `fd_lookup` 重新校验 fd 仍有效），`i_op->readlink` 按 `f->type`（`types.h:40-55` 的 `FD_*` `#define`）合成目标——见 3.4.1。

readlink 实现仿 `tmpfs_readlink`（`tmpfs.c:337`）。

##### 3.4.1 fd-N readlink 目标合成（按 file->type）

`FD_*` 是 `#define`（`types.h:40-55`），非 enum。完整集：FD_NONE=0/PIPE=1/REGULAR=2/DEV=3/DIR=4/SOCKET=5/SHM=6/FILE=7/TTY=8/EPOLL=9/EVENTFD=10/TIMERFD=11/SIGNALFD=12/NETLINK=13/IPC=14/SYNC_FILE=15。**`FD_PTY` 不存在**——PTY 用 `FD_TTY`，`file` 联合的 `struct pty *pty` 成员在 `types.h:102`。

readlink 目标按 type 分发（**P0 完整 + P1 尽力 anon_inode 兜底**）：

| file->type | readlink 目标 | 说明 |
|------------|---------------|------|
| `FD_TTY` 且 `f->pty != NULL` | `/dev/pts%d`（`pty->index`，`pty.h:138`） | **P0 核心**：musl `ttyname_r` 读此路径后做 `stat`/`fstat` dev+ino 交叉校验，`/dev/ptsN` 必须解析到 devtmpfs 真实节点且 (dev,ino) 命中 tty fd 的 fstat |
| `FD_TTY` 且 `f->pty == NULL`（串口） | `/dev/ttyS0` 兜底 | 串口 tty 无 pty 结构 |
| `FD_PIPE` | `pipe:[ino]` | 从 `file->inode->ino` |
| `FD_SOCKET` | `socket:[ino]` | 从 `file->inode->ino` |
| `FD_REGULAR`/`FD_FILE` | `anon_inode:[regular]` | **无路径存储**（`struct inode` 不存路径），返回 anon_inode 字符串而非真路径；procps `ls -l /proc/*/fd/*` 对磁盘文件显示该串，不崩。真路径反查需动 inode/VFS，超出 procfs 范围，列 TODO |
| `FD_SHM` | `anon_inode:[shm]` | |
| `FD_EPOLL` | `anon_inode:[eventpoll]` | |
| `FD_EVENTFD` | `anon_inode:[eventfd]` | |
| `FD_TIMERFD` | `anon_inode:[timerfd]` | |
| `FD_SIGNALFD` | `anon_inode:[signalfd]` | |
| `FD_NETLINK` | `anon_inode:[netlink]` | |
| `FD_IPC` | `anon_inode:[ipc]` | |
| `FD_SYNC_FILE` | `anon_inode:[sync_file]` | |
| 其它/`FD_DEV`/`FD_DIR` | `anon_inode:[unknown]` | |

readlink 时须重新 `fd_lookup(files, fd)` 校验 fd 仍有效（fd 可能已 close），NULL 则 `-ENOENT`。

### 3.5 缺口字段的侧表存储（exe/cmdline/environ/root）

**决策**：不扩 `struct proc`（536 字节 STATIC_ASSERT + 驱动字节镜像），改用 **procfs 自有侧表**，按 pid 索引。

```c
/* kernel/bsd/procfs.c */
struct procfs_pinfo {
  spinlock lock;            /* 自有锁,保护下面字符串指针的原子换 */
  char *exe;                /* /init, /bin/sh ... kmalloc 路径,可空 */
  char *cmdline;            /* argv 拼接(\0 分隔),可空 */
  char *environ;            /* envp 拼接,可空(可选,本期可不做) */
  char *root;               /* 通常 "/",chroot 后变 */
  /* 字符串自带 refcount:见下文同步策略 */
};
static struct procfs_pinfo *pinfo_table[MAX_PROC];  /* tasks_lock 保护表槽 */
```

**同步策略（关键，修文档原有错误）**：procfs_pinfo **自带 spinlock + 字符串引用计数**，**与 `tasks[]`/`proc_reap` 的锁上下文解耦**。

原因：`proc_reap`（`proc.c:150`）在**无锁**下被调用（`sched_task_reap` step 6，`sched.c:849-850`），而此时 `tasks[pid]` 仍非空、`pid` 仍==target、`state` 仍 ZOMBIE（`pid=-1`/`REAPING` 在之后 step 7 `sched.c:861-883` 才置）。所以文档原方案"show 取 pinfo 前 `tasks_lock` 确认 `tasks[pid]` 仍有效"**防不住 clear**——clear 发生在锁外，且进程看起来还活着。`tasks_lock` 只能作 `-ENOENT` 过滤，**不是**同步原语。

正确同步：

- **写（execve 覆盖）**：`procfs_pinfo_set` 在 `procfs_pinfo.lock` 下，kmalloc 新字符串、`refcount_set=1`、原子换指针、放锁；旧字符串的释放交给"最后一个读者 `refcount_dec_and_test`"。
- **写（reap 清理）**：`procfs_pinfo_clear` 在 `procfs_pinfo.lock` 下置指针 NULL 并 `refcount_dec_and_test`（若有读者持有引用则不立即 free，等读者放完归零再 free）。
- **读（show）**：`procfs_pinfo.lock` 下拷贝字符串指针到本地、`refcount_inc`、放锁；读字符串内容；`refcount_dec`（归零则 free）。读期间不持任何全局锁。

**写入点（hook）**：
- `sys_execve`（`proc.c:1379`）：成功后 `procfs_pinfo_set(pid, exe=argv[0], cmdline=argv, environ=envp)`。**hook 点在 `argv_strings` kfree 之前**（`proc.c:1765` 附近；`argv_strings` 在 `proc.c:1595` 由 `strncpy_from_user` 拷入，`proc.c:1765` kfree）。
- `proc_reap`（`proc.c:150`）：末尾 `kfree(bp)` 之前 `procfs_pinfo_clear(pid)`。
- **首进程 init**：`process_create_elf`（`proc_create.c`）的 init argv 是内核硬编码 `/init`（`proc_create.c:117` 附近的 `argv_strings` 分配），可单独 `procfs_pinfo_set(0, "/init", ...)` 或复用 execve 路径。

**为什么用侧表而非扩结构体**：
1. `struct proc` 布局被 `STATIC_ASSERT`（`proc.h:106`）和驱动镜像（`bsd_types.h:149`）双重锁定，扩字段要同步改两处，回归面大。
2. exe/cmdline/environ 是 procfs 专属需求，塞进通用 `proc` 是泄漏关注点。
3. 侧表按 pid 索引，与 `tasks[]` 同构，查找 O(1)。

**风险**：execve 频繁时侧表字符串 kmalloc/kfree 有开销。exe/cmdline 必须在 execve 时抓（运行期栈已释放），故强制 execve 填充；environ 可惰性（本期可不做，留 TODO）。

### 3.6 ino 分配与 inode 生命周期

三段，确定性合成，无冲突，可从 ino 反解 pid/attr/fd：

| 段 | 公式 | 用途 | 反解 |
|----|------|------|------|
| 静态全局 | `procfs_ino_counter++`（起点 `0x20000`） | meminfo/cpuinfo/uptime/version/self 等 | `i_priv` 指向 `procfs_node`，无需反解 |
| per-pid 目录 | `0x21000 + pid*2048` | `/proc/[pid]` | `pid = (ino-0x21000)/2048` |
| per-pid 文件 | `0x21000 + pid*2048 + attr_index`（attr_index < 64，预留 2048 槽） | `/proc/[pid]/{status,stat,comm,cmdline,maps,cwd,exe}` | `pid=(ino-0x21000)/2048`, `attr=(ino-0x21000)%2048` |
| fd 链接 | `0x300000 + pid*MAX_FD + fd`（MAX_FD=1024） | `/proc/[pid]/fd/N` | `pid=(ino-0x300000)/1024`, `fd=(ino-0x300000)%1024` |

- pid < 1024 × 2048 = 0x800000，但 per-pid 段实际范围是 `0x21000 + pid*2048`，pid=1023 时 = `0x211000`，**超过** `0x30000`。故 fd 段不能取 `0x30000`，必须放在 per-pid 段之上，取 `0x300000 + pid*MAX_FD + fd`（`0x300000` 远大于 `0x211000`，无冲突）。
- 三段互不重叠：静态 `0x20000–0x20FFF`（16 位够全局节点）、per-pid `0x21000–0x211000`、fd `0x300000+`。

inode 生命周期完全复用既有 inode hash（`inode.c`）：`inode_get_or_create`（`inode.c:101`）按 ino 查 hash，命中则 `refcount_inc` 返回，未命中则 kmalloc + 入 hash；`inode_put`（`inode.c:187`）`refcount_dec_and_test` 归零则从 hash 摘除 + `page_cache_invalidate_inode` + `kfree`。procfs **不自管 inode 生命周期**——打开 fd 持 inode 引用，close 放引用，pid 复用时缓存 inode 按 ino 命中、内容自然刷新，**无驱逐策略需求**（fd 生命周期天然 bound 住 inode 数）。

### 3.7 日志与调试

procfs 是被调试对象，也是调试工具的数据源。自身调试：

- **串口 printk**：`procfs_init` 挂载成功打一行 `[procfs] mounted /proc`（对齐既有"单字符进度链"克制风格，`doc/design/debug.md`）。show 回调内部**不打日志**（高频路径）。
- **ASSERT**：`procfs_node_to_inode` 用 `ASSERT(n)` 校验非空；lookup 返回前 `ASSERT(inode->i_op != NULL)`。
- **死锁检测**：procfs 严格遵循既有锁序（见 3.8），不引入新锁依赖边。`tasks_lock` → `mmap_lock` 不嵌套（取 mm 引用后放 `tasks_lock` 再取 `mmap_lock`）；`tasks_lock` → `files->fd_lock` 不嵌套；`pinfo->lock` 不嵌套任何全局锁。
- **用户态验证**：`ps`/`cat /proc/meminfo`/`ls -l /proc/self/fd/` 直接跑，串口看输出。GDB 可在 `procfs_file_read` 下断点观察 show 回调。

### 3.8 异常容错

| 外部输入 | 校验 | 失败行为 |
|----------|------|----------|
| pid（来自路径） | `0 <= pid < MAX_PROC` 且 `tasks[pid]` 非 NULL 且 `state != ZOMBIE/REAPING`（maps 另校 `mm!=NULL`） | `-ENOENT` |
| pid 复用竞态 | show 内重新 `tasks_lock` 校验 `tasks[pid]->state` 仍活 | 返回空/`-ENOENT` |
| fd（来自路径） | `0 <= fd < MAX_FD` 且 `fd_lookup` 非 NULL | `-ENOENT` |
| read count | `min(count, 4096)`，4KB kbuf | 超长截断 |
| maps 遍历 | `mm` 已 `refcount_inc`（手动）+ 持 `mmap_lock`，VMA 链只读 | 进程退出则 mm 引用保护，遍历后 `mm_put` |
| 路径注入 | lookup name 长度 ≤ 31（`procfs_node.name[32]`），纯数字校验 pid | 非法返回 `-ENOENT`/`-ENAMETOOLONG` |
| kbuf 分配失败 | `kmalloc` 返回 NULL | `-ENOMEM` |
| pinfo 字符串 | `pinfo->lock` + 字符串 `refcount_inc` 后再读 | clear 并发时读者持有的引用阻止 free |

**mm 引用安全（maps_show 关键）**：取 `xtask->mm` 后必须手动 `refcount_inc(&mm->m_count)` 再放 `tasks_lock`，否则进程退出 free mm 导致 UAF。遍历完 `mm_put`。这是 procfs 唯一需要主动管理引用的地方，其余靠"只存 pid 数值"或"pinfo 字符串引用计数"规避。

---

## 4 接口设计

### 4.1 新增系统调用

**无**。procfs 完全复用现有 `open`/`read`/`getdents`/`readlink`/`close`/`stat`，不新增 syscall。

### 4.2 ioctl / 命令号

**无**。procfs 不暴露 ioctl。

### 4.3 模块内部函数接口

```c
/* kernel/bsd/procfs.h */

/* fstype 注册入口(供 vfs_init 调用) */
extern struct fstype procfs_fstype;
extern const struct file_operations procfs_fops;
struct procfs_node *procfs_root_node(void);   /* 返回 fs_data */
void procfs_init(void);                        /* 预建全局静态节点 */

/* 进程生命周期 hook(供 proc.c 调用) */
void procfs_pinfo_set(pid_t pid, const char *exe,
                      char *const argv[], char *const envp[]);
void procfs_pinfo_clear(pid_t pid);

/* inode_operations(内部) */
extern const struct inode_operations procfs_dir_iop;    /* /proc 根 + pid 目录 */
extern const struct inode_operations procfs_file_iop;   /* pid 属性文件 */
extern const struct inode_operations procfs_lnk_iop;    /* 魔幻 + fd 链接 */
extern const struct inode_operations procfs_fddir_iop;  /* /proc/[pid]/fd 目录 */
```

**入参合法性检查约定**：
- 所有 `show`/`lookup`/`getdents` 入口第一件事是 pid/fd 边界校验，越界直接返回错误码，不 panic。
- `procfs_pinfo_set` 内部 kmalloc 失败时静默跳过（procfs 缺口字段为"尽力提供"，不阻塞 execve）。

---

## 5 风险与稳定性方案

### 5.1 常见崩溃场景

| 场景 | 触发 | 防护 |
|------|------|------|
| **UAF（进程 reap 后访问）** | show 持有 xtask/mm 指针期间进程退出 | 只存 pid 数值；mm 手动 `refcount_inc`；show 内重新校验 `tasks[pid]->state` |
| **pid 复用错配** | getdents 列出 pid=A，lookup 时 pid 槽已被 A reap 并分配给 B | lookup 校验 `state` 仍活；可选比对 comm 防错配 |
| **持锁遍历死锁** | getdents 持 `tasks_lock` 时回调取下级锁 | 锁内只 `dir_emit`（纯内核 buffer，`mount.c:192`），不分配 inode/不取引用/不 `copy_to_user`（对齐 `mount.md` 决策 8） |
| **maps 遍历 UAF** | VMA 链被并发 munmap/execve 修改 | mm 已 `refcount_inc` + 持 `mmap_lock`；execve 换 `mmap_regions` 已修为持 `mmap_lock`（3.3.3） |
| **fd 错配** | `/proc/[pid]/fd/N` 指向已 close 的 fd | readlink 时重新 `fd_lookup`，NULL 则 `-ENOENT` |
| **字符串溢出** | comm/cmdline 超 kbuf | comm 固定 16B；cmdline 从 pinfo 读，4KB 截断 |
| **pinfo 读清竞态** | show 读 pinfo 字符串时 `proc_reap` 并发 clear | pinfo 自带 `spinlock` + 字符串 `refcount`；读者 `refcount_inc` 后放锁，clear 的 `refcount_dec_and_test` 等读者放完再 free |

### 5.2 防呆设计（外部输入全部校验）

- 路径段长度 ≤ 31（`procfs_node.name[32]`），超长 `-ENAMETOOLONG`。
- pid 解析：非数字、超 `MAX_PROC`、`tasks[pid]==NULL`、`state==ZOMBIE/REAPING` 一律 `-ENOENT`。
- fd 解析：非数字、超 `MAX_FD`、`fd_lookup==NULL` 一律 `-ENOENT`。
- read `count`：截断到 4KB，`offset` 支持续读（show 全量生成后按 offset 切片返回，仿 sysfs 单页模型）。

### 5.3 卡死诊断

- **getdents 卡死**：大概率 `tasks_lock` 与下级锁死锁。检查 show/getdents 是否在持 `tasks_lock` 时调用了 `kmalloc`/`inode_get`/`copy_to_user`（这些可能触发页错误取锁）。规则：**`tasks_lock` 锁内只允许 `dir_emit` 到内核 ctx buffer**（`dir_emit` 已证实是纯内核操作，`mount.c:192-211`），`copy_to_user` 在锁外（`sys_getdents` 已如此，`vfs.c:1468` 之后）。
- **maps 卡死**：`mmap_lock` 与 `tasks_lock` 嵌套。确认 maps_show 先放 `tasks_lock` 再取 `mmap_lock`；确认 execve 的 mmap_lock 修复（3.3.3）未引入反向嵌套。
- **GDB 定位**：`break procfs_file_read` / `procfs_root_getdents`，观察持锁状态。watchdog（`doc/design/debug.md`）会报长时间持锁。

### 5.4 测试方案

- **QEMU 自动化**：`build.sh --test` 框架新增 `user/test/test_procfs.c`（仿 `test_sysfs.c`），用 Unity 断言：
  - `open("/proc/self/status")` 成功且 read 含 "State:"
  - `readlink("/proc/self/exe")` 含路径
  - `opendir("/proc")` 后 readdir 含数字 pid
  - `stat("/proc/1/maps")` 为 regular file
  - `readlink("/proc/self/fd/0")` 指向 tty 路径（musl ttyname 闭环）
  - `cat /proc/[pid]/stat` 字段数 = ~52（procps 可解析）
- **procps 验收**：在 OS 上跑 `ps aux`、`free`、`cat /proc/cpuinfo`，对比字段正确性。
- **musl 验收**：放弃 `unistd.cmake` 对 `ttyname_r` 的 REMOVE，跑 musl 自带 `ttyname` 测试用例，确认 `/proc/self/fd/N` 链接 + stat/fstat dev+ino 交叉校验通过。
- **GDB + tmux**：`doc/design/debug.md` 的 tmux 自动化脚本，断点 `procfs_file_read` 观察实时调用。
- **竞态压测**：并发 fork/exit + `ps` 循环，串口看有无 UAF/死锁告警；专门压 maps（并发 execve + `cat /proc/[pid]/maps`）验证 3.3.3 修复。

---

## 6 开发里程碑与迭代计划

| 阶段 | 内容 | 验收 | 优先级 |
|------|------|------|--------|
| **M0 前置修复** | execve 换 `mmap_regions` 持 `mmap_lock`（`proc.c:1824-1846`） | 既有测试不回归；maps 遍历对 execve race-free | M3 前置 |
| **M1 框架接入** | procfs.c/h 骨架 + fstype 注册 + 挂载 + 全局静态节点（meminfo/uptime/version） + `sys_open`/`sys_openat` f_op 接线（各 +1 "procfs" 分支） | `cat /proc/meminfo` 有输出 | — |
| **M2 进程列表** | `/proc/` getdents 扫 tasks[] + `/proc/[pid]` lookup 合成 + `/proc/self` | `ls /proc` 见 pid 列表 | — |
| **M3 per-pid 只读** | status/stat(~52字段)/comm/maps/cwd（全现成数据 + M0 修复） | `cat /proc/1/maps` 正确、`ps aux` 可解析 | — |
| **M4 魔幻链接** | `/proc/self/fd/N` + `/proc/[pid]/fd/N` readlink + ttyname 闭环 | musl `ttyname_r` 通过 | P0 闭环 |
| **M5 缺口字段** | pinfo 侧表（spinlock + 字符串引用计数）+ execve/reap hook + exe/cmdline | `ps aux` 全字段、`readlink /proc/self/exe` | — |
| **M6 cpuinfo + 调优** | cpuid 0x80000002-4 brand string 读取（新 helper）+ cpuinfo 完整 + 边界压测 | `cat /proc/cpuinfo` 对齐 Linux 格式 | — |
| **后续 TODO** | `/proc/[pid]/task/[tid]`（线程级，解锁 `pthread_*name_np`）、`/proc/sys/`（可写）、`/proc/[pid]/environ`、`/proc/loadavg`、seq_file（>页 属性）、FD_REGULAR 真路径反查（动 inode/VFS） | 登记入 `doc/design/todo.md` | — |

每阶段独立可验收，M4 达成即解锁 musl gcc 里程碑前置依赖。

---

## 7 软硬件环境与工具链

| 工具 | 用途 |
|------|------|
| QEMU | 运行内核 + disk.img |
| GDB `target remote localhost:1234` | 断点 `procfs_file_read` / `procfs_root_getdents`，观察 show 回调与持锁 |
| `addr2line -e build/myos.elf` | 解析栈回溯（`build.sh -d`） |
| tmux（`debug.md` 脚本） | QEMU + serial(socat) + GDB 三会话并行，串口发 `cat /proc/...` 实时观察 |
| `log.txt` | 串口输出自动落盘，grep `[procfs]` 看挂载日志 |
| Unity test 框架 | `user/test/test_procfs.c` 自动化断言 |

**调试会话恢复**：tmux 会话持久化 + GDB `save breakpoints`/`source .gdbinit` 断点持久化，网络中断后 `tmux attach` 恢复，GDB 重连即可续调（对齐 `doc/design/debug.md`）。

---

## 附录 A：关键文件清单

| 文件 | 角色 | 改动 |
|------|------|------|
| `kernel/bsd/procfs.c`（新） | procfs 全部实现（含 pinfo 侧表） | 新增 ~700 行 |
| `kernel/bsd/procfs.h`（新） | 对外接口 | 新增 ~70 行 |
| `kernel/bsd/vfs.c` | `vfs_init` 挂载 + `sys_open`/`sys_openat` f_op 接线（各 +1 "procfs" 分支，仿 `vfs.c:576-586`） | +12 行 |
| `kernel/bsd/mount.c/.h` | fstype 注册框架 | 不改（复用） |
| `kernel/bsd/sysfs.c/.h` | 文本文件模板 | 不改（参考） |
| `kernel/bsd/tmpfs.c` | symlink/readlink 模板（`tmpfs.c:337`） | 不改（参考） |
| `kernel/kernel.h` | `KERNEL_VERSION` 宏 | +1 行 |
| `kernel/bsd/proc.c` | execve hook（`procfs_pinfo_set`，~`:1765`）+ `proc_reap` hook（`procfs_pinfo_clear`，~`:150`）+ execve mm swap 持 `mmap_lock`（`:1824-1846`） | +2 hook + 1 锁点 |
| `doc/design/todo.md` | 登记线程级/sys/environ/FD_REGULAR 真路径/seq_file 等 TODO；更新 `:311`/`:331`/`:343` 的解锁状态 | 更新 |
| `doc/design/procfs.md`（建议归档） | 本方案归档至 `doc/design/` | 移动 |

## 附录 B：锁序与引用规则速查

```
procfs 涉及锁(全部既有,不新增,除 pinfo 自有锁):
  procfs_lock   (procfs 自有,保护静态树)        — 短临界区,不嵌套任何锁
  tasks_lock    (sched.c:73,扫 tasks[])          — 锁内只 dir_emit/取指针+refcount_inc,不放取下级锁
  mm->mmap_lock (mm_types.h:76,遍历 VMA)         — 取 mm 引用后持,不嵌 tasks_lock;execve 换 mmap_regions 亦持此锁
  files->fd_lock(types.h:121,扫 fd_table)        — 不嵌 tasks_lock
  pinfo->lock   (procfs 自有,保护字符串指针)     — 不嵌任何全局锁;字符串带 refcount

存活判据(统一):
  tasks[pid] != NULL && state != ZOMBIE && state != REAPING
  (maps 另加 proc->mm != NULL)
  注意:sched_task_reap 无锁下先置 proc->mm=NULL(sched.c:826)再在 tasks_lock 下置
        pid=-1(sched.c:882),故 pid<0 判据不可用,必须看 state。

引用规则:
  xtask/proc    — 不持有指针,只存 pid 数值(规避 UAF)
  mm            — maps_show 须手动 refcount_inc(&m_count)(仿 proc.c:1090) + mm_put(proc.c:734)
  inode         — 复用全局 hash,inode_get/inode_put(仿 sysfs);fd 生命周期 bound,无驱逐策略
  pinfo 字符串  — pinfo->lock + 字符串 refcount;读 refcount_inc 后放锁,clear refcount_dec_and_test 延迟 free
  file (fd-N)   — readlink 时 fd_lookup 重新校验,不长期持有
```
