# 设备管理器：dev_table + sys_load_dev / sys_open_dev

> 内核维护 `dev_table[dev_type → PID]` 映射。用户态驱动发现通过 `open("/dev/xxx")` → `sys_open_dev` 完成，不再需要 `sys_lookup_dev` 或 `device_lookup()`。`sys_open_dev` 返回打包的 `(fd | target_pid << 32)`，一次 syscall 同时提供 fd 和对端 PID。

## 1. 动机

此前所有驱动间 IPC 依赖 `sys_lookup_dev` syscall 查询对端 PID。`sys_open_dev` (#35) 引入后，内核已在 `fd_table[fd].target_pid` 存储了 PID，用户态却仍需二次调用 `sys_lookup_dev` 才能在 libc `fd_table` 中缓存 PID——这是冗余的。

删除 `sys_lookup_dev` 后：
1. 用户态不再需要知道 PID，发消息用 `msg_fd(fd, ...)` 替代 `sys_msg(pid, ...)`
2. 驱动 readiness 检查：`while (device_lookup(DEV_FS) <= 0)` → `while (open("/dev/fs", ...) < 0)`
3. 内核净减 1 个 syscall（NR_SYSCALL 不变，#20 改为 `sys_dev_msg`）

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
#define DEV_DISK      1   // deprecated
#define DEV_KBD       2
#define DEV_KMS       3
#define DEV_FS        4
#define DEV_TERMINAL  5
```

类型编号 1-31 可用，0 保留（表示空/无效），32 以上越界。

### 2.3 sys_load_dev(pid, dev_type) — syscall 19

注册一个驱动 PID 到设备类型表。

| 参数 | 含义 |
|------|------|
| arg1 = pid | 要注册的进程 PID |
| arg2 = dev_type | 设备类型（DEV_DISK 等） |

**返回值**：0=成功，正数=errno（EINVAL=类型无效，EEXIST=已被占用）

### 2.4 sys_open_dev(dev_type) — syscall 35

打开设备节点，返回 fd 和对端 PID。

**返回值**：`(fd | target_pid << 32)` — fd 用于后续 `msg_fd/req_fd/notify_fd`，PID 用于 `sys_shm_attach`（通过 `mmap(MAP_SHARED)` 自动处理）。

### 2.5 sys_dev_msg(fd, msg_buf, msg_len, reply_buf, reply_len) — syscall 20

fd 版的 `sys_msg` — 从当前进程 `fd_table[fd].target_pid` 提取 PID，委托 `sys_msg_to()` 内部逻辑。

| 参数 | 含义 |
|------|------|
| arg1 = fd | 设备 fd（必须为 FD_DEV 类型） |
| arg2-5 | 与 sys_msg 相同 |

### 2.6 内核接口

```c
// kernel/trap.h
int  register_dev(int dev_type, int32_t pid);  // 内核内部调用，非 syscall
void dev_table_cleanup(int32_t pid);           // proc_reap 中清理
pid_t lookup_dev(int dev_type);                // ISR 上下文直接查表
```

- `register_dev`：`kernel_main` 中创建驱动进程后直接调用
- `dev_table_cleanup`：`proc_reap` 中遍历 `dev_table` 清除已退出进程的注册项
- `lookup_dev`：xHCI ISR 中调用，无需进入 syscall 路径

## 3. 启动时注册流程

`kernel_main` 中每个驱动创建成功后立即注册；用户态驱动（ksb_driver/kms_driver/fs_driver）通过 `sys_load_dev` 自注册。

## 4. fd-based 驱动使用方式

### 4.1 等待驱动就绪 → open 轮询

```c
// 替代 device_lookup(DEV_FS)
while (open("/dev/fs", O_RDWR) < 0) {
    recv(&m, NULL, 0, 10);
}
```

### 4.2 发消息 → msg_fd

```c
// 替代 sys_msg(fs_pid, req, ...)
msg_fd(fs_fd, req, sizeof(*req), reply, sizeof(reply));
```

### 4.3 控制命令 → req_fd

```c
// 不变——req_fd 从 libc fd_table 缓存中取 PID
req_fd(kbd_fd, &bind_req, &bind_reply);
```

### 4.4 SHM attach → mmap(MAP_SHARED)

```c
// 不变——sys_mman.cc 从 libc fd_table 取 PID → sys_shm_attach
shm_ptr = mmap(NULL, 0, PROT_READ|PROT_WRITE, MAP_SHARED, kbd_fd);
```

## 5. 进程退出清理

`proc_reap` 调用 `dev_table_cleanup(pid)` 清理注册项。

## 6. Syscall 表变更

| # | 名称 | 功能 |
|---|------|------|
| 19 | sys_load_dev | 注册驱动 PID 到设备类型表 |
| **20** | **sys_dev_msg** | **fd 版变长消息请求（替代 sys_lookup_dev）** |
| 35 | sys_open_dev | 打开设备节点（返回 fd + target_pid） |

## 7. 已删除的设施

- `sys_lookup_dev` (#20) — 不再需要，PID 由 `sys_open_dev` 打包返回
- `device_lookup()` libc 函数 — 改用 `open("/dev/xxx")`
- 所有驱动间 PID 硬编码依赖已被完全消除

## 8. 局限

- 无权限检查：当前任何用户进程均可调用 `sys_load_dev`
- 单实例：每个 dev_type 仅能注册一个 PID
