# evdev 用户态驱动

## 当前架构设计

evdev 进程在用户态实现 evdev 子系统的查询接口，让 `EVIOCG*` 系列 ioctl 对齐 Linux/FreeBSD `input.h` 标准。客户端 `ioctl(fd, EVIOCG*)` → 内核按 `_IOC_SIZE(cmd)` 分流：`≤48B` 走 inline `RECV_REQ`（`req_data[56]` = cmd + arg + minor），`>48B` 走变长 `RECV_IOCTL`（kmalloc buffer + `recv_msg.ioctl.minor`）→ evdev 进程主循环按 `msg.type` 分流、统一调 `handle_ioctl(cmd, minor, src, grab_val)` 按 minor 路由、按 cmd 处理 → `sys_resp` 回传。

事件读取通过内核 `ringbuf_fops.read`（kernel/bsd/sysfs.c : ringbuf_read）从 SHM ring 读取 `struct input_event`（24B），consumer 通过 `read(/dev/input/event0)` 拉取。详见 [libinput.md](libinput.md)。

### 设计决策

| # | 决策 | 选择 | 理由 |
|---|------|------|------|
| 1 | 接口对齐范围 | 对齐头文件结构体/宏、ioctl 命令号编码；内核内部数据结构保持现有实现 | 接口对齐让上游 libinput 可编译，内核机制（dev_ops 形态、回调签名）无需改动 |
| 2 | `freebsd/input.h` 来源 | vendored 在 `third_party/libinput/include/linux/freebsd/`，编译加 `-I third_party/libinput/include/linux` 直接 `#include <freebsd/input.h>` | 不走 facade 转发、不定义 `__FreeBSD__`，保持 vendor 可同步；evdev 进程与 test_evdev 共用 |
| 3 | ioctl 命令号编码 | BSD `_IOWR/_IOC`（type='E'），经 `sys/ioccom.h` 桥接到项目 Linux 风格 `_IOC` | BSD 方向位值（IOC_OUT=2 等）与项目 `_IOC_READ`=2 数值一致，桥接后命令号自洽；与 INPUT_BIND 的 type='I' 不冲突 |
| 4 | minor 路由机制 | minor 存进 `dev_ops`，ioctl 转发填 `req_data[52..56]`，evdev 按 `devices[minor]` 路由 | minor 是唯一设备标识；`dev_ops` 已挂 i_priv，就地取零查找；不建 makedev/i_rdev（st_rdev 留后续 libinput path-seat） |
| 5 | `recv_msg` ABI | 零改动 | dev_id 是 evdev ioctl 专属需求，不该改通用 IPC 结构；minor 走 req_data 载荷尾部即可 |
| 6 | 设备模型 | 一个驱动进程管理多个子设备（/dev/input/eventN），按 minor 区分 | evdev 进程是长期基础设施，多设备路由为未来真实键盘鼠标接入预留 |
| 7 | 设备数据来源 | 静态桩（event0 键盘 + event1 路由探针），`#ifdef TEST` 门控 | `EVIOCG*` 查询元数据，与有无真实事件无关；静态桩让 test_evdev 单进程、无外部依赖、确定性。真实驱动接入是后续 |
| 8 | 变长 getter 回填长度 | 按 `_IOC_SIZE(cmd)` 截断 | 标准 evdev 语义：客户端 buf 长 len，`EVIOCGNAME(len)` 的 `_IOC_SIZE==len`，evdev 截到 len 字节填进 reply |
| 9 | grab 独占语义 | 每个 getter 前查 `grabbed && grab_client != src` → `-EBUSY`；`EVIOCGRAB` 自身绕过检查 | `msg.src` 是请求方 PID，据此判独占；grab 持有者必须能释放，故 EVIOCGRAB 不过检查 |
| 10 | SHM / read 路径 | 纯 ioctl 设备（`device_register_shm(..., -1, minor)`，shm_fd=-1 不绑 SHM） | 当前验收是 EVIOCG* 查询，与 SHM/read 无关；建 ring 而无 read 路径是死代码 |
| 11 | inline vs 变长路径 | `arg_size <= 48` 走 inline `RECV_REQ`（req_data[56]）；`>48` 走变长 `RECV_IOCTL`（kmalloc buffer） | 内核侧两条通道都已通（commit feedde1）；evdev 进程两条路径都已处理（B3 完成）。inline 路径 req_data[56] = cmd[4] + arg[≤48] + minor[4]；变长路径 minor 经 `recv_msg.ioctl.minor` 传递。evdev 主循环按 `msg.type` 分流，统一调 `handle_ioctl(cmd, minor, src, grab_val)` |

