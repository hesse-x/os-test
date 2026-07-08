# ioctl 代理机制

## 当前架构设计

用户态驱动设备的 ioctl 通过内核 IPC 代理转发到驱动进程。按 arg 数据大小分两条路径：

- **inline 路径**（arg_size ≤ 48）：`RECV_REQ` + `req_data[56]`（cmd + arg + minor），零 kmalloc
- **变长路径**（arg_size > 48）：`RECV_IOCTL` + kmalloc'd buffer，支持 ≤64KB 载荷（`EVIOCGBIT(0,96)` 等大 getter）

`sys_resp` 三参分离（reply 纯数据 / reply_len / result），result 走 `req_result` 独立通道作 ioctl 返回值，reply 数据 copy 到客户端 arg，arg layout 对齐 Linux（纯结构体，无 result 前缀）。

### 设计决策

| # | 决策 | 选择 | 理由 |
|---|------|------|------|
| 1 | inline 阈值 | arg_size ≤ 48 | `req_data[56]` = cmd[4] + arg[≤48] + minor[4]；minor 路由占用尾部 4B（见 [evdev.md](evdev.md)） |
| 2 | 变长消息类型 | 新增 `RECV_IOCTL=4` 而非复用 `RECV_MSG` | 驱动需区分"普通 msg"与"ioctl 变长请求"；`recv_msg` union 新增 `ioctl` 命名成员存 {cmd, arg_size, kmaddr, len}，类型安全 |
| 3 | recv_msg ABI | 总大小不变（64B），union 内新增成员 | `RECV_MSG_SIZE` 不变，recv queue / 所有 IPC 生产者不动；新成员只是给同段内存起别名 |
| 4 | sys_resp 签名 | 三参 `(reply, reply_len, result)` | result 与数据分离：result 写 `caller->req_result`，reply 纯数据 copy 到 `req_reply_buf`；消除 ioctl 专属 hack（旧版 result 夹在 reply 前 4B） |
| 5 | req_reply_len | = `arg_size`（动态 `_IOC_SIZE`） | 限制 sys_resp 回写不超过 arg 实际大小；inline 与变长路径统一 |
| 6 | arg 上限 | `uint16_t` 隐式 cap（≤65535）+ `_IOC_SIZE` 14 位字段（≤16383） | 类型天然限制，无需显式守卫；与 sys_msg 显式 `>65536` 检查不同 |
| 7 | 变长路径 arg==0 | 返回 EINVAL | arg_size>48 必然需要 buffer，arg==0 是调用方 bug，会导致 sys_resp 的 copy_to_user(NULL) page fault |
| 8 | 内核设备路径（driver_pid==0） | kbuf_stack[256]，arg_size>240 时 kmalloc | 栈缓冲覆盖常见 ioctl，大命令动态分配；与用户态驱动路径上限对齐 |
| 9 | kbuf 释放时机 | `sys_recv` dequeue 时 copy 到驱动 data_buf 并立即 kfree | kbuf 在 recv 阶段释放，不依赖驱动后续行为；驱动 crash 时 sched_task_reap 兜底清理 recv queue 残留 |
| 10 | 复用等待状态 | 变长路径复用 `WAIT_REQ_REPLY` + 3s 超时 | 与 inline 路径一致，无需新状态 |

### 核心数据结构

recv_msg（include/uapi/xos/syscall_nums.h : recv_msg，64B 不变）：
  type : uint32_t — RECV_IRQ/REQ/NOTIFY/MSG/IOCTL
  src : uint32_t — 发送方 PID
  union（56B）：
    data[56] : uint8_t — RECV_IRQ/REQ/NOTIFY 用
    msg : { kmaddr, len } — RECV_MSG 用
    ioctl : { cmd, arg_size, kmaddr, len } — RECV_IOCTL 用（24B，8 字节对齐）

RECV_IOCTL 的 union 布局（offset 相对 recv_msg）：
  [0..4) type = RECV_IOCTL(4)
  [4..8) src = 客户端 PID
  [8..12) ioctl.cmd — ioctl 命令号
  [12..16) ioctl.arg_size — arg 数据长度
  [16..24) ioctl.kmaddr — 内核 kmalloc buffer 指针（与 msg.kmaddr 同偏移，union 共享）
  [24..32) ioctl.len — = arg_size
  [32..64) unused

