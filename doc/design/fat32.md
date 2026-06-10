# FAT32 文件系统 — 实现记录

## 概述

为微内核 OS 添加 FAT32 文件系统支持。遵循微内核原则，文件系统作为独立用户态进程（fs_driver）运行，通过共享页 IPC 与 shell 交互，内部通过 disk_driver 访问磁盘。当前支持只读操作（readdir/open/read/close）和写入操作（touch/mkdir）。

## 磁盘布局

```
disk.img (1MB, 2048 扇区)
├── LBA 0:       MBR 分区表（sfdisk 创建）
├── LBA 1-50:    disk_driver.elf（50 扇区/25KB）
├── LBA 51-100:  kbd_driver.elf（50 扇区/25KB）
├── LBA 101-150: shell.elf（50 扇区/25KB）
├── LBA 151-200: fs_driver.elf（50 扇区/25KB）
└── LBA 201-2047: FAT32 分区（4KB 簇，MBR 分区2 指向，type=0x0C）
```

- MBR 分区表包含两个分区：
  - 分区1: LBA 1-200，裸数据（4 个 ELF 二进制），type=0xDA
  - 分区2: LBA 201-2047，FAT32 文件系统，type=0x0C
- 内核动态加载 ELF：先读 1 扇区 ELF 头，解析 program headers 计算实际大小，再读剩余扇区（最多 50 扇区/25KB）

## 进程架构

```
Shell (PID 4) ←→ fs_driver (PID 5) ←→ disk_driver (PID 2)
   fs_req/fs_resp      disk_req/disk_resp
```

- fs_driver 是文件系统中间层，向上提供文件级 IPC，向下封装块设备 IPC
- shell 不再直接与 disk_driver 通信，所有磁盘请求（包括调试用的裸扇区读）通过 fs_driver 中转
- PID 分配：idle0=0, disk_driver=2, kbd_driver=3, shell=4, fs_driver=5

## 共享页布局

```
0x500000  kbd_shm        (1页)
0x501000  disk_req_shm   (2页, 0x501000-0x502FFF)
0x503000  disk_resp_shm  (2页, 0x503000-0x504FFF)
0x505000  fs_req_shm     (1页)
0x506000  fs_resp_shm    (2页, 0x506000-0x507FFF)
```

- disk_req_shm 扩到 2 页，数据区可容纳 15 个扇区（7.5KB），支持写入完整扇区
- disk_resp_shm 扩到 2 页，数据区可容纳 15 个扇区（7.5KB），满足 FAT32 4KB 簇单次读取
- 所有进程统一映射全部 8 个共享页（0x500000-0x507FFF）
- shell 不使用 disk_req/disk_resp 指针，改用 fs_req/fs_resp

## IPC 协议

### fs_req_shm 结构（1 页, 0x505000）

```c
struct fs_req_shm {
    uint32_t cmd;          // 0=readdir, 1=open, 2=read, 3=close, 4=raw_read
    uint32_t client_pid;   // 请求方 PID，fs_driver 用它 sys_notify 回来
    char     path[256];    // 绝对路径
    uint32_t fd;           // 文件描述符，read/close/raw_read 用
    uint32_t offset;       // 读偏移，read 用
    uint32_t count;        // 读字节数，read 用
    uint32_t lba;          // LBA 地址，raw_read 用
};
```

### fs_resp_shm 结构（2 页, 0x506000）

```c
struct fs_resp_shm {
    uint32_t status;       // 0=成功, 非零=错误
    uint32_t fd;           // open 返回的文件描述符
    uint32_t count;        // 实际读取/返回的字节数
    uint32_t total;        // 文件总大小（open）或目录项数（readdir）
    uint8_t  data[8176];   // 数据区（8192 - 16 字节头部）
};
```

### 目录项结构（readdir 返回）