### 核心数据结构

evdev_device（user/driver/evdev.cc : evdev_device）：
  minor : uint16_t — 设备 minor（0,1,2...，devnum 自洽无固定基址）
  name : char[64] — 设备名（EVIOCGNAME 返回）
  id : struct input_id — {bustype, vendor, product, version}（EVIOCGID 返回）
  caps_bitmap : uint8_t[EV_CNT][96] — 按事件类型分的能力位图（EVIOCGBIT(ev,len) 返回 caps_bitmap[ev]，截到 len）。EV_CNT=32，每类 96B 覆盖 KEY_CNT（最大）。桩按 byte/bit 置位：`caps_bitmap[EV_SYN][EV_KEY/8] |= 1<<(EV_KEY%8)` 声明支持 EV_KEY，`caps_bitmap[EV_KEY][KEY_A/8] |= 1<<(KEY_A%8)` 声明 KEY_A
  prop_bitmap : uint32_t — 属性位图（EVIOCGPROP 返回）
  grabbed : bool — 是否被独占
  grab_client : pid_t — 独占持有者 PID

静态桩设备（user/driver/evdev.cc : register_stubs，`#ifdef TEST` 门控）：
  event0（minor=0）— name="evdev keyboard"，id={BUS_USB, 0x0001, 0x0001, 0x0001}，caps_bitmap[EV_SYN] 置 EV_KEY 位、caps_bitmap[EV_KEY] 置 KEY_A 位（byte 3 bit 6），prop=0
  event1（minor=1）— name="evdev test dev"，id={0,0,0,0}，caps=0，prop=0（路由探针，只验 minor=1 路由）

桩节点是叠加在进程上的测试夹具：`--test` 构建（`-DTEST=1`）下 evdev 额外绑定 event0/event1；正常构建不建桩节点，进程空等真实驱动接入。桩只填查询元数据，不抢占任何输入源 —— 未来真实键盘接入有两条路：替换桩（event0 元数据从桩变真实，节点路径不变）或加新设备（新 minor，多设备路由承接）。

### 关键流程

evdev 进程主循环（user/driver/evdev.cc : main）：
1. `#ifdef TEST`：`register_stubs()` 建桩设备 + `device_register_shm("input/eventN", -1, minor)` 注册节点
2. `recv(&msg, data_buf, 256, 0)` 阻塞等消息；`data_buf` 接收变长 RECV_IOCTL 的 arg 数据（kernel 从 kmalloc buffer copy 出并 kfree），传 NULL 会被 kernel 以 EINVAL 拒绝并丢请求
3. 按 `msg.type` 分流：RECV_REQ（inline，arg≤48B）从 `msg.data[0..4]` 读 cmd、`msg.data[52..56]` 读 minor、WRITE 方向从 `msg.data[4..]` 读 grab_val；RECV_IOCTL（变长，arg>48B）从 `msg.ioctl.cmd` / `msg.ioctl.minor` 读（minor 由内核 `sys_ioctl` 变长路径填入 `recv_msg.ioctl.minor`）
4. 统一调 `handle_ioctl(cmd, minor, src, grab_val)` 处理（变长路径 grab_val 传 0；EVIOCGRAB 的 `_IOC_SIZE==sizeof(int)≤48` 永远走 inline）

handle_ioctl 分派（user/driver/evdev.cc : handle_ioctl）：
1. `find_device(minor)` 路由，未找到 → `resp(NULL, 0, -ENODEV)`
3. grab 检查：`cmd != EVIOCGRAB && dev->grabbed && dev->grab_client != src` → `resp(NULL, 0, -EBUSY)`
4. 按 `_IOC_NR(cmd)` 分派：

