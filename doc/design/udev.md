# udev

> libudev 兼容层。Linux libudev API 的微内核 OS 实现，使 libinput 等外部库以 drop-in 方式链接运行。
> property 由 udevd 规则引擎算好存 `/run/udev/data/` db（非 shim 现算），对齐 Linux `input_id` builtin。
> 相关：`doc/design/sysfs.md`（属性真文件 + 可写 uevent）、`doc/design/kernel/netlink.md`（uevent 广播）、`doc/design/evdev.md`（evdev 用户态驱动）、`doc/design/kernel/vfs.md`（tmpfs + SYS_RENAME）、`doc/design/boot.md`（init socket activation + settled gate）。

## 设计哲学

对齐 Linux **机制**（不是表象）：

- **property 来自 udevd db**：`ID_INPUT_*` 由 udevd 规则引擎（`input_id` builtin）开 `/dev` 跑 `EVIOCGBIT` 算好存 db，client `udev_device_get_property_value` 直读 db 文件（对齐 Linux `input_id` builtin），非 shim 每次 `open(devnode)` 现算。
- **db 原子写 tmp+rename**：db 文件 per-device keyed by devnum，写 tmp → `rename()` 原子覆盖（对齐 Linux `WRITE_STRING_FILE_ATOMIC`），读者永不读到半截。原子写依赖本 OS 新增的 `SYS_RENAME` + tmpfs rename。
- **db 落 tmpfs 不随 crash 丢**：db 是 tmpfs 文件（内核 RAM），独立于 udevd 进程生命周期——udevd crash 后 init respawn，新 udevd 读回 db。
- **socket activation**：init 建 `/run/udev/socket` listen socket 传 udevd（非 udevd 自 bind），udevd crash 时 socket fd 不随进程死，respawn 复用同一 listen fd。
- **coldplug 用 Linux 标准写法**：sysfs `uevent` 属性可写，udevd 启动写 `add` 触发内核重广播，走与热插拔完全相同的 netlink 路径。

## 当前架构设计

### 分层架构

```
┌─────────────────────────────────────────────────────────┐
│  libinput（外部库，drop-in 链接 libudev.a）              │
│  path-seat：显式 /dev/input/eventX 初始化（不依赖 db）   │
│  monitor：热插拔监听（未实现，shim no-op stub）          │
└────────────────┬────────────────────────────────────────┘
                 │ 链接 libudev.a（静态，udev.c 编入 LIBINPUT_SOURCES）
                 ▼
┌─────────────────────────────────────────────────────────┐
│  libudev shim（user/lib/udev-shim/udev.c）               │
│  path-seat 直读：opendir /dev/input + stat + /sys 属性   │
│  property 查询：直读 /run/udev/data/<key> db 文件        │
│  枚举过滤：device_is_keyboard（保留 EVIOCGBIT 现算）     │
└────────────────┬────────────────────────────────────────┘
                 │ open+read tmpfs db 文件 / open /dev
                 ▼
┌─────────────────────────────────────────────────────────┐
│  /run/udev/data/<key>  ← tmpfs 文件（内核 RAM，crash 不丢）│
│  key = devnum（= stat.st_rdev = ino，三边一致）          │
│  格式：纯文本 KEY=VALUE\n                                │
└──────────────────────▲──────────────────────────────────┘
                       │ tmp+rename 原子写（SYS_RENAME）
┌──────────────────────┴──────────────────────────────────┐
│  udevd（user/udev/udevd.c，db 写者 + 规则引擎 + coldplug）│
│  netlink 订阅 uevent → handle_uevent_add：               │
│    stat /dev 取 devnum → input_id_compute 探 caps         │
│    → 合成 ID_INPUT_* → tmp+rename 原子写 db              │
│  coldplug：coldplug_trigger 写 /sys/.../uevent 重广播 +   │
│    coldplug_drain_settle 排干建 /run/udev/settled        │
│  socket activation：探测 fd 3 listen socket（accept 待实现）│
└────────────────┬────────────────────────────────────────┘
                 │ EVIOCGBIT ioctl（变长 RECV_IOCTL 通道）
                 ▼
┌─────────────────────────────────────────────────────────┐
│  内核基座                                                │
│  netlink：nl_uevent_broadcast（uevent 广播）             │
│  sysfs：/sys/class/<subsys>/<dev>/<attr> 真文件          │
│         + 可写 uevent 属性（coldplug 重广播）            │
│  devtmpfs：/dev 设备节点 + getdents 枚举                 │
│  sys_dev_set_meta(96)：evdev 两步注册第二步               │
│  tmpfs + SYS_RENAME(98)：db 原子写基座                   │
└─────────────────────────────────────────────────────────┘
```

