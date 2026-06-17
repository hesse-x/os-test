# 文件系统设计 — fs_driver 异步事件循环 + FAT32 实现

## 概述

FAT32 文件系统作为独立用户态进程（fs_driver）运行，通过 `sys_msg`/`sys_msg_resp` 与客户端进程通信，通过 `sys_block_read`/`sys_block_write` 直接访问内核 AHCI 驱动。支持目录遍历、文件读写、创建文件/目录，VFAT LFN 长文件名读写。

fs_driver 采用单线程事件循环 + 异步 I/O 架构：磁盘 I/O 通过 `sys_block_async` 提交，完成后内核发送 RECV_NOTIFY 回调继续处理，不阻塞 recv 循环。

## FAT32 目录结构

```
/                    根目录
├── README           测试用文本文件
├── driver/          用户态驱动
│   ├── kbd.dev
│   └── kms.dev
├── usr/
│   ├── bin/         用户态可执行文件
│   │   ├── terminal
│   │   └── shell
│   └── lib/         库文件
│       └── libc.a
└── local/           用户程序
    ├── hello.elf
    └── malloc.elf
```

## 性能优化

### FAT 扇区缓存

独立于数据簇 cache，固定 4 页（16KB）FAT 扇区缓存（`fat_cache_entry`，`FAT_CACHE_PAGES=4`）。FAT 表挂载后几乎只读，缓存策略简单——读入后基本不驱逐。

- `-s 8`（4KB/簇）下 FAT 表约 15KB，4 页可 100% 缓存
- `fat_read_entry` 先查缓存，miss 时从磁盘读入缓存再返回
- FAT 写入走 FAT dual-write（写 FAT1 + FAT2），同时更新缓存
- `fat32_init` 时预填充前 4 页 FAT 扇区

### 数据簇 LRU 缓存

`CACHE_SLOTS` 为 16（16 × 4KB = 64KB），为 readahead 预取簇提供空间，同时支持多个文件缓存共存。

FAT 缓存和数据簇 cache 分离，互不挤占：
- FAT 缓存：随机访问模式，固定 4 页
- 数据簇 cache：遍历模式，16 slot，带 pin_count 防止写期间淘汰

### write_dir_entry_at 只写目标扇区

修改一个 32 字节目录项只写目标条目所在扇区（512B），不再写回整个簇（4KB）。跨扇区边界时最多写 2 个扇区。

### spf32 缓存

`static uint32_t spf32` 在 `fat32_init` 时缓存，消除 `fat_write_entry` 和 `find_free_cluster` 中重复计算 `(data_start_lba - fat_start_lba) / 2`。

### mkdisk.sh 簇大小 `-s 8`

- 512B/簇 → 4KB/簇，符合 Microsoft FAT32 规范推荐
- 每簇 128 个目录项（vs 原来 16 个），覆盖 255 字符极端 LFN
- FAT 表从 ~120KB 缩减到 ~15KB，FAT 缓存 4 页可 100% 覆盖

## 消息协议

fs_driver 与客户端通过 `sys_msg`/`sys_msg_resp` 通信（变长 IPC，≤64KB）。

| cmd | 名称 | 请求字段 | 回复字段 |
|-----|------|---------|---------|
| 1 | open | path, flags | status, fd, file_size |
| 2 | read | fs_fd, count | status, count, data[] |
| 3 | write | fs_fd, count, data[] | status, count, file_size |
| 4 | close | fs_fd | status |
| 5 | readdir | path, readdir_offset, readdir_count | status, total, count, data[] |
| 6 | create | path | status |
| 7 | mkdir | path | status |
| 8 | raw_read | lba, count | status, count, data[] |
| 9 | stat | path | status, file_size |
| 10 | ping | — | status |

## fs_driver 多客户端 session