| ioctl | _IOC_NR | 行为 |
|-------|---------|------|
| EVIOCGVERSION | 0x01 | data 写 EV_VERSION |
| EVIOCGID | 0x02 | data 写 dev->id |
| EVIOCGNAME(len) | 0x06 | data 写 dev->name（截到 `_IOC_SIZE`） |
| EVIOCGPROP(len) | 0x09 | data 写 dev->prop_bitmap（截到 `_IOC_SIZE`） |
| EVIOCGBIT(ev,len) | 0x20..0x3F | data 写 dev->caps_bitmap[ev]（ev=nr-0x20，截到 `_IOC_SIZE`） |
| EVIOCGABS(abs) | 0x40..0x7F | result = -ENOSYS（桩无 ABS 轴） |
| EVIOCGRAB | 0x90 | arg=1 设置 grabbed/grab_client=src，arg=0 清除；无 data 返回 |
| 其它 | — | result = -ENOSYS |

5. `resp(data, data_len, result)` 回传

### 回复契约

`resp(data, data_len, result)`（user/lib/sys_ipc.cc : resp → sys_resp）三参分离：
- `result`（int32）写到 `caller->req_result`，作为客户端 ioctl 返回值（0 成功 / 负 errno）
- `data`（≤56B 纯数据）copy 到 `caller->req_reply_buf`（客户端 ioctl 的 arg 缓冲区），copy_len = `min(data_len, req_reply_len)`

`req_reply_len = arg_size`（即 `_IOC_SIZE(cmd)`，kernel/bsd/syscall.c : sys_ioctl inline 路径）。客户端 `ioctl(fd, EVIOCGNAME(buf), buf)`：返回值 = result，buf 被 data 回填名字。

### 内核侧 minor 路由

3 项改动：

| # | 改动 | 位置 |
|---|------|------|
| 1 | `dev_ops` 加 `uint32_t minor` 字段 | kernel/bsd/devtmpfs.h : dev_ops |
| 2 | `sys_dev_create` 扩 3 参 `(name, shm_fd, minor)` | kernel/bsd/vfs.c : sys_dev_create（dispatch 已传 6 寄存器，ABI 不动；现有 2 参调用方默认 minor=0） |
| 3 | `sys_ioctl` FD_DEV req 填 minor | kernel/bsd/syscall.c : sys_ioctl（`*(uint32_t*)(req_data+52) = ops->minor`） |

user 态：`device_register_shm(name, shm_fd, minor)`（user/lib/sys_device.cc : device_register_shm）3 参，现有调用方（kbd_driver）传 minor=0。

req_data 载荷布局（56B，inline 路径）：
  [0..4) = cmd
  [4..4+arg_size) = arg 数据（arg_size = `_IOC_SIZE(cmd)` ≤ 48）
  [52..56) = minor

arg 从 offset 4 起，长度 ≤48 最多到 offset 52；minor 放 [52..56) 正好在 arg 之后不冲突。EVIOCG* 全 READ 无 WRITE，arg 不拷进 req_data，`req_data=[cmd][...zeros...][minor]`。

### 接口定义

vendored 头文件（third_party/libinput/include/linux/freebsd/）：
- `freebsd/input.h` — 结构体（24B `struct input_event` 含 timeval、`struct input_id`、`struct input_absinfo`）+ ioctl 命令号（BSD `_IOWR/_IOC` 编码，type='E'）
- `freebsd/input-event-codes.h` — 事件码常量（EV_KEY/KEY_A/ABS_X...）

`freebsd/input.h` 依赖 `<sys/time.h>`（已有）与 `<sys/ioccom.h>`（本轮新建）。event-codes 用同目录相对 include，外部只配 `-I third_party/libinput/include/linux`。

sys/ioccom.h 桥接层（user/include/sys/ioccom.h）：
  IOC_OUT = _IOC_READ（2）
  IOC_IN = _IOC_WRITE（1）
  IOC_INOUT = _IOC_READ | _IOC_WRITE（3）
  IOC_VOID = _IOC_NONE（0）
  _IOWINT(type, nr) = _IOW(type, nr, int)

BSD ioccom.h 方向位值与项目 ioctl.h 的 `_IOC_NONE/_IOC_WRITE/_IOC_READ` 数值一致，故 BSD 编码的 `EVIOCGNAME=_IOC(IOC_OUT,'E',0x06,len)` 与 Linux 风格 `_IOR('E',0x06,...)` 命令号完全相同，内核 `_IOC_DIR(cmd)` 解出方向走对应路径，无需特判。vendor input.h 零改动，保持可同步。