```c
struct fs_dirent {
    char     name[28];     // 文件名（8.3 短名）
    uint32_t size;         // 文件大小
    uint32_t date;         // 修改日期
    uint8_t  attr;         // FAT 属性（目录/文件/只读等）
};
```

- 每个目录项 40 字节（含对齐），8KB 数据区可容纳约 200 个条目

### 命令语义

| cmd | 名称 | 请求字段 | 响应字段 |
|-----|------|---------|---------|
| 0 | readdir | cmd, client_pid, path | status, total(条目数), data(目录项数组) |
| 1 | open | cmd, client_pid, path | status, fd, total(文件大小) |
| 2 | read | cmd, client_pid, fd, offset, count | status, count(实际字节数), data |
| 3 | close | cmd, client_pid, fd | status |
| 4 | raw_read | cmd, client_pid, lba, count | status, count(字节数), data |
| 5 | create | cmd, client_pid, path | status |
| 6 | mkdir | cmd, client_pid, path | status |

### IPC 流程

```
Shell (PID 4)                      fs_driver (PID 5)                 disk_driver (PID 2)
    |                                     |                                 |
    | 1. 写 fs_req_shm                    |                                 |
    | 2. sys_notify(fs_driver_pid) -----> |                                 |
    |                                     | 3. 读 fs_req_shm                |
    |                                     | 4. 解析 FAT32 路径/簇链         |
    |                                     | 5. 写 disk_req_shm              |
    |                                     | 6. sys_notify(disk_driver_pid)->|
    |                                     |                                 | 7. ATA PIO 读
    |                                     | <---- sys_notify(fs_driver_pid) |
    |                                     | 8. 读 disk_resp_shm             |
    |                                     | 9. 写 fs_resp_shm               |
    | <---- sys_notify(shell_pid)         |                                 |
    | 10. 读 fs_resp_shm                  |                                 |
    |                                     |                                 |
    | 11. sys_wait()                      | 12. sys_wait()                  | 13. sys_wait()
```

## Shell 命令

| 命令 | 用途 |
|------|------|
| `ls` | 列目录 |
| `ls -l` | 长格式列目录（权限/uid/gid 用占位值） |
| `cat <path>` | 读文件打印内容 |
| `cd <path>` | 切换目录（shell 维护 cwd，拼绝对路径给 fs_driver） |
| `pwd` | 打印当前工作目录（本地操作，无 IPC） |
| `touch <path>` | 创建空文件 / 更新已存在文件时间戳 |
| `mkdir <path>` | 创建目录 |
| `r LBA [COUNT]` | 裸扇区读（调试用，走 fs_driver→disk_driver 透传） |
| `h` | 帮助 |

### ls -l 输出格式

```
-rw-r--r--  0 root root 18 Jan 01 00:00 HELLO.TXT
-rw-r--r--  0 root root 34 Jan 01 00:00 README
```

- 权限位：目录用 `drwxr-xr-x`，文件用 `-rw-r--r--`（占位，无实际意义）
- uid/gid：固定 `root root`（占位）
- 大小、日期：从 FAT32 目录项读取

### 路径处理

- shell 维护 `cwd` 字符串，初始为 `/`
- `cd <path>`：若是绝对路径则直接设置 cwd，若是相对路径则拼接
- `ls`/`cat` 发送前将路径拼成绝对路径写入 fs_req_shm
- fs_driver 只处理绝对路径，从根目录开始逐级遍历

## fs_driver 内部设计

### FAT32 只读解析

1. 读 MBR（LBA 0），解析分区表，找到 FAT32 分区起始 LBA（type=0x0B/0x0C）
2. 读 BPB（分区起始扇区），解析卷参数（簇大小、FAT 起始、数据区起始等）
3. BPB 校验：`bps!=512 || sectors_per_cluster==0 || spf32==0 || root_cluster<2` 时打印 "EBPB" 错误并挂起（防止 disk_driver 未就绪时 BPB 全零导致除零异常）
4. 路径解析：从根目录开始，逐级查找目录项
5. 文件读取：沿 FAT 簇链读取数据