```c
#define MAX_CLIENTS  16
#define MAX_SESSION_FDS 8

struct session_open_file {
    bool     used;
    uint32_t start_cluster;
    uint32_t file_size;
    uint64_t offset;
    uint32_t dir_start_cluster;
    int      dir_entry_index;
    bool     dir_entry_valid;
    uint32_t flags;               // O_WRONLY, O_RDWR, O_APPEND
    uint64_t ra_prev_offset;
    uint32_t ra_prev_count;
    bool     ra_sequential;
};

struct client_session {
    pid_t client_pid;
    struct session_open_file open_files[MAX_SESSION_FDS];
};
```

## 异步事件循环架构

### 设计目标

多客户端低延迟高并发。fs_driver 单线程同步处理所有请求时，磁盘 I/O 期间阻塞无法 recv 新请求，多客户端时请求排队等待。

### 并发模型

单线程事件循环 + 异步 I/O。fs_driver 不再阻塞等待磁盘响应，提交磁盘请求后回到 recv 循环，磁盘完成后通过 RECV_NOTIFY 回调继续处理。

选择单线程事件循环而非多 worker 线程的理由：AHCI 驱动本身单请求同步，多线程在磁盘端完全串行化，并发收益仅存在于"CPU 处理与磁盘 I/O 重叠"，加锁复杂度不值得。

**初始化阶段保持同步**：启动时的 MBR/BPB/FAT 缓存预填充无客户端，直接用同步磁盘 I/O（`disk_read_sync`），事件循环只在主循环启用。

### 两层抽象：disk_io + pending_op

**底层：disk_io**（I/O 描述，不关心请求语义）：

```c
struct disk_io {
    uint32_t lba;
    uint32_t count;
    void *buf;                     // read: 目标缓冲区; write: 源缓冲区
    uint8_t  dir;                  // 0=read, 1=write
    void (*complete)(disk_io *io); // 完成回调
    void *ctx;                     // 回调上下文（通常指向 pending_op）
    uint32_t cookie;               // async 完成匹配 cookie
    disk_io *next;
};
```

提交：`sys_block_async(lba, buf, count, dir)` → 返回 cookie。完成：RECV_NOTIFY 携带 cookie → `find_pending_op_by_cookie` → `io->complete(io)`。

**上层：pending_op**（客户端请求状态）：

```c
enum op_type { OP_READ, OP_WRITE, OP_OPEN, OP_READDIR, OP_RAW_READ, OP_CREATE, OP_MKDIR, OP_STAT };

struct pending_op {
    int client_pid;
    int session_idx;
    op_type type;
    void (*resume)(pending_op *op);  // 磁盘完成后继续处理
    disk_io io;                      // 嵌入 disk_io
    bool io_active;
    union {
        struct { /* read */ } read;
        struct { /* open + stat 共享 resolve_state */ } open;
        struct { /* readdir */ } readdir;
        struct { /* raw_read */ } raw_read;
        create_dir_context create_dir;
        struct { /* write */ } write;
    } u;
    uint8_t reply_buf[sizeof(file_resp) + 65536];
};
```

pending_op 池 `MAX_PENDING_OPS=16`，readahead 不走 pending_op（走独立 disk_io），消除特殊 case。

### 事件循环主循环

```
while (1) {
    recv(&m, data_buf, sizeof(file_req) + 65536, 0);

    if (m.type == RECV_NOTIFY) {
        // 解析 AHCI 完成数据: cookie(4) + result(4) + lba(4) + count(4)
        // 1. readahead 完成 → 清标志，continue
        // 2. pending_op 完成 → io_active=false, io.complete(&io)

    } else if (m.type == RECV_MSG) {
        // 客户端文件请求 → 分配 pending_op → start_xxx()
        // 缓存命中时直接完成 msg_resp
        // 缓存未命中时提交 disk_io，挂起 pending_op
    }
}
```

### 全异步统一框架

所有操作统一走 pending_op + disk_io，无同步/异步两条路径：

