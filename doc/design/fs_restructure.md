# 文件系统重构 — LFN + 目录结构 + Shell 改造 + 性能优化

## 概述

重构文件系统目录结构和 fs_driver 能力，已完成目标：
1. FAT32 目录结构对齐 Linux FHS（Filesystem Hierarchy Standard）
2. 支持 VFAT LFN 长文件名读写
3. Shell 支持路径执行（替代 `run` 命令）

启动流程改造（init + exec + 从 FAT32 启动用户态服务）已拆到 [refactor_boot.md](refactor_boot.md)。

## FAT32 目录结构

```
/                    根目录
├── README           测试用文本文件
├── boot/            启动必需文件
│   ├── bin/         init（refactor_boot 后内核从裸LBA加载的唯一用户态入口）
│   └── driver/      disk.dev, fs.dev（refactor_boot 后内核从裸LBA加载的驱动）
├── driver/          用户态驱动（refactor_boot 后 init/fork 从 FAT32 加载）
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

### O.2 数据簇 cache 增大

`CACHE_SLOTS` 从 2 增到 8（8 × 4KB = 32KB），覆盖典型目录遍历深度。

FAT 缓存和数据簇 cache 分离，互不挤占：
- FAT 缓存：随机访问模式，固定 4 页
- 数据簇 cache：遍历模式，8 slot

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

## 搁置项

| 项目 | 原因 | 触发条件 |
|------|------|---------|
| dresp->data 生命周期 | FAT 缓存部分缓解（FAT hit 时不覆盖 dresp->data），当前单客户端不会触发 | 多客户端 |
| disk_wait_reply deferred 队列 | FAT 缓存降低调用频率，当前单客户端不会触发忙循环 | 多客户端 |