### 启动流程

```
kernel_main → devtmpfs_init()

init 进程（init/init.c : main）
  → spawn_service("/driver/kbd.dev"); wait_dev_ready("/dev/kbd")
  → spawn_service("/driver/evdev.dev")          // 无条件 spawn，不 wait
  → spawn_service("/usr/bin/terminal")

evdev 进程（user/driver/evdev.cc : main）
  → #ifdef TEST: register_stubs() 建 event0/event1 桩节点
  → 主循环 recv→handle_req，等真实驱动接入（正常构建空等）

test_evdev（仅 --test 构建，由 test_runner 串行调起）
  → wait_dev_ready("/dev/input/event0")
  → open event0 → EVIOCG* 断言；open event1 → 路由断言；fork → 真独占断言
```

`wait_dev_ready`（user/lib/dev_ready.c）轮询 `open(O_RDWR)`，失败则 recv 等 10 tick 重试，init 与 test_evdev 共用。

### 测试

test_evdev（user/test/test_evdev.c，仅 `if(TEST)` 构建）11 个用例：

| 用例 | 调用 | 期望 |
|------|------|------|
| 版本 | `EVIOCGVERSION` | v == EV_VERSION |
| 标识 | `EVIOCGID` | bustype=0x03, vendor/product/version=0x0001 |
| 名称 | `EVIOCGNAME(48)` | "evdev keyboard" |
| 能力位图(KEY) | `EVIOCGBIT(EV_KEY,48)` | KEY_A(bit 30) 位置位 |
| 能力位图(KEY,变长) | `EVIOCGBIT(EV_KEY,96)` | KEY_A 位置位（96B>48B 走 RECV_IOCTL 变长路径，验不再静默丢弃） |
| 能力位图(ABS) | `EVIOCGBIT(EV_ABS,8)` | buf 全 0（桩无 ABS） |
| 属性 | `EVIOCGPROP(4)` | buf 全 0 |
| ABS 信息 | `EVIOCGABS(ABS_X)` | 返回 -ENOSYS |
| 独占(grab) | `EVIOCGRAB` 1/0 | 均返回 0 |
| 真独占语义 | 父 grab event0 → fork 子 open+EVIOCGVERSION | 子返回 -EBUSY |
| 路由 | open event1 → EVIOCGVERSION | 返回 EV_VERSION（验 minor=1 路由） |

真独占语义测试：父进程 open event0 + EVIOCGRAB(1) → fork 子进程 open event0 + EVIOCGVERSION，期望 -EBUSY；子退出后父 EVIOCGRAB(0) 释放。

### 与其他模块的关系

- **devtmpfs**：evdev 通过 `device_register_shm` 注册 /dev/input/eventN 节点，`dev_ops.minor` 由内核 ioctl 转发回填。详见 [vfs.md](kernel/vfs.md)
- **IPC**：ioctl 查询走内核 req/resp 转发（inline `RECV_REQ` + 变长 `RECV_IOCTL` + sys_resp）。详见 [ipc.md](kernel/ipc.md)
- **用户态驱动**：evdev 是 IOPL=0 的用户态驱动进程，与 kbd_driver 同模式。详见 [user_driver.md](user_driver.md)
- **键盘**：kbd_driver 是当前真实键盘事件源（USB HID → input SHM ring → terminal）；evdev 接入真实键盘是后续工作。详见 [kbd.md](kbd.md)

## 待完成项

| 项目 | 说明 | 优先级 |
|------|------|--------|
| grab 僵尸锁 | `file_put` 对 user-driver 不调 close，客户端崩溃/关 fd 后 grab_client 残留 → eventN 永久 EBUSY。需 close 通知驱动清除 grab 状态 | 中 |
| 真实底层驱动 → evdev 事件桥接 | kbd_driver 现走 → /dev/kbd → terminal 链；evdev 接入需决定两套 driver 并存还是合并。届时 evdev.cc 同时用 16B xos input_event 与 24B linux struct input_event，需 rename/隔离 | 中 |
| stat().st_rdev + lookup_devt | libinput path-seat 的 udev devnum 反查前置；当前按路径 open 不触发。届时再建 makedev/i_rdev | 低 |
