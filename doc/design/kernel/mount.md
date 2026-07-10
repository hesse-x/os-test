# Mount 框架

## 当前架构设计

VFS 路径分发通过 `mount_table` + 最长前缀匹配实现。`/`（FAT32）、`/dev`（devtmpfs）均为挂载点，由内核在 `vfs_init` 阶段内部挂载。路径相关 syscall（open/stat/mkdir/unlink/rmdir/getdents）统一经 `vfs_resolve_user` 解析到 `(mount_entry, relpath)`，再调对应 fstype 回调。fd 打开后的 I/O（read/write/ioctl/close）仍走 `switch(f->type)` 分发，与 mount 框架正交。

源文件：kernel/bsd/mount.c / mount.h

### 设计决策

| # | 决策 | 选择 | 理由 |
|---|------|------|------|
| 1 | 挂载点匹配 | 最长前缀匹配 | `/dev`(mplen=4) 优先于 `/`(mplen=1)，子挂载点不被根抢走；根挂载 `"/"` 天然兜底，无需特例分支 |
| 2 | 路径规范化 | 字符串级 `normalize_path`（split by `/`，`.` 跳过，`..` 弹栈） | 无 symlink 场景下与 Linux 逐组件 `path_walk` 结果等价；引入 symlink 后需重写为 inode 级逐组件遍历 |
| 3 | fstype 无状态 | 回调只描述"怎么做"，挂载实例状态放 `mount_entry.fs_data` | fstype 是静态注册全局对象，不释放；多个挂载实例共享同一 fstype |
| 4 | 目录枚举模型 | `dir_context` + `dir_emit`（对齐 Linux） | fstype 回调只 emit `(name, ino, d_type)`，不碰 `dirent64` 布局；`sys_getdents` 负责 buffer 管理和 offset 写回 |
| 5 | fd-I/O 不改 | 仍走 `switch(f->type)` 分发 | fd 打开后 I/O 与路径无关，mount 框架只负责路径解析到 inode，不引入 `file_operations` 分发 |
| 6 | devtmpfs 设备 fd 创建 | `sys_open` 命中 `/dev` 且 relpath 非空时直调 `devtmpfs_open`（不走 `fs->lookup`） | 设备 fd 需为 FD_DEV 并执行 `ops->open`（ptmx/serial 等），通用 lookup 路径无法满足 |
| 7 | devtmpfs ino | `inode_create(0, ...)`，ino=0 | devtmpfs inode 不参与 FAT32 cluster 编号空间；`stat` 返回的 `st_ino` 当前为 0（待唯一化） |
| 8 | mount_lock 作用域 | 查询侧拷指针后即放锁，回调在锁外执行 | 避免"持锁遍历 + 回调内再取锁"死锁 |
| 9 | SYS_MOUNT 权限 | 不检查 | 当前 OS 无用户/capability 模型，所有进程等权；`flags`/`data` 预留 |
| 10 | FAT32 单实例限制 | 卷几何用文件级 static 全局 | 只挂一个根 FAT32 场景足够；多实例需移到 per-mount `fat32_sb` |

### 核心数据结构

**struct fstype**（kernel/bsd/mount.h : fstype）
  name : const char* — "fat32" / "devtmpfs" / "sysfs"
  lookup : inode* (*)(const char *relpath) — 解析挂载内相对路径，返回 inode（已 inode_get）或 NULL
  getdents : ssize_t (*)(inode *dir, dir_context *ctx) — 目录枚举，逐条 dir_emit
  mkdir / unlink / rmdir : int (*)(const char *relpath) — 目录创建/删除，伪 fs 返回 -ENOSYS
  stat : int (*)(const char *relpath, kstat *ks) — 填充 kstat

**struct mount_entry**（kernel/bsd/mount.h : mount_entry）
  mntpoint : char[64] — 挂载点绝对路径（"/" / "/dev" / "/sys"）
  fs : fstype* — 所属文件系统类型
  fs_data : void* — 挂载私有数据（fat32/devtmpfs 为 NULL）
  in_use : bool

**struct dir_context**（kernel/bsd/mount.h : dir_context）
  pos : uint64_t — 读入 f->offset，写回更新后的游标
  buf : void* — 内核态 buffer
  len : size_t — buffer 容量
  written : size_t — 已写入字节数