### 限制

- 不支持文件内容写入（write cmd），仅支持创建空文件和目录
- 只支持 8.3 短文件名，LFN 条目跳过
- 单客户端（shell）
- 时间戳硬编码（无 RTC）

### LRU 缓存

- 2 簇 LRU 缓存，数据簇专用（FAT 表读取直接走 disk_resp_shm）
- 每个缓存条目：簇号 + 4KB 数据 + 年龄标记
- 命中时跳过 disk_driver IPC，减少上下文切换开销

### 打开文件表

- 单客户端，维护一张打开文件表（最多 8 个 fd）
- 每条记录：used 标志、起始簇号、文件大小
- `open`：遍历目录找到文件，分配 fd，返回起始簇号和文件大小
- `read(fd, offset, count)`：从起始簇沿 FAT 链定位到 offset 对应的簇，读取 count 字节
- `close(fd)`：释放 fd

## disk_driver 改动

- `shell_pid` 改为 `fs_driver_pid`（= my_pid + 3）
- disk_resp_shm 扩到 2 页（数据区 ~8KB，可容纳 15 个扇区）
- disk_driver 不再通知 shell，改为通知 fs_driver
- 新增 ATA PIO WRITE cmd（cmd=1）

## kbd_driver 改动

- 支持 Shift（左右，scancode 0x2A/0x36）和 CapsLock（0x3A）产生大小写
- 状态追踪：`shift_pressed`（bool，make/break 切换）、`capslock_on`（bool，make 时 toggle）
- 字母键逻辑：`shift XOR capslock` → 大写，否则小写（Shift+CapsLock 产生小写，符合标准）
- 非字母键（数字行、符号键）：Shift 时使用 `scancode_shifted` 表（如 `1→!`、`[→{`）
- Shell 命令匹配大小写敏感（`my_strcmp`），路径大小写不敏感（FAT32 8.3 格式存储为大写，`format_83_name` 将输入转大写后匹配）

## 内核改动

### kernel_main ELF 加载

- 动态 ELF 加载：先读 1 扇区 ELF 头，解析 e_phnum/e_phoff 计算实际文件大小，再读剩余扇区
- ELF_MAX_SECTORS=50（25KB/ELF），按实际大小读取，避免加载不完整 ELF 导致 page fault
- LBA 偏移更新：disk_driver=1, kbd_driver=51, shell=101, fs_driver=151

### shm_init 扩展

- 新增 5 个物理页：disk_req_shm_phys2、disk_resp_shm_phys(2)、fs_req_shm_phys、fs_resp_shm_phys(2)
- map_shared_pages 从 3 页扩展到 8 页映射
- 所有用户进程统一映射全部共享页

## 构建改动（build.sh）

### disk.img 构建

1. `dd` 创建 1MB 零填充镜像
2. `dd` 写 4 个 ELF 到 LBA 1/51/101/151
3. `sfdisk` 创建 MBR 分区表（分区1: LBA 1-200 type=0xDA，分区2: LBA 201-2047 type=0x0C）
4. `dd` 提取 FAT32 分区区域 → `mkfs.fat -F 32 -s 8` 格式化（4KB 簇）
5. `mcopy` 写入测试文件（hello.txt、README）
6. `dd` 写回 FAT32 分区到 disk.img

### fs_driver.elf 编译

- `g++ -m64 -ffreestanding -nostdlib -fno-builtin -fno-pie -fno-stack-protector -I. -c driver/fs_driver.cc`
- `objcopy --remove-section .note.gnu.property`
- `ld -m elf_x86_64 -Ttext 0x400000`

### 依赖工具

- `sfdisk`（util-linux）— MBR 分区表操作
- `dosfstools`（mkfs.fat）— FAT32 格式化
- `mtools`（mcopy）— FAT32 文件拷贝（无需 root）

