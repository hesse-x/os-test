# Phase 3: 硬编码共享页迁移到动态 SHM

## 目标

将 disk_driver ↔ fs_driver 和 shell ↔ fs_driver 的硬编码共享页 IPC 迁移到动态 SHM（`sys_shm_create`/`sys_shm_attach`），然后删除全部硬编码共享页基础设施。

迁移后，所有 IPC 共享内存统一走动态 SHM 路径，与 KBD/KMS 已有的模式一致。

---

## 设计决策

| # | 决策 | 选择 | 理由 |
|---|------|------|------|
| 1 | disk SHM 拓扑 | 一块 5 页 SHM | header 1 页 + req 2 页 + resp 2 页；单块分配与 kbd 模式一致，且 `sys_shm_attach` 只支持 first-fit |
| 2 | fs SHM 拓扑 | 一块 4 页 SHM | header 1 页 + req 1 页 + resp 2 页；同上 |
| 3 | 通信协议 | 保持 req/resp 结构体 + 添加 sleeping flag | disk/fs 是严格请求-响应模型，ring buffer 无收益；sleeping flag 修复 lost-wakeup 隐患 |
| 4 | Sleeping flag 位置 | 各自独立 header | disk 是双方通信，kbd/kms 是三方协作，语义不同不应共用 |
| 5 | 启动顺序 | attach 重试循环，不依赖启动顺序 | 3 行代码，兼容未来 init 服务重构 |
| 6 | SHM 内部布局 | header 独占 offset 0，req/resp 页对齐 | 保持结构体定义不变，迁移只改地址获取方式 |
| 7 | client_pid | 保留 | 不增加复杂度，保留未来多客户端扩展能力 |
| 8 | lost-wakeup | 仅 sleeping flag 过滤，不做 double-check | 严格请求-响应模型下不会死锁，最坏多一次空转 |
| 9 | #7/#8 实施时机 | 一起做 | 改动模式相同，一起做避免维护混合状态 |
| 10 | proc_reap | 删 7 路硬编码特判 | 动态 shm_regions 逻辑（含 ref_count 扫描释放）已完善，自动接管 |
| 11 | MAX_SHM_PER_PROC | 4 够用，不扩容 | shell 占 3 槽（kbd + kms + fs），余 1；fs_driver 占 2 槽（自创 fs + attach disk） |
| 12 | fs_driver 调用顺序 | 先 create 再 attach | `sys_shm_attach` first-fit 语义，先 create 确保 shm_regions[0] 是 fs SHM |
| 13 | DISK_CMD_* | 移到 shm.h | 所有 IPC 协议常量集中管理 |
| 14 | disk SHM 大小 | 5 页 | req data[8180] 容纳 15 扇区，FAT32 4KB 簇 = 8 扇区，不能缩减 |
| 15 | 结构体定义 | 不变 | 只改指针获取方式（硬编码地址 → shm_base + offset） |
| 16 | shell ↔ disk_driver | 无直接 IPC，不变 | shell 只通过 fs_driver 间接访问 disk_driver |

---

## SHM 内部布局

### disk_driver SHM（5 页 = 20480 字节）

```
Offset    内容              大小
0         disk_shm_header   1 页 (4096)
4096      disk_req_shm      2 页 (8192: cmd 4B + lba 4B + count 4B + data[8180])
12288     disk_resp_shm     2 页 (8192: status 4B + count 4B + data[8180])
```

disk_driver 调用 `sys_shm_create(5 * 4096)`，fs_driver 调用 `sys_shm_attach(sys_lookup_dev(DEV_DISK))`。

### fs_driver SHM（4 页 = 16384 字节）

```
Offset    内容              大小
0         fs_shm_header     1 页 (4096)
4096      fs_req_shm        1 页 (4096: cmd + client_pid + path[256] + fd/offset/count/lba)
8192      fs_resp_shm       2 页 (8192: status + fd + count + total + data[8176])
```

fs_driver 调用 `sys_shm_create(4 * 4096)`，shell 调用 `sys_shm_attach(sys_lookup_dev(DEV_FS))`。