### 设计决策

| # | 决策 | 选择 | 理由 |
|---|------|------|------|
| 1 | 通知通道 | netlink `nl_uevent_broadcast` | netlink 已落地，多订阅者广播；废弃原 pipe(86)+ring(88)（86/87/88 已分配给 epoll） |
| 2 | 属性来源 | sysfs 真文件（`/sys/class/<subsys>/<dev>/<attr>`） | 废弃 udevd 内存库虚拟 syspath；shim 直读对齐 Linux |
| 3 | /dev 枚举 | `SYS_GETDENTS`(50) + devtmpfs fstype callback | 复用通用 getdents；废弃独立 87 号 syscall |
| 4 | property 来源 | udevd db（`/run/udev/data/<key>`），client 直读 | 对齐 Linux `input_id` builtin：property 由 udevd 规则引擎算好存 db，client 直读，非 shim 现算 |
| 5 | 库形态 | `libudev.a` 静态链接（`udev.c` 直接编入 libinput） | 无独立 .a，无动态加载需求 |
| 6 | devnum 映射 | `devnum = stat.st_rdev = ino`（三边一致） | 内核 stat/shim 存储/udevd db key 同一 ino；不合成 Linux major/minor（会打破三边一致） |
| 7 | db key | `devnum` 十进制数值字符串 | 对齐 Linux `/run/udev/data/c<major>:<minor>`；本 OS 无 major/minor，简化为 devnum |
| 8 | db 原子写 | tmp 文件 + `rename()` 覆盖 | 对齐 Linux `WRITE_STRING_FILE_ATOMIC`；读者要么旧要么新，无半截。依赖 `SYS_RENAME`(98) |
| 9 | db 存储 | tmpfs 文件（非进程内存） | 进程 crash 不丢（对齐 Linux）；tmpfs 内核 RAM 重启清空，coldplug 重建 |
| 10 | coldplug | udevd 写 `/sys/.../uevent` 触发内核重广播 | 对齐 Linux `udevadm trigger`（`echo add > /sys/.../uevent`）；走与热插拔同款 netlink 路径 |
| 11 | caps 探测 | udevd 开 /dev 跑 `EVIOCGBIT`（变长 RECV_IOCTL 通道） | 对齐 Linux `input_id` builtin；caps 不进 sysfs/uevent（`input_dev_props` 只携 identity） |
| 12 | udevd 启动 | socket activation（init 建 listen socket 传 udevd） | 对齐 systemd-udevd；crash 时 socket fd 不随进程死，respawn 复用 |

### 核心数据结构

`struct udev` / `struct udev_device` / `struct udev_enumerate` / `struct udev_monitor` / `struct udev_list_entry`（user/lib/udev-shim/libudev.h）— 与 Linux libudev ABI 一致。

`udev_device` 关键字段：
- syspath : char[] — `/sys/class/<subsys>/<dev>`（真实路径）
- devnode : char[] — `/dev/input/eventX`
- sysname : char[] — `eventX`（syspath 末段）
- subsystem / devtype : char[]
- devnum : dev_t — = stat.st_rdev（ino），作 udevd db key
- refcount

### udevd db

**路径与 key：** `/run/udev/data/<key>`，`key = devnum` 十进制数值字符串（`devnum = stat.st_rdev = ino`，三边一致）。例 `/dev/input/event0` ino=`0x80000005` → `/run/udev/data/2147483653`。

> **偏离 Linux：** Linux key = `c<major>:<minor>`。本 OS 无 major/minor（三边一致用 ino，合成 major/minor 会打破三边一致），简化为 devnum 数值字符串。本 OS db 场景全是字符设备（input/dri），无块设备 db 需求。

**文件格式：** 纯文本，逐行 `KEY=VALUE\n`（对齐 Linux udev db 格式）。当前键盘类设备：