## 变更文件清单

| 文件 | 操作 | 说明 |
|------|------|------|
| `common/shm.h` | 修改 | 地址常量更新（8页布局）；disk_req_shm 扩到 2 页；disk_resp_shm 扩到 2 页（地址后移）；新增 FS_CMD_CREATE=5, FS_CMD_MKDIR=6 |
| `kernel/proc.cc` | 修改 | shm_init 多分配物理页；map_shared_pages 改为 8 页映射；地址常量更新 |
| `kernel/kernel.cc` | 修改 | 动态 ELF 加载（load_elf_from_disk）；LBA 更新（1/51/101/151）；ELF_MAX_SECTORS=50 |
| `driver/kbd_driver.cc` | 修改 | 新增 Shift/CapsLock 状态追踪 + shifted scancode 表，支持大小写输入 |
| `driver/fs_driver.cc` | 修改 | 新增 handle_create/handle_mkdir；新增 find_free_cluster/allocate_cluster/dir_chain_extend；BPB 校验防除零；静态全局缓冲区防栈溢出 |
| `shell/shell.cc` | 修改 | 重构为表驱动命令解析；新增 cmd_pwd/cmd_touch/cmd_mkdir |
| `arch/x64/utils.h` | 修改 | 新增 outw() 内联函数 |
| `build.sh` | 修改 | ELF slot 50扇区/25KB；LBA 1/51/101/151；FAT32 从 LBA 201 起 |

## FAT32 写入支持 — pwd/touch/mkdir 设计

从只读文件系统升级为支持写入。新增三个 shell 命令（pwd/touch/mkdir），打通完整写入链路（disk_driver ATA PIO write → fs_driver 目录项创建/簇分配 → shell 命令）。

### 命令语义

| 命令 | 用途 | 说明 |
|------|------|------|
| `pwd` | 打印当前工作目录 | shell 本地操作，直接 `puts(cwd)`，不需要 IPC |
| `touch <path>` | 创建空文件 / 更新时间戳 | 已存在 → 更新 wrt_date/wrt_time；不存在 → 创建空目录项（attr=0x20, fst_clus=0, file_size=0） |
| `mkdir <path>` | 创建目录 | 创建目录项（attr=0x10）+ 分配空闲簇 + 初始化 . 和 .. 条目 |

### 写入链路

```
Shell → fs_req(CREATE/MKDIR) → fs_driver(写目录项+簇分配) → disk_req(WRITE) → disk_driver(ATA PIO写) → 磁盘
```

三个层级各新增功能：
- disk_driver：新增 WRITE cmd（ATA PIO LBA28 write）
- fs_driver：新增 handle_create / handle_mkdir / 簇分配 / 目录簇链扩展
- shell：新增 pwd/touch/mkdir 命令

### 共享页布局（8 页）

disk_req_shm 从 1 页扩到 2 页，地址重排：

```
0x500000  kbd_shm        (1页)
0x501000  disk_req_shm   (2页, 0x501000-0x502FFF) ← 扩到2页，数据区~8KB可写15扇区
0x503000  disk_resp_shm  (2页, 0x503000-0x504FFF) ← 地址后移
0x505000  fs_req_shm     (1页)
0x506000  fs_resp_shm    (2页, 0x506000-0x507FFF) ← 地址后移
```

总计 8 页（0x500000-0x507FFF），内核 shm_init 多分配 1 个物理页，map_shared_pages 改为 8 页映射。

### disk_req_shm 结构（2 页）

```c
struct disk_req_shm {
    uint32_t cmd;        // READ=0, WRITE=1
    uint32_t lba;
    uint32_t count;      // sector count
    uint8_t  data[8180]; // 2 pages - 12 bytes header，写数据放完整扇区
};
```

- WRITE 发送完整扇区数据（fs_driver 先读扇区→本地修改→把完整修改后扇区放进 data 写回）
- disk_driver 只做纯写，不做 read-modify-write（RMW 组合命令留 todo）

