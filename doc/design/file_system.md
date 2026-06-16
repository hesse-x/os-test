# 文件系统重构 — LFN + 目录结构 + Shell 改造 + 性能优化

## 概述

重构文件系统目录结构和 fs_driver 能力，已完成目标：
1. FAT32 目录结构对齐 Linux FHS（Filesystem Hierarchy Standard）
2. 支持 VFAT LFN 长文件名读写
3. Shell 支持路径执行（替代 `run` 命令）

启动流程改造（init + 从 FAT32 启动用户态服务）已拆到 [boot.md](boot.md)。

## FAT32 目录结构

```
/                    根目录
├── README           测试用文本文件
├── boot/            启动必需文件
│   ├── bin/         init（boot 改造后内核从裸LBA加载的唯一用户态入口）
│   └── driver/      disk.dev, fs.dev（boot 改造后内核从裸LBA加载的驱动）
├── driver/          用户态驱动（boot 改造后 init/fork 从 FAT32 加载）
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

当前启动流程不变（内核从裸 LBA 加载 6 个进程），FAT32 上已建好目标目录结构并放入文件。`/boot` 下的文件和裸扇区中的内容是同一份 ELF 的两份拷贝。

根目录保留 README 作为 `cat` 测试文件；hello.elf 和 malloc.elf 只放在 `/local/`，根目录不额外存放。

## 性能优化（fs_driver）

### O.1 FAT 扇区缓存

独立于数据簇 cache，固定 4 页（16KB）FAT 扇区缓存（`fat_cache_entry`，`FAT_CACHE_PAGES=4`）。FAT 表挂载后几乎只读，缓存策略简单——读入后基本不驱逐。

- `-s 8`（4KB/簇）下 FAT 表约 15KB，4 页可 100% 缓存
- `fat_read_entry` 先查缓存，miss 时从磁盘读入缓存再返回
- `fat_write_entry` 修改缓存中的 FAT 扇区并写回磁盘（FAT1 + FAT2）
- `fat32_init` 时预填充前 4 页 FAT 扇区

### O.2 数据簇 LRU 缓存

`CACHE_SLOTS` 为 16（16 × 4KB = 64KB），为 readahead 预取簇提供空间，同时支持多个文件缓存共存。

FAT 缓存和数据簇 cache 分离，互不挤占：
- FAT 缓存：随机访问模式，固定 4 页
- 数据簇 cache：遍历模式，16 slot

### O.3 write_dir_entry_at 只写目标扇区

修改一个 32 字节目录项只写目标条目所在扇区（512B），不再写回整个簇（4KB）。

跨扇区边界时最多写 2 个扇区，仍远优于写整个簇。

### O.4 spf32 缓存

`static uint32_t spf32` 在 `fat32_init` 时缓存，消除 `fat_write_entry` 和 `find_free_cluster` 中重复计算 `(data_start_lba - fat_start_lba) / 2`。

### O.5 mkdisk.sh 簇大小 `-s 8`

- 512B/簇 → 4KB/簇，符合 Microsoft FAT32 规范推荐
- 每簇 128 个目录项（vs 原来 16 个），覆盖 255 字符极端 LFN
- FAT 表从 ~120KB 缩减到 ~15KB，FAT 缓存 4 页可 100% 覆盖

## Phase 1: LFN 读取 + 目录结构 + fs_dirent 扩展

### 1.1 fs_dirent 扩展（common/shm.h）

```c
struct fs_dirent {
    char     name[256];  // LFN 长文件名（原 name[28]）
    uint32_t size;       // 文件大小
    uint32_t date;       // 修改日期（FAT32 wrt_date）
    uint32_t time;       // 修改时间（FAT32 wrt_time）
    uint8_t  attr;       // FAT 属性（目录/文件/只读等）
    // 3 bytes padding → sizeof = 272
};
```

每个 dirent 占 272 字节，8176 字节 data 区最多存 30 个目录项。

### 1.2 readdir 分页协议

fs_dirent 扩到 272 字节后，单次 readdir 最多返回 30 个条目。加 offset/count 分页避免大目录截断。

**请求**（file_req）：`readdir_offset`（跳过前 N 个）+ `readdir_count`（最多返回 N 个）。
**响应**：返回实际条目数 `total`。
**shell 端**：循环 `offset=0; offset+=total` 直到 `total < count`。
**fs_driver 端**：纯无状态遍历，跳过前 `offset` 个有效条目，收集到 `count` 个就停。

### 1.3 VFAT LFN 读取（driver/fs_driver.cc）

**LFN 条目格式**：attr=0x0F，每个条目存 13 个 UCS-2 字符，以逆序存储在 8.3 短名条目之前。

**UCS-2 → ASCII 转换**（`collect_lfn_entry`）：高字节为 0、低字节为 ASCII → 直接转为 ASCII 字符；高字节非零 → 丢弃整个 LFN，回退到 8.3 短名。理由：系统不会产生非 ASCII 文件名。

**find_dir_entry 双匹配**：遍历目录项时用 `static char lfn_buf[256]` 收集 LFN 条目片段，遇到短名条目时 LFN + 8.3 双匹配。每次遇到非 LFN 条目（无论是否匹配）时重置 LFN 缓冲区，防止孤儿 LFN 条目污染后续匹配。

**resolve_path 零拷贝**：指针+长度（`comp_start`/`comp_len`），不再用 `char[13]` 栈缓冲区拷贝路径组件。`find_dir_entry` 接受 `(name, name_len)`。

**handle_readdir**：有 LFN 条目返回长文件名，无 LFN 条目格式化 8.3 为可读形式。填充 `time` 字段。支持分页。

### 1.4 mkdisk.sh 目录结构

使用 `mmd` + `mcopy` 创建目录结构并复制文件。`mcopy` 从宿主机写入时自动生成 LFN 条目（保留小写），所有预装文件都有 LFN。

## Phase 2: Shell 改造

### 2.1 路径执行（替代 `run` 命令）

命令分发：匹配内置命令 → 执行；否则当作文件路径执行：
1. `build_abs_path(token)` 得到绝对路径
2. `fs_request(FS_CMD_OPEN, path)`
3. 读取整个文件到 malloc 缓冲区
4. ELF magic 校验（`\x7fELF`）
5. `sys_spawn(elf_buf, file_size, 0)` + `sys_waitpid`
6. `free(elf_buf)`

错误信息：open 失败 → `file not found`；ELF 校验失败 → `not an executable file`。

语义：不带 `./` 前缀也当路径执行（如 `hello.elf` 等同于 `./hello.elf`），不实现 PATH 搜索。同步执行（waitpid 等待子进程退出）。

去掉的命令：`run`、`malloc`、`free`、`mtest`。保留的命令：`r`（裸扇区读取）。

### 2.2 ls 支持路径参数

`ls [path]`：默认列 cwd，指定路径则列目标目录。路径指向普通文件 → 报 `ls: not a directory`。

### 2.3 ls -l Linux 格式

```
drwxr-xr-x 2 root root    0 Jun 14 10:30 boot
-rw-r--r-- 1 root root 4096 Jun 14 10:30 hello.elf
```

字段映射：权限（目录 `drwxr-xr-x`/普通 `-rw-r--r--`/只读 `-r--r--r--`）、硬链接数（目录 2/文件 1）、属主属组固定 `root`、大小 `file_size`、日期时间 `wrt_date + wrt_time` → `Mon DD HH:MM` 格式。

## Phase 3: LFN 写入

### 3.1 写入策略

与 Linux vfat 驱动一致：**始终写 LFN + 8.3 条目**，保留原始大小写。即使文件名是合法 8.3 也写 LFN 条目。

### 3.2 LFN 条目写入（write_lfn_entries）

每个 LFN 条目存 13 个 UCS-2 字符，文件名长度 N 需要 `ceil(N/13)` 个 LFN 条目。写入逆序：最后一个 LFN 条目（seq | 0x40）→ ... → 第一个 LFN 条目（seq=1）→ 8.3 短名条目。填充：文件名结束后 `0x0000`，剩余 `0xFFFF`。

### 3.3 8.3 短名生成（generate_short_name）

- 合法 8.3 → 直接作为短名
- 含小写/特殊字符 → 转大写
- 超 8.3 限制 → 前 6 字符 + `~N`，扩展名取前 3 字符，冲突时 `~N` 递增（用 `find_dir_entry` 查冲突）

### 3.4 checksum 计算

```c
static uint8_t lfn_checksum(const uint8_t *name) {
    uint8_t sum = 0;
    for (int i = 11; i; i--)
        sum = ((sum & 1) ? 0x80 : 0) + (sum >> 1) + *name++;
    return sum;
}
```

### 3.5 find_free_dir_slots 连续槽查找

查找 N 个连续空闲槽位（N = LFN 条目数 + 1）。不跨簇——当前簇不够时跳到下一个簇。遍历完所有现有簇都找不到时才扩展新簇。

`-s 8` 下每簇 128 个目录项，正常文件名（2-3 槽）单簇绰绰有余。

### 3.6 已存在检查

`handle_create`/`handle_mkdir` 使用双匹配 `find_dir_entry` 检查是否已存在，`touch` 重复执行更新时间戳而非创建重复条目。`resolve_parent` 的 `leaf_name` 为 `char[256]`。

### 3.7 find_free_cluster 游标

`static uint32_t next_free_hint`（初始值 2），分配簇时从上次位置继续扫描，扫到末尾回绕。

## Phase 4: 启动性能优化（fs_driver IPC + readahead）

### 4.1 删 stat，fd_table 存 file_size

`spawn_service()` 原先调用 stat 获取 `file_size` 再 open/read/close，stat 和 open 各做一次 `resolve_path()` 目录遍历，完全重复。

删除 `stat()` 调用，libc `open()` 把 `resp->file_size` 存入 `fd_table[fd].file_size`，`spawn_service` 改为 `open → fd_file_size → malloc → read → close → spawn`，4 次 IPC 往返变 3 次。

- `user/lib/file.cc`：`file_fd_entry` 增加 `uint64_t file_size` 字段；`open()` 中存 `fd_table[fd].file_size = resp->file_size`；新增 `fd_file_size(fd)` 函数（声明在 `user/include/fcntl.h`）
- `init/init.c`：`spawn_service` 不再 include `sys/stat.h`，不再调用 `stat()`

### 4.2 数据簇 LRU 缓存增大

`CACHE_SLOTS` 从 8 增到 16（16 × 4KB = 64KB），为 readahead 预取簇提供空间，同时支持多个文件缓存共存。

### 4.3 disk_driver SHM resp 扩容 + 多扇区读

readahead 一次预取 4 簇（16KB），原 disk_driver SHM resp 区域仅 2 页（8KB），无法容纳。

- `disk_driver.cc`：`shm_create` 参数从 5 页改为 8 页（header 1 页 + req 2 页 + resp 5 页 = 20KB resp 区域）
- `common/shm.h`：`disk_resp_shm.data` 从 `data[8180]` 扩为 `data[20472]`
- `fs_driver.cc`：新增 `read_clusters(start_cluster, count)` 函数，一次磁盘请求读取连续多扇区（最多 32 扇区 = 4 簇 = 16KB），已缓存的簇跳过

### 4.4 顺序 readahead

对齐 Linux readahead 模型：检测顺序访问模式，预取后续簇进 LRU 缓存，使后续 read 请求命中缓存而零磁盘 I/O。

**顺序检测**：在 `session_open_file` 增加 readahead 状态：

```c
struct session_open_file {
    bool     used;
    uint32_t start_cluster;
    uint32_t file_size;
    // readahead 状态
    uint64_t ra_prev_offset;   // 上一次 read 的 offset
    uint32_t ra_prev_count;    // 上一次 read 的 count
    bool     ra_sequential;    // 是否检测到顺序模式
};
```

当 `offset == ra_prev_offset + ra_prev_count` 时标记 `ra_sequential = true`，触发预取。

**预取策略**（固定窗口，4 簇）：
1. `handle_read` 正常处理当前请求，填充 reply
2. `msg_resp` 先回复调用方（调用方被唤醒）
3. 检测到顺序模式后，调用 `read_clusters` 预取后续 4 簇进 LRU 缓存
4. 预取完成后调 `recv()` 取下一个请求

**pipeline 效果**：步骤 2 调用方被唤醒后准备下一次 msg 请求，与步骤 3 的磁盘 I/O 时间重叠。后续 read 请求命中 LRU 缓存直接返回，无磁盘 I/O 等待。

**FAT 链预取**：readahead 需沿 FAT 链遍历确定预取簇号，连续簇（next == cur + 1）合并为单次 `read_clusters` 调用，非连续簇拆分多次调用。

## Phase 5: fs_driver 事件循环 + disk_driver DMA

### 设计目标

多客户端低延迟高并发。当前 fs_driver 单线程同步处理所有请求，磁盘 I/O 期间阻塞无法 recv 新请求，多客户端时请求排队等待。

### 5.1 fs_driver 单线程事件循环

**并发模型**：单线程事件循环 + 异步 I/O。fs_driver 不再阻塞在 disk_wait_reply，提交磁盘请求后回到 recv 循环，磁盘完成后通过 RECV_NOTIFY 回调继续处理。

选择单线程事件循环而非多 worker 线程的理由：disk_driver 本身单请求同步，多线程在磁盘端完全串行化，并发收益仅存在于"CPU 处理与磁盘 I/O 重叠"，加锁复杂度不值得。

**初始化阶段保持同步**：启动时的 MBR/BPB/FAT 缓存预填充无客户端，直接用同步磁盘 I/O，事件循环只在主循环启用。

### 5.2 两层抽象：disk_io + pending_op

**底层：disk_io**（I/O 描述，不关心请求语义）：

```c
struct disk_io {
    uint32_t lba;
    uint32_t count;
    void *dest;                     // 数据目标地址（memcpy 到此）
    void (*complete)(disk_io *io);  // 数据已拷完，回调继续
    void *ctx;                      // 回调上下文（指向 pending_op 或 readahead 状态）
    struct disk_io *next;           // 队列链接
};
```

完成路径：`memcpy(io->dest, dresp->data, io->count * 512)` → `io->complete(io)`。dest 字段将"先拷数据再回调"合并为一步，消除 dresp->data 生命周期 footgun。

**上层：pending_op**（客户端请求状态）：

```c
struct pending_op {
    int client_pid;
    int session_idx;
    enum op_type { OP_READ, OP_WRITE, OP_OPEN, OP_READDIR, OP_RAW_READ } type;
    void (*resume)(pending_op *op);  // 磁盘完成后继续处理
    union {
        struct { /* read 上下文 */ } read;
        struct { /* write 上下文 */ } write;
        struct { /* open 上下文 */ } open;
        struct { /* readdir 上下文 */ } readdir;
        struct { /* raw_read 上下文 */ } raw_read;
    } u;
};
```

pending_op 永远有有效 client_pid，readahead 不走 pending_op（走 disk_io），消除特殊 case。

### 5.3 磁盘请求队列

fs_driver 侧维护磁盘 FIFO 队列 + `disk_in_flight` 标志：

- `disk_in_flight == true`：当前有 disk_io 在 disk_driver 中处理
- 新 disk_io 提交：`disk_in_flight == false` 且队列非空 → 直接写 SHM + notify disk_driver；否则入队尾
- complete 回调：清 `disk_in_flight = false`，检查队列，非空则提交新队头

disk_driver 不改动，sleeping flag 机制完全复用。

**FIFO 调度**：先来先服务。readahead 是异步的、小窗口的，实际阻塞其他 client 的概率低。需要时再加分优先级队列。

### 5.4 事件循环主循环

```
while (1) {
    recv(&m, data_buf, sizeof(file_req), 0);

    if (m.type == RECV_MSG) {
        // 客户端文件请求 → 构造 pending_op，尝试同步处理
        // 需要磁盘时：构造 disk_io 入队，挂起 pending_op
        // 缓存命中时：直接完成，msg_resp

    } else if (m.type == RECV_NOTIFY && m.src == disk_driver_pid) {
        // 磁盘完成通知
        disk_io *io = current_disk_io;
        memcpy(io->dest, dresp->data, io->count * 512);
        disk_in_flight = false;
        io->complete(io);
        submit_next_disk_io();

    } else if (m.type == RECV_NOTIFY) {
        // 其他通知（如 write_lock 释放通知），按需处理
    }
}
```

### 5.5 全异步统一框架

所有操作统一走 pending_op + disk_io，无同步/异步两条路径：

| 操作 | 磁盘 I/O | 异步化 |
|------|---------|--------|
| open | 读目录项查文件 | pending_op + disk_io |
| read | 读 FAT + 读数据簇 | pending_op + disk_io |
| readdir | 读目录簇 | pending_op + disk_io |
| raw_read | 读裸扇区 | pending_op + disk_io |
| touch | 分配 FAT + 写 FAT + 写目录 | pending_op + disk_io |
| mkdir | 分配簇 + 写 FAT + 初始化 + 写目录 | pending_op + disk_io |
| close | 无磁盘 | 同步直接处理 |

### 5.6 write 一致性：write_lock + cache pin

**write_lock**：全局锁，write pending_op 在 flight 期间持有。新 write 请求排队等释放。read 不受 write_lock 限制，可以并行。

**cache pin**：LRU 条目增加 `pin_count` 字段，>0 时不允许淘汰。write 修改缓存数据前 pin，写盘完成后 unpin。`cache_alloc` 跳过 `pin_count > 0` 的条目，全 pin 时报错（当前 16 slot 对 1-2 个并发写绰绰有余）。

### 5.7 FAT 链遍历异步化

read 操作的 FAT 链遍历在 resume 回调中自包含：

```c
void resume_read(pending_op *op) {
    while (op->u.read.chain_pos < op->u.read.offset_clusters) {
        // FAT 缓存命中 → continue 循环
        if (fat_cache_hit(fat_lba)) {
            op->u.read.current_cluster = fat_cache_lookup(op->u.read.current_cluster);
            op->u.read.chain_pos++;
            continue;
        }
        // FAT 缓存未命中 → 提交 disk_io，回调是 resume_read 自身
        submit_disk_io(fat_lba, 1, fat_cache_slot, resume_read, op);
        return;  // 挂起，等磁盘
    }
    // FAT 遍历完成，开始读数据
    submit_data_read(op);
}
```

缓存命中时 continue，未命中时 return 挂起，逻辑集中在一个函数里。

### 5.8 readahead 异步化

readahead 不走 pending_op，构造 disk_io 直接入磁盘队列：
- `readahead_pending` 标志防止重复提交（上一次 readahead 还在磁盘队列里时不入新请求）
- FAT 缓存命中时正常提交 disk_io 读数据簇
- FAT 缓存未命中时跳过本次 readahead（FAT 缓存 4 页覆盖全部 FAT，未命中几乎不可能）
- disk_io complete 回调：memcpy 数据到 LRU slot，清 `readahead_pending`，无 msg_resp

### 5.9 disk_wait_reply deferred queue 修复

事件循环下 disk_wait_reply 问题自动消失——fs_driver 不再阻塞在 disk_wait_reply，而是在 recv 循环中统一收消息。磁盘完成通知和其他客户端消息都是普通的 RECV_NOTIFY / RECV_MSG，自然交替处理。

### 5.10 disk_driver DMA 改造（暂缓，需 PCI 子系统）

**前提**：DMA 需要 PCI 配置空间枚举（定位 IDE 控制器 BAR4）、PCI 命令寄存器 enable（I/O Space + Bus Master 位）、以及正确的 BAR 解析。当前内核无 PCI 子系统，用户态 `outl(0xCF8, ...)` 扫描虽然能读配置空间但 BM IDE 寄存器写入不生效（BM_CMD_REG 写 0x01 读回 0x00），原因是 PCI 命令寄存器未 enable Bus Master 位。需先实现内核 PCI 枚举框架。

**实现方案**（PCI 就绪后启用）：

**收益**：QEMU 下 PIO 每扇区需 ~2000 次 I/O 指令模拟（256 × inw + 状态轮询），DMA 是单次批量 memcpy + 中断通知。QEMU 下 DMA 收益比实机更大。

**PCI 配置空间访问**：disk_driver IOPL=3，直接 `outl(0xCF8, ...)` + `inl(0xCFC)` 读 PCI 配置空间 BAR4 获取 BM IDE 基地址，零内核改动。与 Linux 用户态驱动直接 I/O 端口访问一致。

**新增 syscall**（NR_SYSCALL 从 26 增到 28）：

```c
// 分配物理连续、<4GB 的 DMA 缓冲区，映射到调用者地址空间
int sys_dma_alloc(size_t size, void **vaddr, uint64_t *paddr);