```
ID_INPUT=1
ID_INPUT_KEYBOARD=1
ID_INPUT_KEY=1
ID_SEAT=seat0
```

**原子写（tmp+rename）：** 写 `/run/udev/data/<key>.tmp` → `rename()` 覆盖 `/run/udev/data/<key>`。rename 是目录项原子替换，读者要么旧要么新。详见 [vfs.md](kernel/vfs.md) tmpfs rename 节 + `SYS_RENAME`(98)。

**约束：**
- 单文件 64KB 上限（`TMPFS_FILE_CAP`）+ 总量 1MB（`TMPFS_TOTAL_CAP`，含 socket inode）。db KV 远不到（per-device KB 级 × N 设备，N<1000 充裕）。
- tmpfs 无 fsync（内存 fs 不需要），crash-safety 靠 tmpfs inode 在内核 RAM 独立于 udevd 进程。
- udevd 单线程无锁，db 文件 IO 串行无并发；rename 原子性保证 udevd 写 + client 读无半截读。

**接口（`user/udev/udevd.c:83-152`）：**

| 函数 | 说明 |
|------|------|
| `db_write_property(devnum, kv_str, kv_len)` | tmp+rename 原子写 db；失败 `unlink(tmp)` 清理 |
| `db_read_all(devnum, buf, bufcap)` | 读 db 全量到 buf（client 侧）；db 不存在返 `-ENOENT` |
| `db_remove(devnum)` | 删 db 文件（remove 两阶段真删用） |

### 规则引擎 input_id builtin

**`input_id_compute(devnode, devnum)`（`udevd.c:157-211`）** 对齐 Linux `src/udev/udev-builtin-input_id.c`：

1. `open("/dev/input/eventX")` + `EVIOCGBIT(0)` 取事件类型位图（变长走 RECV_IOCTL 通道代理到 evdev 进程，详见 [evdev.md](evdev.md) / [ioctl.md](ioctl.md)）；
2. 任何 `EV_KEY`/`EV_REL`/`EV_ABS` → `ID_INPUT=1`；
3. `EV_KEY` 且有键盘类按键（`KEY_A`/`KEY_ENTER`/`KEY_SPACE`/`KEY_LEFTCTRL` 任一）→ `ID_INPUT_KEYBOARD=1` + `ID_INPUT_KEY=1`（第二轮 `EVIOCGBIT(EV_KEY)` 取按键位图）；
4. `ID_SEAT=seat0`（对齐 Linux 永远 seat0）；
5. 合成 KV → `db_write_property` 原子写 db。

**本轮范围（键盘类子集）：** event0 现状纯键盘 caps（evdev `init_caps` 只填 EV_KEY）。MOUSE（`EV_REL`+`REL_X/Y`）/ TOUCHPAD（`BTN_TOOL_FINGER`+`ABS_X/Y`）依赖真实多设备 caps，本轮不探 `EV_REL`/`EV_ABS`/`EVIOCGABS`，记 todo（B6 延后）。

**uevent 入口 `handle_uevent_add(devname, subsystem)`（`udevd.c:214-235`）：**
- `stat("/dev/<name>")` 取 devnum（=ino）作 db key；`/dev` 未就绪跳过；devnum=0 跳过；
- 仅 input 子系统跑 input_id（对齐 Linux input_id 只处理 input 设备）；
- `process_one_uevent`（`:240`）解析 `\0` 分隔 uevent payload 取 ACTION/DEVPATH/SUBSYSTEM，add 调 `handle_uevent_add`，drain 与主循环共用。

### coldplug

**问题：** 启动时序 evdev 驱动先起、两步注册产出 `/dev/input/eventX` + sysfs 属性树（step 2 `nl_uevent_broadcast("add",...)`）→ **然后** udevd 才被 init spawn。`nl_uevent_broadcast` 是 fire-and-forget（无 listener 即丢），udevd 订阅前的 add uevent 被丢 → `input_id_compute` 从未跑 → db 从不建 → shim 读 db 为空 → libinput 判 unsupported → terminal block 黑屏。