### disk_driver ATA PIO WRITE

- cmd=1 时执行 ATA PIO LBA28 write
- 协议：选从盘(0xF0) → 写 LBA/head → 写 sector count → 写命令 0x30 → 等 BSY/DRQ → 写 512 字节到 DATA register → 等 BSY 清除
- 写 FAT1 后立即写 FAT2（镜像冗余），两次 disk_write IPC
- notify 目标不变（fs_driver_pid）

### FS 命令扩展

| cmd | 名称 | 请求字段 | 响应字段 |
|-----|------|---------|---------|
| 5 | create | cmd, client_pid, path | status |
| 6 | mkdir | cmd, client_pid, path | status |

- fs_req_shm / fs_resp_shm 结构不变，CREATE/MKDIR 复用现有 cmd + client_pid + path 字段
- CREATE 和 MKDIR 是独立命令（不做通用 CREATE_ENTRY），未来 mkdir 可自然扩展

### 错误码（扩展现有）

| status | 语义 |
|--------|------|
| 0 | 成功 |
| 1 | 不存在（路径中某组件找不到） |
| 2 | 不是目录（父路径中间组件是文件） |
| 3 | 已存在（touch 已存在文件时更新时间戳不算错误；mkdir 碰到已存在条目返回此码） |
| 4 | 无空闲簇（FAT 表无 entry==0 的簇） |
| 5 | fd 用尽（打开文件表满） |

- touch 对已存在文件：读目录项所在扇区 → 更新 wrt_date/wrt_time → 写回（status=0 表示成功）
- mkdir 对已存在目录项：返回 status=3

### 空闲簇查找

- 线性扫描 FAT 表：从簇 2 开始逐扇区读 FAT，找 entry==0
- 分配时：标记为链尾（0x0FFFFFFF），写入 FAT1 + FAT2
- 簇追加时：更新前一簇 FAT entry 指向新簇（0x0FFFFFFF → 新簇号）

### 目录簇链扩展

当父目录所有簇的 32 字节槽位都用完（无 0xE5 可复用也无 0x00 空位）：

1. 从 FAT 分配空闲簇
2. 新簇写入零（清空目录内容）
3. 更新 FAT：前一簇的 entry 从 0x0FFFFFFF 改为新簇号，新簇 entry 为 0x0FFFFFFF
4. 在新簇的第一个位置写入新目录项

### mkdir 新目录初始化

FAT32 标准要求新目录包含 `.` 和 `..` 两个特殊条目：

- `.` ：name[11] = `".          "`（点+10空格），attr=0x10，fst_clus_hi/lo = 新目录自身簇号
- `..` ：name[11] = `"..         "`（2点+9空格），attr=0x10，fst_clus_hi/lo = 父目录簇号（根目录的 .. fst_clus=0）
- 新簇剩余位置填 0x00（目录结束标记）

写入流程：构造完整扇区数据（含 ./.. 条目 + 0x00 终止）→ disk_req WRITE

### 时间戳

- touch/mkdir 使用硬编码固定时间戳（FAT32 date/time 格式）
- 固定值：wrt_date = 0x5A21（2026-01-01），wrt_time = 0x0000（00:00:00）
- FAT32 date 格式：(year-1980)<<9 | month<<5 | day
- FAT32 time 格式：hour<<11 | minute<<5 | second/2

### 缓存一致性

写后更新缓存（write-through + update cache）：
- 修改目录项时，直接在缓存簇数据上修改，然后提取受影响扇区写回磁盘——缓存自然是正确的
- FAT 表写入不涉及簇缓存（FAT 读走 disk_resp_shm），无需处理
- 新簇分配后：新簇数据直接构造（./.. 条目或全零），不读缓存，写完后不需要缓存更新

### Shell 命令解析

重构为表驱动方式，便于扩展：