// 释放 DMA 缓冲区
int sys_dma_free(void *vaddr);
```

内核侧：`bfc_alloc(n)` 分配连续物理页（约束物理地址 < 4GB）→ `map_user_pages` 映射到用户态 → 返回虚拟地址 + 物理地址。与 Linux `dma_alloc_coherent` 语义一致。

**ATA IRQ 绑定**：复用 `sys_irq_bind(14)`（primary IDE channel），需内核在 I/O APIC 里 unmask IRQ14。

**DMA 数据流**：
1. `sys_dma_alloc(16KB)` 分配 DMA buffer（启动时一次分配，终身复用）
2. 填写 PRDT（指向 DMA buffer 物理地址 + 传输长度）
3. 设置 BM IDE 寄存器，启动 DMA 传输
4. ATA 控制器直接 DMA 到 DMA buffer
5. 传输完成，IRQ 14 → disk_driver 收到 RECV_IRQ
6. disk_driver 从 DMA buffer memcpy 到 `dresp->data`

SHM 协议不变，disk_driver 外部接口不变。DMA 是 disk_driver 内部实现替换。

**DMA buffer 策略**：启动时分配一块 16KB（最大单次 I/O = 4 cluster = 16KB），所有请求复用。

### 5.11 实施顺序

1. **fs_driver 事件循环改造** ✅：disk_io / pending_op / 磁盘队列 / write_lock + cache pin / readahead 异步化 / disk_wait_reply 修复
2. **disk_driver DMA 改造**：暂缓，需先实现内核 PCI 枚举子系统（配置空间读写 + 设备扫描 + BAR 解析 + 命令寄存器 enable）。disk_driver 当前使用纯 PIO 模式

事件循环是架构改造，决定后续所有代码的形状；DMA 是局部替换，改 disk_driver 内部不影响 fs_driver 设计。

## 搁置项

| 项目 | 原因 | 触发条件 |
|------|------|---------|
| readahead 自适应窗口 | 当前文件规模（20-40KB）固定 4 簇窗口已足够 | 大文件顺序读场景 |
| 磁盘队列优先级调度 | FIFO 在当前少量客户端下足够，readahead 窗口小 | 多客户端延迟敏感场景 |
| 回调链 → C++20 协程迁移 | GCC 10 协程 experimental + 栈回溯断裂（协程帧在堆上，addr2line 不可用） | GCC ≥ 12 + 栈回溯支持协程帧 |
| 多线程客户端 session 并发安全 | 当前每客户端同一时刻仅一个 sys_msg 阻塞调用，session 内无并发 | 多线程客户端（需 per-fd 粒度锁） |
| DMA 多请求 slot / buffer pool | 当前单请求同步模型，一块 DMA buffer 够用 | disk_driver 多请求并发 |