| 操作 | 磁盘 I/O | 异步化 |
|------|---------|--------|
| open/stat | 读目录簇查文件（resolve_state） | pending_op + disk_io |
| read | 读 FAT + 读数据簇 | pending_op + disk_io |
| readdir | 解析路径 + 读目录簇 | pending_op + disk_io |
| raw_read | 读裸扇区 | pending_op + disk_io |
| write | FAT 链遍历 + 簇分配 + 数据写入 + 目录项更新 | pending_op + disk_io |
| create/mkdir | 路径解析 + 簇分配 + FAT 写入 + 目录项写入 | pending_op + disk_io |
| close/ping | 无磁盘 | 同步直接处理 |

## resolve_state：可复用异步路径解析

所有需要路径解析的操作（open/stat/readdir/create/mkdir）共享 `resolve_state` 子状态机：

```c
#define RS_INIT          0  // 提取路径组件，初始化扫描
#define RS_READ_CLUSTER  1  // 异步读目录簇（cache miss）
#define RS_SCAN_ENTRIES  2  // 扫描目录项（纯计算，无阻塞）
#define RS_READ_FAT      3  // 异步读 FAT（cache miss，链遍历）
#define RS_DONE          4

struct resolve_state {
    const char *path;
    int path_pos, comp_start, comp_len;
    uint32_t dir_cluster, current_cluster;
    int entry_idx;
    char lfn_buf[256];
    bool found;
    fat_dir_entry result;
    uint32_t result_cluster;
    int result_entry_idx;
    bool is_parent;        // true=解析父目录，提取 leaf_name
    char leaf_name[256];
    int leaf_len;
    int phase;
};
```

`resolve_step(rs, op)` 驱动状态机：缓存命中时在同一轮循环中继续，未命中时提交 disk_io 并返回 RESOLVE_ASYNC。`is_parent=true` 时解析到父目录并提取 leaf_name（用于 create/mkdir）。

**`..` 处理**：`..` 作为普通目录名在 FAT32 目录项中查找，找到后 descend 到其 `fst_clus_hi/lo` 指向的簇。shell 负责 `build_abs_path` 构建含 `..` 的绝对路径。

**根目录特殊处理**：路径为 `/` 时直接设置 `result_entry_idx=-1` 标记，READDIR 遇到此标记直接进入 `resume_readdir(root_cluster)`。

## write 一致性：write_lock + cache pin

**write_lock**：全局锁，write/create/mkdir pending_op 在 flight 期间持有。新写请求排队等释放（`write_wait_queue`，FIFO 8 项）。read 不受 write_lock 限制，可以并行。

**cache pin**：LRU 条目增加 `pin_count` 字段，>0 时不允许淘汰。write 修改缓存数据前 pin，写盘完成后 unpin。`cache_alloc` 跳过 `pin_count > 0` 的条目，全 pin 时报错。

## FAT dual-write（原子性）

FAT 写入必须同时写 FAT1 和 FAT2（`fat_start_lba` 和 `fat_start_lba + spf32`），使用 `fat_write_state` 子状态机：

1. Phase 0: 读 FAT 扇区到 sector_buf（cache hit 时直接拷贝）
2. Phase 1: 修改 entry，写 FAT1（`sys_block_async` dir=1）
3. Phase 2: 写 FAT2（`sys_block_async` dir=1）
4. Phase 3: 更新 fat_cache 从 sector_buf，完成

## write 状态机

write 操作是最复杂的状态机， phases：

| Phase | 说明 |
|-------|------|
| WPHASE_ACQUIRE_LOCK | 获取 write_lock（可能排队） |
| WPHASE_LOCATE | FAT 链遍历到目标偏移簇 |
| WPHASE_EXTEND_ALLOC | 链不够长 → 分配新簇（`allocate_cluster_async`） |
| WPHASE_EXTEND_ZERO | 零填充新簇（`zero_fill_cluster_async`） |
| WPHASE_EXTEND_LINK | FAT dual-write 链接尾簇 → 新簇 |
| WPHASE_WRITE_DATA | 读簇 → 修改 → 写回扇区 |
| WPHASE_WRITE_NEXT | 检查是否更多数据/簇需写入 |
| WPHASE_UPDATE_DIR | 更新目录项（文件大小 + 时间戳 + 首簇号） |
| WPHASE_DONE | 释放 write_lock，回复，free |

