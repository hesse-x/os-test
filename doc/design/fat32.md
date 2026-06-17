# FAT32 文件系统 — 实现记录

## 概述

FAT32 文件系统作为独立用户态进程（fs_driver）运行，通过 `sys_msg`/`sys_msg_resp` 与客户端进程通信，通过 `sys_block_read`/`sys_block_write`（以及异步 `sys_block_async`）直接访问内核 AHCI 驱动。支持目录遍历、文件读写、创建文件/目录，VFAT LFN 长文件名读写。

完整设计详见 [file_system.md](file_system.md)。

## 磁盘布局

```
disk.img (64MB, 131072 扇区)
├── LBA 0:       MBR 分区表（sfdisk 创建）
├── LBA 1-100:   fs_driver.elf（100 扇区/50KB）
├── LBA 201-300: init.elf（100 扇区/50KB）
├── LBA 301+:    FAT32 分区（4KB 簇，MBR 分区2 指向，type=0x0C）
```

- MBR 分区1 (type=0xDA): LBA 1-300，裸 ELF 存储
- MBR 分区2 (type=0x0C): LBA 301-131071，FAT32 文件系统
- FAT32 使用 `-s 8`（4KB/簇），符合 Microsoft FAT32 规范推荐
- 目录结构详见 [file_system.md](file_system.md)

## IPC 协议

fs_driver 与客户端通过 `sys_msg`/`sys_msg_resp` 通信（变长 IPC，≤64KB）。fs_driver 与内核 AHCI 驱动通过 `sys_block_read`/`sys_block_write`/`sys_block_async` 通信。

### 消息协议

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

## fs_driver 内部设计

### FAT32 解析

1. 读 MBR（LBA 0），解析分区表，找到 FAT32 分区起始 LBA
2. 读 BPB（分区起始扇区），解析卷参数（簇大小、FAT 起始、数据区起始等）
3. BPB 校验：`bps!=512 || sectors_per_cluster==0 || spf32==0 || root_cluster<2` 时报错
4. 路径解析：`resolve_state` 子状态机，从根目录逐级查找，支持 VFAT LFN + `..`
5. 文件读取：沿 FAT 簇链读取数据

### FAT 扇区缓存

固定 4 页（16KB）FAT 扇区缓存。`-s 8` 下 FAT 表约 15KB，4 页可 100% 缓存。

### 数据簇 cache

`CACHE_SLOTS=16`（16 × 4KB = 64KB），LRU 策略，带 `pin_count` 防止写期间淘汰。

### 写入支持

- **文件写入（write cmd）**：完整状态机（FAT 链遍历 + 簇分配/扩展 + 数据写入 + 目录项更新），支持 O_WRONLY/O_RDWR/O_APPEND
- **create**：创建空文件或更新已存在文件时间戳，LFN 条目 + 8.3 短名
- **mkdir**：创建目录 + `.`/`..` 条目 + 簇分配
- LFN 写入：始终写 LFN + 8.3 条目（与 Linux vfat 一致）
- 8.3 短名生成：合法 8.3 直接用，否则前 6 字符 + `~N`，异步冲突检查
- 时间戳：硬编码 `HARD_DATE=0x5A21`，`HARD_TIME=0x0000`

### 异步事件循环

fs_driver 采用单线程事件循环 + 异步 I/O。磁盘请求通过 `sys_block_async` 提交，完成后内核发送 RECV_NOTIFY 回调。详见 [file_system.md](file_system.md)。

### 限制

- 时间戳硬编码（无 RTC）
- 删除（unlink/rmdir）未实现
- 不支持硬链接/符号链接
- 不支持文件截断（truncate）

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

## 未来扩展

- 删除（unlink/rmdir）：目录项标记 0xE5 + FAT 簇释放
- RTC 时间源：真实时间戳
- FSINFO 空闲簇提示：加速查找
- 文件截断（truncate）
