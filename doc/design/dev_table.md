# 设备管理器：dev_table + sys_load_dev / sys_lookup_dev

> **已实现**（commit b4ed753）。内核维护 `dev_table[dev_type → PID]` 映射，用户态驱动通过 `sys_lookup_dev` 动态发现对端 PID，消除了 `common/pid.h` 硬编码 PID 依赖。

## 1. 动机

此前所有驱动间 IPC 依赖 `common/pid.h` 中硬编码的 PID 常量（`DISK_DRIVER_PID=2`、`KBD_DRIVER_PID=3` 等）。这带来三个问题：

1. **启动顺序耦合**：PID 由 `kernel_main` 中 `process_create_elf` 的调用顺序决定，改变加载顺序即破坏所有驱动
2. **动态 spawn 不可用**：用户态 `sys_spawn` 创建的驱动 PID 不固定，无法被其他进程发现
3. **冗余依赖**：每个驱动源文件都 `#include "common/pid.h"` 仅为了获取一个常量

## 2. 设计

### 2.1 设备类型表（dev_table）

内核维护全局数组 `dev_table[DEV_TYPE_MAX]`（DEV_TYPE_MAX=32），以设备类型为索引、PID 为值：

```c
// kernel/trap.cc
static pid_t dev_table[DEV_TYPE_MAX];  // 索引=dev_type，值=PID，0=空
```

初始化在 `isr_init()` 中全置 0。

### 2.2 设备类型定义（common/dev.h）

```c
#define DEV_TYPE_MAX  32
#define DEV_NONE      0   // 空槽 / 未找到
#define DEV_DISK      1
#define DEV_KBD       2
#define DEV_KMS       3
#define DEV_FS        4
#define DEV_TERMINAL  5
```

类型编号 1-31 可用，0 保留（表示空/无效），32 以上越界。

### 2.3 sys_load_dev(pid, dev_type) — syscall 18

注册一个驱动 PID 到设备类型表。

| 参数 | 含义 |
|------|------|
| arg1 = pid | 要注册的进程 PID |
| arg2 = dev_type | 设备类型（DEV_DISK 等） |

**返回值**：0=成功，正数=errno（EINVAL=类型无效，EEXIST=已被占用）

**权限**：当前无权限检查，任何进程均可调用。内核启动时由 `kernel_main` 直接调用 `register_dev()` 内核函数注册；用户态通过 syscall 注册。

### 2.4 sys_lookup_dev(dev_type) — syscall 19

查询指定设备类型的驱动 PID。

| 参数 | 含义 |
|------|------|
| arg1 = dev_type | 设备类型 |

**返回值**：PID（成功），0（未注册或类型无效）

### 2.5 内核接口

```c
// kernel/trap.h
int  register_dev(int dev_type, int32_t pid);  // 内核内部调用，非 syscall
void dev_table_cleanup(int32_t pid);           // proc_reap 中清理
```

- `register_dev`：`kernel_main` 中创建驱动进程后直接调用，跳过 syscall 开销
- `dev_table_cleanup`：`proc_reap` 中遍历 `dev_table` 清除已退出进程的注册项

## 3. 启动时注册流程

`kernel_main` 中每个驱动创建成功后立即注册：

```c
proc_t *disk_proc = process_create_elf(elf, sz, 3);
if (disk_proc) register_dev(DEV_DISK, disk_proc->pid);

proc_t *kbd_proc = process_create_elf(elf, sz, 3);
if (kbd_proc) register_dev(DEV_KBD, kbd_proc->pid);

// ... 同理 DEV_KMS, DEV_TERMINAL, DEV_FS
```

Shell 不注册（它不是驱动服务，不被动发现）。

## 4. 驱动使用方式

### 4.1 替代硬编码 PID

所有原来 `#include "common/pid.h"` 并使用 `XXX_PID` 常量的地方，改为 `sys_lookup_dev(DEV_XXX)` 动态获取：