---

## Sleeping Flag 协议

### disk_shm_header

```c
struct disk_shm_header {
    uint8_t disk_driver_sleeping;   // disk_driver 在 sys_wait 前置 1
    uint8_t fs_driver_sleeping;     // fs_driver 在 disk wait 前置 1
    uint8_t reserved[6];
};
```

**disk_driver 侧：**
```c
// 通知 fs_driver 前：只在对方已睡眠时才 notify
if (hdr->fs_driver_sleeping) {
    sys_notify(sys_lookup_dev(DEV_FS));
}
// 自身睡眠前：设 flag → sys_wait → 清 flag
hdr->disk_driver_sleeping = 1;
sys_wait(0);
hdr->disk_driver_sleeping = 0;
```

**fs_driver 侧（disk 通道）：**
```c
// 通知 disk_driver 前
if (hdr->disk_driver_sleeping) {
    sys_notify(sys_lookup_dev(DEV_DISK));
}
// 自身睡眠前（等 disk 响应）
hdr->fs_driver_sleeping = 1;
sys_wait(0);
hdr->fs_driver_sleeping = 0;
```

### fs_shm_header

```c
struct fs_shm_header {
    uint8_t fs_driver_sleeping;     // fs_driver 在 sys_wait 前置 1
    uint8_t client_sleeping;        // shell 在 fs wait 前置 1
    uint8_t reserved[6];
};
```

**fs_driver 侧（fs 通道）：**
```c
if (hdr->client_sleeping) {
    sys_notify(client_pid);
}
hdr->fs_driver_sleeping = 1;
sys_wait(0);
hdr->fs_driver_sleeping = 0;
```

**shell 侧：**
```c
if (hdr->fs_driver_sleeping) {
    sys_notify(sys_lookup_dev(DEV_FS));
}
hdr->client_sleeping = 1;
sys_wait(0);
hdr->client_sleeping = 0;
```

### 为什么不需要 double-check

KBD/KMS 需要 double-check（`ring->head != ring->tail`）是因为流式数据可能丢失唤醒。disk/fs 是严格请求-响应模型：

1. 严格交替：请求方发完必阻塞，响应方回完必阻塞，不存在连续两次 notify
2. 最坏情况：notify 发生在目标设 flag 之前，notify 被丢弃但目标不会进入 sys_wait（继续循环到下一轮），不会死锁
3. 多一次空转的开销可忽略（微秒级）

---

## 具体改动清单

### 1. `common/shm.h`

**删除：**
- `DISK_REQ_ADDR` / `DISK_REQ_ADDR2` / `DISK_RESP_ADDR` / `DISK_RESP_ADDR2`
- `FS_REQ_ADDR` / `FS_RESP_ADDR` / `FS_RESP_ADDR2`

**新增：**
```c
// Disk SHM internal offsets (within disk_driver's 5-page SHM)
#define DISK_SHM_HEADER_OFFSET  0
#define DISK_REQ_OFFSET         4096    // 1 page in
#define DISK_RESP_OFFSET        12288   // 3 pages in

// FS SHM internal offsets (within fs_driver's 4-page SHM)
#define FS_SHM_HEADER_OFFSET    0
#define FS_REQ_OFFSET           4096    // 1 page in
#define FS_RESP_OFFSET          8192    // 2 pages in

struct disk_shm_header {
    uint8_t disk_driver_sleeping;
    uint8_t fs_driver_sleeping;
    uint8_t reserved[6];
};

struct fs_shm_header {
    uint8_t fs_driver_sleeping;
    uint8_t client_sleeping;
    uint8_t reserved[6];
};

#define DISK_CMD_READ  0
#define DISK_CMD_WRITE 1
```

**保留不变：** `disk_req_shm`、`disk_resp_shm`、`fs_req_shm`、`fs_resp_shm`、`fs_dirent`、`FS_CMD_*`、`kms_fb_info`、`driver_shm_header`、`kbd_ring`、`kms_ring`

### 2. `driver/disk_driver.cc`