**对齐 Linux coldplug：** sysfs `uevent` 属性可写（详见 [sysfs.md](sysfs.md)）。udevd 启动时 `coldplug_trigger`（`udevd.c:265`）扫 `/sys/class/input`，对每个现有设备 `write("add", "/sys/class/input/<sysname>/uevent")` → 内核 `uevent_store` → `nl_uevent_broadcast("add",...)` 重广播走与热插拔完全相同的 netlink 路径。

**settle（对齐 systemd udev settle）：** `coldplug_drain_settle`（`udevd.c:289`）用 `MSG_DONTWAIT` 非阻塞排干 trigger 产的 uevent，每个 add 调 `handle_uevent_add` 写 db，排干后建 `/run/udev/settled` 标志。init 在 spawn terminal 前轮询 `access("/run/udev/settled", F_OK)`（最多 ~2s，超时仍 spawn 退化为原行为），保证 db 就绪再起 terminal（详见 [boot.md](boot.md)）。

**偏离 Linux（写明）：**
- 扫 `/sys/class/input` 而非 `/sys/devices/**` 全树（本 OS sysfs 只有 class 视图，无 /sys/devices 树，记 todo）；
- trigger 只产 `add`（`uevent_store` 只接受 `"add"`，remove/change 记 todo）；
- uevent 文件只写不读（Linux 可读，记 todo）；
- settle 用文件标志 + init 轮询而非 `udevadm settle` 命令通道。

### shim path-seat 直读

**设备发现与属性查询（`user/lib/udev-shim/udev.c`）：**
1. `scan_devices` — `opendir("/dev/input")` + stat + `device_is_keyboard`（`udev.c:87`，`EVIOCGBIT(0)`）过滤键盘，建静态设备表；
2. `create_udev_device` — syspath 构造为 `/sys/class/<subsys>/<dev>`；`devnum = stat.st_rdev`；
3. `udev_device_new_from_devnum` — `find_device_by_devnum` + 扫描 fallback；
4. `udev_device_get_sysattr_value(attr)` — `open(syspath + "/id/" + attr)`(input) 或 `syspath + "/" + attr`(drm) + read 到静态缓冲区；
5. `udev_device_get_property_value(key)`（`udev.c:276-315`）— **直读 db 文件**：`open("/run/udev/data/<devnum>")` + read + 解析 `KEY=VALUE\n` 找指定 key。db 不存在返 NULL（降级，path-seat 仍 work）。

**与枚举过滤的边界：** `device_is_keyboard` 是设备**枚举**过滤（`scan_devices` 挑键盘设备用），走 `open(devnode)`+`EVIOCGBIT(0)`，非 property 查询。本轮只对齐 property 路径到 db，枚举过滤保持现状（偏离 Linux：Linux 枚举也走 db 查 property，记 todo——改动面大且枚举频率低）。

### 关键流程

**通知：netlink uevent 广播**（kernel/bsd/netlink.c : nl_uevent_broadcast）
- devtmpfs_create（ops->subsystem 非 NULL）/ remove / cleanup_pid 三处调用；
- `sys_dev_set_meta`(96) 第二步设备就绪时调用；
- sysfs 可写 uevent 的 `uevent_store` 写 `add` → 重广播（coldplug 路径）；
- payload Linux 格式：`"action@devpath\0ACTION=action\0DEVPATH=devpath\0SUBSYSTEM=subsystem\0"`，nlmsghdr 包裹，`nlmsg_type`=ADD/REMOVE/CHANGE。`devpath` 实参为设备 name（如 `input/event0`），非 Linux `/devices/...` 路径；
- group bit 0（NETLINK_KOBJECT_UEVENT），`nl_groups[NL_MAX_GROUPS=32]` 链表 fan-out，recv_queue 上限 256 drop-oldest。

**局限：** payload 仅 3 键（ACTION/DEVPATH/SUBSYSTEM），无 SEQNUM/MODALIAS/DEVNAME/MAJOR/MINOR，buffer 上限 256B。这是 udevd `handle_uevent_add` 必须 `stat /dev` 补全 device 标识的根因。

**devnum 三边一致：** `devnum = stat.st_rdev = ino`，内核 stat / shim 存储 / udevd db key 同一 ino 值；不合成 Linux major/minor（会打破三边一致）。