**常量**：MAX_MOUNTS=8、MNTPOINT_MAX=64、RELPATH_MAX=256、MAX_FSTYPES=8

**inode 关联**：`struct inode`（kernel/bsd/inode.h : inode）有 `mount : mount_entry*` 字段，由 `sys_open` lookup 命中后填入 `ip->mount = m`。`mount_of_inode(ip)` 优先返回 `ip->mount`，为 NULL 时回退到根挂载（`"/"`）。

### 关键流程

**路径解析** `vfs_resolve_user(upath, relpath, relcap)`（kernel/bsd/mount.c : vfs_resolve_user）：
1. `strncpy_from_user` 拷用户路径到内核 buffer（256B）
2. 校验绝对路径（`kpath[0] == '/'`），否则 -EINVAL
3. `normalize_path` 规范化（去多余 `/`，处理 `.`/`..`，`..` 越出 `/` 裁剪到 `/`）
4. `vfs_resolve` 遍历 `mount_table` 找最长前缀匹配，写出挂载内相对路径（去前导 `/`）

**挂载点穿越** `vfs_resolve(path, relpath, relcap)`：
1. 持 `mount_lock` 遍历 `mount_table`
2. 对每个 `in_use` 条目，匹配条件：`path` 以 `mntpoint` 开头且下字符为 `/` 或 `\0`；根挂载 `"/"` 匹配所有绝对路径
3. 选 mplen 最大者，拷出指针后放锁
4. 写出 relpath（`path + best_len` 去前导 `/`）

**挂载初始化** `vfs_init()`（kernel/bsd/vfs.c : vfs_init）：
1. `mount_init()` 清空 `mount_table`
2. 扫描 AHCI 端口，`fat32_init()` 成功后 `register_fstype(&fat32_fstype)` + `register_fstype(&devtmpfs_fstype)`
3. `mount_internal(&fat32_fstype, "/")` — 内核内部挂载 FAT32 到根
4. `fat32_stat("/dev")` 不存在则 `fat32_mkdir("/dev")` — 在 FAT32 根建真实目录项，使 `getdents("/")` 可见 `dev`
5. `mount_internal(&devtmpfs_fstype, "/dev")` — 内核内部挂载 devtmpfs

**sys_getdents**（kernel/bsd/vfs.c : sys_getdents）：
1. 校验 fd 为 FD_DIR，取 `ip = f->inode`
2. `mount_of_inode(ip)` 获取挂载条目
3. 构造 `dir_context`（`pos = f->offset`、`buf`/`len` 为 kmalloc 的内核 buffer）
4. 调 `m->fs->getdents(ip, &ctx)`
5. `copy_to_user` + 写回 `f->offset = ctx.pos` + 返回 `ctx.written`

**SYS_MOUNT** `sys_mount(source, target, fstype, flags, data)`（kernel/bsd/mount.c : sys_mount）：拷 `target`/`fstype` 用户字符串 → `find_fstype_by_name` → `mount_internal(fs, target)`。`source`/`flags`/`data` 当前预留。

### 锁模型

| 锁 | 类型 | 保护范围 | 获取顺序 |
|----|------|---------|---------|
| mount_lock | spinlock | mount_table 增删查、fstype_table 查 | 独立，回调不持锁 |

`vfs_resolve` 在 `mount_lock` 内遍历 `mount_table`，只拷出 `fs`/`fs_data` 指针后即放锁，回调（lookup/getdents）在锁外执行。fstype 是静态注册全局对象，不释放。

### fstype 实现

**fat32_fstype**（kernel/bsd/fat32.c : fat32_fstype）：
- 6 个回调（lookup/stat/mkdir/unlink/rmdir/getdents）均经 `fat32_prepend_slash` 给 relpath 补前导 `/`，适配 `fat32_resolve_path` 要求 `path[0]=='/'`
- `getdents` 取 `dir->start_cluster` 传给 `fat32_getdents`

**devtmpfs_fstype**（kernel/bsd/devtmpfs.c : devtmpfs_fstype）：
- `lookup`：relpath 空串返回根目录 dir inode；含 `/` 则 split 为 dir + leaf 匹配；无 `/` 平铺扫描
- `getdents`：emit 顶级目录（`dir_list`）+ 顶级设备（`dev_list` 中 name 无 `/` 者）；`ctx->pos != 0` 返回 EOF
- `stat`：空串合成 `/dev` 目录元数据；匹配 `dir_list`/`dev_list` 合成（目录 S_IFDIR|0755，设备 S_IFCHR|0600）
- `mkdir`/`unlink`/`rmdir` 返回 -ENOSYS（devtmpfs 目录由 `devtmpfs_create` 内部建）