`ioctl.kmaddr` 与 `msg.kmaddr` 在 union 同一偏移（offset 16），sched_task_reap 清理时统一用 `m->msg.kmaddr` 读取。

### 关键流程

**inline 路径**（arg_size ≤ 48，kernel/bsd/syscall.c : sys_ioctl）：
1. `req_data[56]`：[0..4)=cmd，[4..4+arg_size)=arg（`_IOC_WRITE` 时 copy_from_user），[52..56)=ops->minor
2. 构造 `RECV_REQ` 入队 target->recv_buf，唤醒驱动（若 WAIT_RECV）
3. 调用者阻塞 `WAIT_REQ_REPLY`，`req_reply_buf=arg`，`req_reply_len=arg_size`，3s 超时
4. 驱动 recv → handle → `sys_resp(reply_data, len, result)`
5. `sys_resp`：result 写 `caller->req_result`，reply copy 到 `caller->req_reply_buf`（copy_len=min(len, req_reply_len)），唤醒
6. 调用者唤醒后 `return req_result`（arg 搬移 hack 已删除）

**变长路径**（arg_size > 48，kernel/bsd/syscall.c : sys_ioctl）：
1. arg==0 → EINVAL；kmalloc(arg_size)，`_IOC_WRITE` 时 copy_from_user
2. 构造 `RECV_IOCTL`（hdr->ioctl = {cmd, arg_size, kbuf, arg_size}）入队，唤醒驱动
3. 调用者阻塞 `WAIT_REQ_REPLY`，`req_reply_buf=arg`，`req_reply_len=arg_size`
4. 驱动 `recv(&msg, data_buf, data_buf_len, ...)`：
   - data_buf 不足 → kfree kbuf，回传 len，返回 EINVAL（驱动可探测后重试）
   - 正常 → copy_to_user(data_buf, kbuf, len)，kfree kbuf
5. 驱动 handle → `sys_resp(reply_data, len, result)`
6. `sys_resp`：reply_len>0 时 kmalloc + copy_from_user + CR3 switch + copy_to_user(arg) + kfree；result 写 req_result；唤醒
7. 调用者唤醒后 `return req_result`

**sys_resp**（kernel/xcore/ipc.c : sys_resp）：
1. 校验 reply 指针（reply_len>0 时）
2. `caller->req_result = result`
3. reply_len==0 → 仅唤醒（如只需返回 result 无数据）
4. copy_len = min(reply_len, caller->req_reply_len)
5. copy_len ≤ RECV_MSG_SIZE(64) → 栈 kbuf；否则 kmalloc
6. copy_from_user(kbuf, reply) → CR3 switch 到 caller → copy_to_user(req_reply_buf, kbuf) → CR3 还原 → kfree
7. 唤醒 caller

**sys_recv 处理 RECV_IOCTL**（kernel/xcore/ipc.c : sys_recv）：
1. dequeue 到 RECV_IOCTL：记录 `proc->req_caller_pid = msg->src`
2. data_buf==NULL 或 data_buf_len<len → kfree kmaddr，回传 len 到 umsg->ioctl.len，返回 EINVAL
3. 正常 → copy_to_user(data_buf, kmaddr, len)，kfree kmaddr，清空 umsg->ioctl.kmaddr

**内核设备路径**（driver_pid==0，kernel/bsd/syscall.c : sys_ioctl）：
1. kbuf_stack[256] 栈缓冲；arg_size>240 → kmalloc(arg_size)
2. `_IOC_WRITE` 时 copy_from_user(arg → kbuf)
3. `ops->ioctl(cmd, kbuf)` 直接调用内核 handler
4. `_IOC_READ` 时 copy_to_user(arg ← kbuf)
5. 动态 kbuf → kfree

### 锁模型

变长路径 sys_resp 的 CR3 switch 区间无锁，依赖 `caller->pid != caller_pid` 检查防 caller 已退出（caller 提前被 kill 时 sched_task_reap 设 pid=-1/cr3=0，sys_resp 检查后返回 ESRCH）。检查与 CR3 switch 间的 TOCTOU 窗口是既有竞态，变长路径不引入新风险。

### 回写数据约定

`sys_resp` 回写到客户端 arg 的是纯结构体数据（`_IOC_READ` 内容），不含 result 前缀：