**evdev 两步注册（对齐 Linux allocate→fill→register，详见 [sysfs.md](sysfs.md)）：**
1. `device_register_shm("input/event0", shm_fd, minor)` → `devtmpfs_create` 建 `/dev` 节点 + 绑 SHM，**不推 uevent**；
2. `device_set_meta("input/event0", "input", "evdev", &props)` → `sys_dev_set_meta`(96)：填 dev_ops.subsystem/devtype、kmalloc `input_dev_props` 填 subsys_priv、建 sysfs 子树（含可写 uevent 属性）、推 `nl_uevent_broadcast("add",...)`；
3. udevd 只在 step 2 后感知设备，无竞态窗口。

### 启动流程

```
UEFI → BOOTX64.EFI → myos.elf → kernel_main
  → VFS/FAT32 初始化（vfs_init 挂 tmpfs /run）
  → bsd_init：vfs_init + mount sysfs + netlink_init
  → init.elf（PID 1）
       ├─ open("/dev/serial") + dup2 stdio
       ├─ spawn evdev 驱动（两步注册产出 /dev/input/event0 + sysfs 属性树）
       ├─ create_udev_socket：mkdir /run/udev + socket+bind+listen(/run/udev/socket)
       │  → spawn_with_fd("/usr/bin/udevd", listen_fd)（fd 3 经继承传入）
       ├─ udevd main()
       │    ├─ 探测 fd 3 listen socket（否则 fallback 自 bind+listen）
       │    ├─ mkdir /run/udev/data（db 落点）
       │    ├─ netlink 订阅 group 1 + epoll
       │    ├─ coldplug_trigger + coldplug_drain_settle（建 /run/udev/settled）
       │    └─ epoll_wait 主循环：netlink uevent → handle_uevent_add 写 db
       ├─ wait_dev_ready("/dev/input/event0")
       ├─ 轮询 access("/run/udev/settled", F_OK)（~2s，db 就绪门控）
       ├─ spawn terminal（libinput path-seat，直读 /sys + stat /dev + 查 db property）
       └─ 收尸循环 waitpid(-1) → udevd crash respawn（退避 1s，StartLimitBurst=5）
```

### 系统调用/接口

新增 1 个 syscall（db 原子写基座）：

| syscall | 号 | 用途 |
|---------|----|------|
| `SYS_RENAME` | 98 | db 原子写 tmp+rename 覆盖（完整 rename(2) 语义，详见 [vfs.md](kernel/vfs.md) tmpfs rename 节） |

复用清单：

| syscall | 用途 | 接入点 |
|---------|------|--------|
| `SYS_GETDENTS`(50) | /dev、/sys/class 枚举 | shim scan_devices / udevd coldplug 扫 /sys/class |
| `SYS_DEV_SET_META`(96) | evdev 两步注册第二步（sysfs 建树 + uevent） | evdev 驱动调 |
| `SYS_IOCTL`(EVIOCGBIT) | 探设备 caps（变长 RECV_IOCTL 通道） | udevd `input_id_compute` |
| `SYS_STAT` | stat /dev 取 st_rdev(=ino) 作 db key | `handle_uevent_add` |
| `SYS_OPEN`/`READ`/`WRITE`/`CLOSE` | 开 /dev 探 caps；开 db 文件读写 | udevd + shim |
| `SYS_MKDIR` | mkdir /run/udev + /run/udev/data | init + udevd |
| `SYS_UNLINK` | 删 db 文件（remove 两阶段真删） | udevd |
| `SYS_SOCKET`/`BIND`/`LISTEN`/`ACCEPT` | socket activation | init 建 listen socket / udevd 探测 fd 3 |
| `socket(AF_NETLINK,...)` | 订阅 uevent | udevd netlink fd |
| `SYS_EPOLL_*` | udevd 事件循环 | udevd |

### 与其他模块的关系

- `doc/design/sysfs.md`：shim 读 `/sys/class/.../<attr>` 真文件；evdev 两步注册建树；**可写 uevent 属性**（coldplug 重广播）
- `doc/design/kernel/netlink.md`：uevent 广播机制 + coldplug 重广播路径
- `doc/design/evdev.md`：evdev 用户态驱动产出 sysfs 属性 + SHM ring 事件流 + caps 提供者
- `doc/design/kernel/vfs.md`：tmpfs `/run` + `SYS_RENAME`(98) tmpfs rename（db 原子写基座）
- `doc/design/boot.md`：init socket activation + settled gate + udevd crash respawn