### 系统调用

| # | 名称 | 签名 | 行为 |
|---|------|------|------|
| 95 | SYS_MOUNT | `int64_t sys_mount(int64_t source, int64_t target, int64_t fstype, int64_t flags, int64_t data, int64_t unused)` | target 必须绝对路径；fstype 未注册返回 -ENODEV；同 mntpoint 已在用返回 -EBUSY；槽满返回 -ENOMEM。无权限检查 |

用户态 wrapper：`int mount(const char *source, const char *target, const char *fstype, unsigned long flags, const void *data)`（user/lib/sys_mount.cc + user/include/sys/mount.h）

### 路径 syscall 改造点

所有路径 syscall 经 `vfs_resolve_user` → 调 fstype 回调：

| syscall | 位置 | 回调 |
|---|---|---|
| sys_open | kernel/bsd/vfs.c : sys_open | `fs->lookup`（devtmpfs 设备 fd 例外，直调 `devtmpfs_open`） |
| sys_stat | kernel/bsd/vfs.c : sys_stat | `fs->stat` |
| sys_mkdir | kernel/bsd/vfs.c : sys_mkdir | `fs->mkdir` |
| sys_unlink | kernel/bsd/vfs.c : sys_unlink | `fs->unlink` |
| sys_rmdir | kernel/bsd/vfs.c : sys_rmdir | `fs->rmdir` |
| sys_getdents | kernel/bsd/vfs.c : sys_getdents | `mount_of_inode` → `fs->getdents(ip, ctx)` |

`sys_dev_create` 不经路径解析，直接调 `devtmpfs_create`，不走 mount 框架。

### 与其他模块的关系

- **VFS**：mount 框架是 VFS 路径分发的统一入口，替代旧的 `/dev/` 前缀字节比较。详见 [vfs.md](vfs.md)
- **devtmpfs**：从"前缀 hack"迁为挂载到 `/dev` 的 fstype。`devtmpfs_create` 调用点（virtio_gpu、xhci 等）不变，只改访问入口
- **FAT32**：注册为根 fstype，6 个函数加 `fat32_prepend_slash` shim 适配 relpath 语义
- **fd-I/O（syscall.c）**：mount 框架不涉及，fd 打开后仍走 `switch(f->type)` 分发

## 待完成项

| 项目 | 说明 | 优先级 |
|------|------|--------|
| sysfs 挂载占位 | `mount(NULL, "/sys", "sysfs", 0, NULL)` 可成功，内容见 sysfs 方案 | 中 |
| SYS_UMOUNT | `mount_table` 结构已预留 remove 路径，后续可加 umount2 syscall | 低 |
| devtmpfs ino 唯一化 | 当前所有 devtmpfs inode 用 `inode_create(0, ...)`，ino 全为 0，导致 `inode_lookup` hash 冲突且 `st_ino=0`（测试断言 `st_ino > 0` 失败）。需递增分配器（如 `0x80000000` 起，避开 FAT32 cluster 范围） | 高 |
| devtmpfs getdents 子目录枚举 | 当前 `devtmpfs_getdents` 只 emit 顶级目录和顶级设备，`opendir("/dev/dri")` 无法枚举 `card0`。需按 `dir + "/"` 前缀过滤直接子条目 | 中 |
| VFS 逐组件 path_walk | 当前 `normalize_path` 字符串级处理 `.`/`..`，无 symlink 场景下与 Linux 逐组件 `..` 穿越结果等价。引入 symlink 后需重写为 inode 级逐组件遍历 + `follow_dotdot` 穿越挂载边界，同时可引入 dentry cache | 低 |
| dentry cache | 每次 open 读磁盘解析路径，添加 `(path, inode)` hash + LRU 缓存 | 中 |
| mount 权限检查 | 当前无用户/capability 模型，`sys_mount` 不检查权限。引入权限体系时在入口加门控，ABI 不变 | 低 |
| FAT32 多实例 | 卷几何用文件级 static 全局，只能挂一个 FAT32 实例。多实例需移到 per-mount `struct fat32_sb` | 低 |
