# FAT32 文件系统 — 实现记录

## 概述

FAT32 文件系统作为独立用户态进程（fs_driver）运行，通过 `sys_msg`/`sys_msg_resp` 与客户端进程通信，内部通过 disk_driver SHM IPC 访问磁盘。支持目录遍历、文件读写、创建文件/目录，VFAT LFN 长文件名读写。

## 磁盘布局

```
disk.img (64MB, 131072 扇区)
├── LBA 0:       MBR 分区表（sfdisk 创建）
├── LBA 1-100:   disk_driver.elf（100 扇区/50KB）
├── LBA 101-200: kbd_driver.elf（100 扇区/50KB）
├── LBA 201-300: kms_driver.elf（100 扇区/50KB）
├── LBA 301-400: terminal.elf（100 扇区/50KB）
├── LBA 401-500: shell.elf（100 扇区/50KB）
├── LBA 501-600: fs_driver.elf（100 扇区/50KB）
└── LBA 601-131071: FAT32 分区（4KB 簇，MBR 分区2 指向，type=0x0C）
```

- MBR 分区表包含两个分区：
  - 分区1: LBA 1-600，裸 ELF 存储，type=0xDA
  - 分区2: LBA 601-131071，FAT32 文件系统，type=0x0C
- FAT32 使用 `-s 8`（4KB/簇），符合 Microsoft FAT32 规范推荐
- 目录结构详见 [fs_restructure.md](fs_restructure.md)

## IPC 协议

fs_driver 与客户端通过 `sys_msg`/`sys_msg_resp` 通信（变长 IPC，≤64KB）。fs_driver 与 disk_driver 通过动态 SHM 通信。

### 消息协议

详见 [rpc.md](rpc.md) §libc 文件 I/O。

| cmd | 名称 | 请求字段 | 回复字段 |
|-----|------|---------|---------|
| 1 | open | path, flags | status, fd, file_size |
| 2 | read | fs_fd, offset, count | status, count, data[] |
| 3 | write | fs_fd, offset, count, data[] | status（stub，返回 ENOSYS） |
| 4 | close | fs_fd | status |
| 5 | readdir | path, readdir_offset, readdir_count | status, total, count, data[] |
| 6 | create | path | status |
| 7 | mkdir | path | status |
| 8 | raw_read | lba | status, count, data[] |

### fs_driver 多客户端 session

```c
#define MAX_CLIENTS  16

struct client_session {
    pid_t client_pid;
    struct open_file open_files[8];  // 每客户端 8 个 fd
};

static struct client_session sessions[MAX_CLIENTS];
```

## fs_driver 内部设计

### FAT32 解析

1. 读 MBR（LBA 0），解析分区表，找到 FAT32 分区起始 LBA
2. 读 BPB（分区起始扇区），解析卷参数（簇大小、FAT 起始、数据区起始等）
3. BPB 校验：`bps!=512 || sectors_per_cluster==0 || spf32==0 || root_cluster<2` 时报错
4. 路径解析：从根目录逐级查找，支持 VFAT LFN
5. 文件读取：沿 FAT 簇链读取数据

### FAT 扇区缓存

固定 4 页（16KB）FAT 扇区缓存（`fat_cache_entry`，`FAT_CACHE_PAGES=4`）。`-s 8` 下 FAT 表约 15KB，4 页可 100% 缓存。

### 数据簇 cache

`CACHE_SLOTS=8`（8 × 4KB = 32KB），LRU 策略。

### 写入支持

- `handle_create`：创建空文件或更新已存在文件时间戳，LFN 条目 + 8.3 短名
- `handle_mkdir`：创建目录 + `.`/`..` 条目 + 簇分配
- LFN 写入：始终写 LFN + 8.3 条目（与 Linux vfat 一致）
- 8.3 短名生成：合法 8.3 直接用，否则前 6 字符 + `~N`
- 时间戳：硬编码 `HARD_DATE=0x5A21`，`HARD_TIME=0x0000`

### 限制

- 文件内容写入（write cmd）返回 ENOSYS
- 时间戳硬编码（无 RTC）
- 删除（unlink/rmdir）未实现

## disk_driver 通信

fs_driver 通过动态 SHM 与 disk_driver 通信（5 页 SHM），详见 [dynamic_shm_migration.md](dynamic_shm_migration.md)。disk_driver 支持 READ(0) 和 WRITE(1) 命令。

## Shell 命令

| 命令 | 用途 |
|------|------|
| `ls [-l]` | 列目录（支持路径参数，`-l` 显示权限/大小/日期） |
| `cat <path>` | 读文件打印内容 |
| `cd <path>` | 切换目录 |
| `pwd` | 打印当前工作目录 |
| `touch <path>` | 创建空文件 / 更新已存在文件时间戳 |
| `mkdir <path>` | 创建目录 |
| `r LBA [COUNT]` | 裸扇区读（调试用） |
| `<path>` | 路径执行（替代旧 `run` 命令） |
| `h` | 帮助 |

路径执行：未匹配内置命令时当作文件路径 → `open` → `read` 整个文件 → ELF 校验 → `sys_spawn` + `sys_waitpid`。

## 未来扩展

- 文件内容写入（write cmd）：簇分配 + FAT 链扩展 + 数据写入
- 删除（unlink/rmdir）：目录项标记 0xE5 + FAT 簇释放
- RTC 时间源：真实时间戳
- RMW 组合命令：一次 IPC 内读扇区→改→写回
- FSINFO 空闲簇提示：加速查找