- 删除 `DISK_REQ_ADDR`/`DISK_RESP_ADDR` 硬编码指针
- 开头 `sys_shm_create(5 * 4096)`，初始化 header + sleeping flag 清零
- `req = (volatile disk_req_shm *)(shm_base + DISK_REQ_OFFSET)`
- `resp = (volatile disk_resp_shm *)(shm_base + DISK_RESP_OFFSET)`
- `hdr = (volatile disk_shm_header *)shm_base`
- notify fs_driver 前检查 `hdr->fs_driver_sleeping`
- `sys_wait(0)` 前设 `hdr->disk_driver_sleeping = 1`，返回后清零

### 3. `driver/fs_driver.cc`

- 删除 `DISK_REQ_ADDR`/`DISK_RESP_ADDR`/`FS_REQ_ADDR`/`FS_RESP_ADDR` 硬编码指针
- **先** `sys_shm_create(4 * 4096)` 创建 fs SHM（占 shm_regions[0]，确保 shell attach 时 first-fit 命中 fs SHM）
- **再** `sys_shm_attach(sys_lookup_dev(DEV_DISK))` + 重试循环 attach disk SHM
- fs 通道：
  - `freq = (volatile fs_req_shm *)(fs_shm + FS_REQ_OFFSET)`
  - `fresp = (volatile fs_resp_shm *)(fs_shm + FS_RESP_OFFSET)`
  - `fs_hdr = (volatile fs_shm_header *)fs_shm`
- disk 通道：
  - `dreq = (volatile disk_req_shm *)(disk_shm + DISK_REQ_OFFSET)`
  - `dresp = (volatile disk_resp_shm *)(disk_shm + DISK_RESP_OFFSET)`
  - `disk_hdr = (volatile disk_shm_header *)disk_shm`
- 两个方向的 sleeping flag 过滤逻辑

### 4. `shell/shell.cc`

- 删除 `FS_REQ_ADDR`/`FS_RESP_ADDR` 硬编码指针
- `sys_shm_attach(sys_lookup_dev(DEV_FS))` + 重试循环获取 fs SHM
- `freq = (volatile fs_req_shm *)(fs_shm + FS_REQ_OFFSET)`
- `fresp = (volatile fs_resp_shm *)(fs_shm + FS_RESP_OFFSET)`
- `fs_hdr = (volatile fs_shm_header *)fs_shm`
- `fs_request()` 中：notify fs_driver 前检查 `fs_hdr->fs_driver_sleeping`，wait 前设 `fs_hdr->client_sleeping`

### 5. `kernel/proc.cc`

- 删除 7 个 `*_shm_phys` 静态变量（行 51-57）
- 删除 `shm_init()` 函数（行 98-147）
- 删除 `map_shared_pages()` 函数（行 150-167）
- `process_create_elf()` 中删除 `map_shared_pages()` 调用（行 328 附近）
- `proc_reap()` 中删除 7 路硬编码物理地址特判（行 494-500），保留动态 `shm_regions[]` 逻辑不变

### 6. `kernel/proc.h`

- 删除 `void shm_init()` 声明

### 7. `kernel/kernel.cc`

- 删除 `kernel_main` 中的 `shm_init()` 调用

---

## 不改的文件

- `common/dev.h` — 设备类型定义，驱动 PID 通过 `sys_lookup_dev` 动态发现（详见 [dev_table.md](dev_table.md)）
- `kernel/trap.cc` — `sys_shm_create`/`sys_shm_attach` 不变
- `driver/kbd_driver.cc` / `driver/kms_driver.cc` / `driver/terminal.cc` — 已迁移，不动

---

## 验证方案

1. `./build.sh && ./run.sh` 启动后等待 10s+
2. 检查 `log.txt` 无 panic/halt
3. 在 shell 中测试：
   - `ls` — 验证 fs_driver IPC（shell → fs_driver → disk_driver 链路）
   - `cat HELLO.TXT` — 验证多轮 READ 请求
   - `run HELLO.ELF` — 验证 OPEN + READ + CLOSE 完整流程
   - `r 601` — 验证 RAW_READ 路径
4. 串口无异常输出