## create/mkdir 状态机

create 和 mkdir 共享 `create_dir_context` 状态机：

| Phase | 说明 |
|-------|------|
| CD_RESOLVE_PATH | 完整路径解析（检查目标是否存在） |
| CD_RESOLVE_PARENT | 父目录解析（is_parent=true，提取 leaf_name） |
| CD_ALLOCATE | mkdir: 分配新簇 |
| CD_ALLOCATE_WAIT | 等待 FAT dual-write 完成 |
| CD_INIT_DIR | mkdir: 初始化 `.`/`..` 条目，写簇 |
| CD_FIND_SLOTS | 查找连续空闲目录槽（`find_slots_state`） |
| CD_WRITE_LFN | 写 LFN 条目（0~N 个） |
| CD_WRITE_SHORT | 写 8.3 短名条目 |
| CD_UPDATE_TIMESTAMP | touch 已存在文件：更新时间戳 |
| CD_DONE | 释放 write_lock，回复 |

短名生成（`gen_short_name`）带异步冲突检查：合法 8.3 直接用，否则前 6 字符 + `~N`，冲突时递增 N。

## find_slots_state：目录槽查找 + 链扩展

查找 N 个连续空闲槽位（N = LFN 条目数 + 1），不跨簇。遍历完所有现有簇都找不到时扩展新簇（allocate → zero-fill → link）。

dir chain tail 缓存（`dir_tail_cache`，64 项）加速扩展时查找链尾。

## readahead

对齐 Linux readahead 模型：检测顺序访问模式，预取后续簇进 LRU 缓存。

**顺序检测**：`session_open_file` 中 `ra_prev_offset`/`ra_prev_count` 跟踪上次读偏移，`offset == ra_prev_offset + ra_prev_count` 时标记 `ra_sequential`。

**预取策略**：固定 4 簇窗口。read 正常处理并 `msg_resp` 回复后，提交 readahead disk_io（独立于 pending_op，`readahead_pending` 防重入）。

## Shell 命令

| 命令 | 用途 |
|------|------|
| `ls [-l] [path]` | 列目录（支持路径参数，`-l` 显示权限/大小/日期） |
| `cat <path>` | 读文件打印内容 |
| `cd <path>` | 切换目录 |
| `pwd` | 打印当前工作目录 |
| `touch <path>` | 创建空文件 / 更新已存在文件时间戳 |
| `mkdir <path>` | 创建目录 |
| `echo TEXT > FILE` | 写文本到文件 |
| `r LBA [COUNT]` | 裸扇区读（调试用） |
| `<path>` | 路径执行 ELF 文件 |
| `h` | 帮助 |

路径执行：未匹配内置命令时当作文件路径 → `open` → `read` 整个文件 → ELF 校验 → `sys_spawn` + `sys_waitpid`。

**cwd 路径管理**：shell 维护 `static char cwd[256]`（初始 `/`），所有命令通过 `build_abs_path` 将相对路径转为绝对路径传给 fs_driver。`cd` 验证目标为有效目录后更新 cwd，尾随 `/` 自动去除。

## 限制

- 时间戳硬编码（无 RTC）
- 删除（unlink/rmdir）未实现
- 不支持硬链接/符号链接
- 不支持文件截断（truncate）

## 搁置项

| 项目 | 原因 | 触发条件 |
|------|------|---------|
| readahead 自适应窗口 | 当前文件规模（20-40KB）固定 4 簇窗口已足够 | 大文件顺序读场景 |
| 回调链 → C++20 协程迁移 | GCC 10 协程 experimental + 栈回溯断裂（协程帧在堆上，addr2line 不可用） | GCC ≥ 12 + 栈回溯支持协程帧 |
| 多线程客户端 session 并发安全 | 当前每客户端同一时刻仅一个 sys_msg 阻塞调用，session 内无并发 | 多线程客户端（需 per-fd 粒度锁） |