```c
typedef void (*cmd_func)(const char *args);
struct cmd_entry {
    const char *name;
    cmd_func handler;
};
static const cmd_entry cmds[] = {
    {"ls",    cmd_ls},
    {"cat",   cmd_cat},
    {"cd",    cmd_cd},
    {"pwd",   cmd_pwd},
    {"touch", cmd_touch},
    {"mkdir", cmd_mkdir},
    {"r",     cmd_raw},
    {"h",     cmd_help},
};
```

- 每个命令用 strcmp 匹配名称，args 传给 handler
- pwd 无参数，handler 直接 `puts(cwd)`
- touch/mkdir 接受一个路径参数，handler 调用 build_abs_path + fs_request(FS_CMD_CREATE/MKDIR)
- ls 的 `-l` 子选项在 cmd_ls 内部解析

### IPC 流程（CREATE/MKDIR）

```
Shell (PID 4)                      fs_driver (PID 5)                 disk_driver (PID 2)
    |                                     |                                 |
    | 1. 写 fs_req(CREATE/MKDIR)         |                                 |
    | 2. sys_notify(5) --------------->  |                                 |
    |                                     | 3. 读 fs_req, 解析路径           |
    |                                     | 4. 查找父目录簇链               |
    |                                     | 5. 找空闲目录项槽位/分配新簇     |
    |                                     | 6. 构造目录项                   |
    |                                     | 7. 构造扇区数据                 |
    |                                     | 8. disk_req(WRITE)              |
    |                                     | 9. sys_notify(2) -------------> |
    |                                     |                                 | 10. ATA PIO 写
    |                                     | <---- sys_notify(5)             |
    |                                     | 11. (若需FAT更新，重复8-10)      |
    | <---- sys_notify(4)                 |                                 |
    | 12. 读 fs_resp(status)             |                                 |
```

### 变更文件清单（新增）

| 文件 | 操作 | 说明 |
|------|------|------|
| `common/shm.h` | 修改 | 地址常量更新（8页布局）；disk_req_shm 扩到 2 页；新增 FS_CMD_CREATE=5, FS_CMD_MKDIR=6 |
| `kernel/proc.cc` | 修改 | shm_init 多分配 1 物理页；map_shared_pages 改为 8 页映射；地址常量更新 |
| `driver/disk_driver.cc` | 修改 | notify 目标改为 fs_driver_pid；新增 WRITE cmd（ATA PIO LBA28 write） |
| `driver/kbd_driver.cc` | 修改 | 新增 Shift/CapsLock 状态追踪 + shifted scancode 表，支持大小写输入 |
| `driver/fs_driver.cc` | 修改 | 新增 handle_create/handle_mkdir；新增 find_free_cluster/allocate_cluster/dir_chain_extend；BPB 校验防除零；缓存写后更新 |
| `shell/shell.cc` | 修改 | 重构为表驱动命令解析；新增 cmd_pwd/cmd_touch/cmd_mkdir |

## 未来扩展

- RMW 组合命令：disk_driver 新增 READ-MODIFY-WRITE cmd，一次 IPC 内读扇区→改→写回，减少 IPC 开销
- FSINFO 空闲簇提示：解析 BPB 中的 FSINFO sector（LBA part_start+6），用上次空闲簇提示加速查找，写完后更新 FSINFO
- RTC 时间源：UEFI 获取初始时间 → 内核全局时间变量 → sys_gettime syscall → fs_driver 获取真实时间戳
- 真实 RTC 支持：cmos_read 驱动读取硬件实时时钟
- LFN 支持：解析长文件名目录项
- 多客户端：fs_driver 打开文件表按 PID 索引，支持多个进程同时访问
- VFS 层：支持多种文件系统类型
- 文件写入（write cmd）：fs_driver 支持文件内容写入（簇分配 + FAT 链扩展 + 数据写入）
- 删除（unlink/rmdir）：目录项标记 0xE5 + FAT 簇释放