| 驱动 | 原代码 | 新代码 |
|------|--------|--------|
| disk_driver | `FS_DRIVER_PID` | `sys_lookup_dev(DEV_FS)` |
| kbd_driver | `TERMINAL_PID` | `sys_lookup_dev(DEV_TERMINAL)` |
| kms_driver | `sys_shm_attach(KBD_DRIVER_PID)` | `sys_shm_attach(sys_lookup_dev(DEV_KBD))` |
| terminal | `KBD_DRIVER_PID` / `KMS_DRIVER_PID` | `sys_lookup_dev(DEV_KBD)` / 缓存到 `kms_driver_pid` |
| fs_driver | `DISK_DRIVER_PID` | `sys_lookup_dev(DEV_DISK)` |
| shell | `FS_DRIVER_PID` | `sys_lookup_dev(DEV_FS)` |
| libc stdio | `KBD_DRIVER_PID` / `KMS_DRIVER_PID` | `sys_lookup_dev(DEV_KBD)` / `sys_lookup_dev(DEV_KMS)` |

### 4.2 PID 缓存

对于频繁使用的对端 PID（如 terminal 每次写 KMS ring 都需要 notify kms_driver），在 `_start` 中查询一次并缓存到局部变量，避免每次 IPC 都走 syscall：

```c
// driver/terminal.cc
static int32_t kms_driver_pid;

extern "C" void _start() {
    kms_driver_pid = sys_lookup_dev(DEV_KMS);
    // ...
}
```

### 4.3 shm_attach 时序

`sys_shm_attach` 需要 target PID 参数。原来直接传常量，现在传 `sys_lookup_dev()` 的返回值。由于 shm_attach 有重试循环（对端可能尚未创建 shm），lookup_dev 返回值在循环外获取一次即可：

```c
// driver/fs_driver.cc
while ((disk_shm = sys_shm_attach(sys_lookup_dev(DEV_DISK))) == 0) {
    sys_wait(1);
}
```

## 5. 进程退出清理

`proc_reap` 在回收进程资源时调用 `dev_table_cleanup(pid)`，遍历 `dev_table` 将该 PID 对应的槽位清零：

```c
// kernel/proc.cc — proc_reap()
dev_table_cleanup(proc->pid);
// 之后清零 PCB 槽位
```

这保证已退出驱动的 dev_type 不会被误查找。后续新驱动注册相同 dev_type 时不会触发 EEXIST。

## 6. 移除 common/pid.h

`common/pid.h` 已删除，其硬编码 PID 常量（DISK_DRIVER_PID=2 等）全部被 `sys_lookup_dev(DEV_XXX)` 替代。`common/dev.h` 取代了其角色。

## 7. Syscall 表更新

NR_SYSCALL 从 18 增至 20，新增编号连续无空洞：

| # | 名称 | 功能 |
|---|------|------|
| 18 | sys_load_dev | 注册驱动 PID 到设备类型表 |
| 19 | sys_lookup_dev | 按设备类型查询驱动 PID |

## 8. 新增 errno

`common/errno.h` 新增 `EEXIST = 8`，用于 `sys_load_dev` 重复注册时返回。

## 9. 局限与后续

- **无权限检查**：当前任何用户进程均可调用 `sys_load_dev` 注册设备。后续可限制为 IOPL≥1 或特权 PID 才能注册
- **单实例**：每个 dev_type 仅能注册一个 PID，不支持同类多设备（如多个磁盘控制器）
- **无注销 syscall**：驱动正常退出由 `dev_table_cleanup` 自动清理，但用户态无法主动注销。可按需新增 `sys_unload_dev`
- **查询无锁**：`dev_table` 读取未加锁，依赖内核单写者（仅 `register_dev` 和 `dev_table_cleanup` 写入，均在内核态）的隐式安全。SMP 场景下写入与读取的时序由 x86 store 原子性保证