### 与 Linux 偏离清单

| 项 | Linux | 本 OS | 偏离性质 |
|----|-------|-------|---------|
| db key | `c<major>:<minor>` | devnum 数值字符串 | 无 major/minor（三边一致用 ino），简化 |
| DEVPATH | `/devices/...` 路径 | 设备 name（如 `input/event0`） | sysfs 无 /sys/devices 树；udevd 据 name 构造 /dev+syspath |
| uevent payload | SEQNUM/DEVNAME/MAJOR/MINOR/MODALIAS 等 | 3 键（ACTION/DEVPATH/SUBSYSTEM） | netlink 精简；udevd 补全时 stat /dev + 读 /sys |
| db 写原子性 | tmp+fsync+rename | tmp+rename（无 fsync） | tmpfs 内存 fs 不需 fsync；crash-safety 靠内核 RAM |
| ID_INPUT_* 范围 | KEYBOARD/KEY/MOUSE/TOUCHPAD/... 全类 | 仅 KEYBOARD/KEY/SEAT（键盘类子集） | B6 延后到真实 evdev 多设备前置 |
| shim 设备枚举 | 走 db 查 property 过滤 | `device_is_keyboard` 仍 open+EVIOCGBIT 现算 | 枚举路径本轮不对齐；只对齐 property 路径，记 todo |
| uevent 属性 | 可读可写 | 只写不读 | 本轮只 coldplug 写用，可读记 todo |
| coldplug 扫描范围 | /sys/devices 全树 | /sys/class/input | sysfs 无 /sys/devices 树，记 todo |
| coldplug action | add/remove/change | 只 add | 本轮只 input coldplug，其余记 todo |
| settled 语义 | `udevadm settle` 命令通道 | 文件标志 + init 轮询 | 无 IPC 命令通道 |

## 待完成项

| 项目 | 说明 | 优先级 |
|------|------|--------|
| monitor 实现 | shim `udev_monitor_*` 已全真实实现（commit `221c373`，详见根目录 `udev_design.md`）：`enable_receiving` AF_UNIX connect `/run/udev/socket` + `recvmsg` SCM_RIGHTS 收 pipe rd fd；`get_fd` 返 pipe rd fd（可 epoll）；`receive_device` read pipe + 解析 `\0` 分隔 KV + 建 `udev_device`。**唯一 stub = `filter_add_match_subsystem_devtype`**（返 0，no-op，本轮不实现）。path-seat 不依赖 monitor | ✅ 已落地 |
| dev_entries 动态化（B5） | `dev_entries[MAX_DEV_ENTRIES=32]` 仍为静态数组；改 kmalloc 动态节点 + 链表（含 `dev_dirs[16]`），删静态上限。DRI/input/serial/pty 并发易撑满 | 低 |
| B6 ID_INPUT_MOUSE/TOUCHPAD/SEAT 全类 | 依赖 evdev 真实多设备 caps + 每设备独立 caps（evdev 驱动长期基础设施）。做合成器（Wayland）时连同真实多输入设备一起做 | 低 |
| shim 枚举路径对齐 db | `device_is_keyboard` 改走 db 查 property（对齐 Linux），删 EVIOCGBIT 现算 | 低 |
| uevent 属性可读 | 现 `show=NULL`（只写不读）；Linux 可读输出当前 uevent KV | 低 |
| uevent 接受 remove/change | `uevent_store` 只接受 `"add"`；补 remove/change | 低 |
| coldplug 扫 /sys/devices 全树 | 现只扫 /sys/class/input；sysfs 补 /sys/devices 树后全扫 | 低 |
| udevd daemon 通道完整 | AF_UNIX accept + pipe SCM_RIGHTS 转发 + client 重连（crash 恢复），详见 `udev_design.md` | 中 |
| udevd db remove 两阶段 | remove 时用 db 快照补全转发后才真删（依赖 monitor 管道） | 中 |
| 测试补全 | `test_rename.c` + `test_udevd_db.c` 已落地接线。`test_dev_notify.c` 废弃（`SYS_DEV_NOTIFY`(88) 已被 epoll 占用，uevent 走 netlink） | — |