```
客户端 arg buffer:
  调用前: [_IOC_WRITE 数据]   ← 客户端填入
  调用后: [_IOC_READ 数据]    ← sys_resp 写入（纯数据）
  返回值: sys_ioctl return    ← 来自 req_result
```

### 用户态驱动适配

驱动主循环按 msg.type 分支处理（user/lib/sys_ipc.cc : resp → sys_resp）：

- `RECV_REQ`：inline 路径，cmd/arg 从 msg.data 解包，`resp(data, len, result)` 回传
- `RECV_IOCTL`：变长路径，cmd/arg_size 从 msg.ioctl 解包，arg 数据在 recv 的 data_buf 中，`resp(data, len, result)` 回传

`recv(&msg, data_buf, data_buf_len, timeout)`：RECV_IOCTL 需提供 data_buf 接收 arg 数据；不确定大小时可先传 NULL 探测，返回 EINVAL 后按 umsg->ioctl.len 分配重试。

现有调用点：
- user/lib/input_driver.cc : INPUT_BIND 调用 `sys_resp(&reply, sizeof(reply), result)`（reply 含 result 字段，_IOWR 回写 8B + result 独立通道，冗余兼容）
- user/driver/evdev.cc : `resp(data, data_len, result)`（纯数据 + result 分离，见 [evdev.md](evdev.md)）

### 数据流

inline 路径（arg ≤ 48B）：
```
客户端 sys_ioctl → req_data{cmd,arg,minor} → RECV_REQ 入队 → 驱动 recv→handle
  → sys_resp(reply_data, len, result) → copy_to_user(arg) + req_result=result → 唤醒
  → sys_ioctl return req_result
```

变长路径（arg > 48B）：
```
客户端 sys_ioctl → kmalloc+copy_from_user(arg) → RECV_IOCTL 入队 → 驱动 recv(data_buf)
  → kfree kbuf → handle → sys_resp(reply_data, len, result)
  → kmalloc+copy_from_user+CR3 switch+copy_to_user(arg) + req_result=result → 唤醒
  → sys_ioctl return req_result
```

### 测试

test_ioctl_varlen（user/test/test_ioctl_varlen.c，fork 父进程当 driver + 子进程当 client）：

| 用例 | 调用 | 期望 |
|------|------|------|
| 96B 变长 | _IOWR 96B | 0xBB 模式回写正确 |
| 边界 48/49 | 48B inline / 49B 变长 | 0xAA / 0xBB 模式分别正确 |
| 256B _IOR | 纯 _IOR 256B | EINVAL（libc 240B 客户端 cap） |
| 过大 | 256B _IOWR | EINVAL（libc 240B cap） |
| 超时 | driver 不 resp | ETIMEDOUT（3s） |

test_ioctl.c / test_dev_vfs.c：INPUT_BIND 断言读 ioctl 返回值 `r`（非 arg.result）。

注：libc `ioctl()` 用户态有 240B 栈 buffer cap，>240B 在客户端即 EINVAL，无法到达内核 64KB 上限。内核变长路径的 64KB cap 由 `uint16_t` 类型隐式保证。

### 与其他模块的关系

- **evdev**：evdev 进程当前仅处理 RECV_REQ（inline 路径），RECV_IOCTL 变长支持是待完成项。详见 [evdev.md](evdev.md)
- **IPC**：变长路径复用 sys_msg 的 kmalloc + CR3 switch 回写模式。详见 [ipc.md](kernel/ipc.md)
- **VFS**：dev_ops.minor 由 inline 路径填入 req_data[52..56] 供驱动路由。详见 [vfs.md](kernel/vfs.md)

## 待完成项

| 项目 | 说明 | 优先级 |
|------|------|--------|
| evdev 处理 RECV_IOCTL | evdev.cc 主循环增加 RECV_IOCTL 分支，recv 传 data_buf，支持 >48B getter（完整 KEY 位图 96B 等）。当前 `recv(&msg, NULL, 0, 0)` 传 data_buf=NULL，RECV_IOCTL 收到也返回 EINVAL。是接 libinput 前硬前置 | 高 |
| libc ioctl 客户端 cap 提升 | libc `ioctl()` 用户态 240B 栈 buffer 限制 >240B 命令在客户端即 EINVAL，无法到达内核 64KB 变长路径。需改为动态分配以支持完整 evdev 大 getter | 中 |
