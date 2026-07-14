# udev

> libudev 兼容层。Linux libudev API 的微内核 OS 实现，使 libinput 等外部库以 drop-in 方式链接运行。
> 相关：`doc/design/sysfs.md`（属性真文件）、`doc/design/kernel/netlink.md`（uevent 广播）、`doc/design/evdev.md`（evdev 用户态驱动）。

## 当前架构设计

### 设计决策

| # | 决策 | 选择 | 理由 |
|---|------|------|------|
| 1 | 通知通道 | netlink `nl_uevent_broadcast` | netlink 已落地，多订阅者广播；废弃原 pipe(86)+ring(88)（86/87/88 已分配给 epoll） |
| 2 | 属性来源 | sysfs 真文件（`/sys/class/<subsys>/<dev>/<attr>`） | 废弃 udevd 内存库虚拟 syspath；shim 直读对齐 Linux |
| 3 | /dev 枚举 | `SYS_GETDENTS`(50) + devtmpfs fstype callback | 复用通用 getdents；废弃独立 87 号 syscall |
| 4 | 架构形态 | 阶段 A = netlink + shim（无 daemon 依赖） | libinput path-seat 模式显式指定设备路径，不依赖 monitor；无单点故障 |
| 5 | 库形态 | `libudev.a` 静态链接 | 无动态加载需求，链接进 libinput |
| 6 | devnum 映射 | `devnum = stat.st_rdev = ino`（三边一致） | 内核 stat/shim 存储/libinput 传参同一 ino 值即可匹配；不合成 Linux major/minor（会打破三边一致） |
| 7 | ID_INPUT_* 合成 | shim 每次 `open(devnode)` + `EVIOCGBIT` 直探 | path-seat 初始化时查询频率低，open/ioctl/close 开销可接受；阶段 B 可改读 sysfs caps |
| 8 | udevd 角色 | 阶段 A 维持 netlink 日志骨架 | 仅打印 uevent；path-seat 不依赖 udevd，crash 无影响 |

### 核心数据结构

`struct udev` / `struct udev_device` / `struct udev_enumerate` / `struct udev_monitor` / `struct udev_list_entry`（user/lib/udev-shim/libudev.h）— 与 Linux libudev ABI 一致。

`udev_device` 关键字段：
- syspath : char[] — `/sys/class/<subsys>/<dev>`（真实路径）
- devnode : char[] — `/dev/input/eventX`
- sysname : char[] — `eventX`（syspath 末段）
- subsystem / devtype : char[]
- devnum : dev_t — = stat.st_rdev（ino）
- refcount

`udev_enumerate`（libudev.h : udev_enumerate）
- refcount / udev
- subsystem_filter : char[32] — "input" 等，空串不过滤
- sysname_filter : char[64] — "event*" 等，空串不过滤
- devices : udev_list_entry* — scan 结果链表头

### 关键流程

**通知：netlink uevent 广播**（kernel/bsd/netlink.c : nl_uevent_broadcast）
- devtmpfs_create（ops->subsystem 非 NULL）/ remove / cleanup_pid 三处调用
- `sys_dev_set_meta`(96) 第二步设备就绪时调用
- payload Linux 格式：`"action@devpath\0ACTION=action\0DEVPATH=devpath\0SUBSYSTEM=subsystem\0"`，nlmsghdr 包裹，`nlmsg_type`=ADD/REMOVE/CHANGE
- group bit 0（NETLINK_KOBJECT_UEVENT），`nl_groups[NL_MAX_GROUPS=32]` 链表 fan-out，recv_queue 上限 256 drop-oldest

**/dev 枚举：** `devtmpfs_open` 对目录返回 `FD_DIR`，`devtmpfs_getdents` 枚举子项；shim `scan_devices()` 用 `opendir("/dev/input")` 调此路径。

**shim 设备发现与属性查询：**
1. `scan_devices` — `opendir("/dev/input")` + stat + `EVIOCGBIT` 过滤键盘，建静态设备表
2. `create_udev_device` — syspath 构造为 `/sys/class/<subsys>/<dev>`（drm 走 `/sys/class/drm/...`，其余 `/sys/class/%s/%s`）；`devnum = stat.st_rdev`
3. `udev_device_new_from_devnum` — `find_device_by_devnum(stat.st_rdev)` + 扫描 fallback
4. `udev_device_get_sysattr_value(attr)` — `open(syspath + "/id/" + attr)`(input) 或 `syspath + "/" + attr`(drm) + read 到静态缓冲区（单次有效，去尾换行）

**shim enumerate：**
1. `add_match_subsystem("input")` / `add_match_sysname("event*")` 写 filter
2. `scan_devices` — `opendir("/sys/class/<subsys>")`，`match_pattern`（尾部 `*` glob，无 `*` 退化为 strcmp）过滤 sysname
3. 每命中项构造 syspath 入 `udev_list_entry` 链表
4. `get_list_entry` 返回链表头，`unref` 释放链表

**启动流程：** init.elf → `open("/dev/serial")` dup2 stdio → spawn evdev 驱动 → `wait_dev_ready("/dev/input/event0")` → spawn udevd → spawn terminal。evdev 两步注册（见 sysfs.md）产出 `/dev/input/eventX` + sysfs 属性树。

### 系统调用/接口

无新增 syscall（86/87/88 已被 epoll 占用，废弃的原 pipe(86)/getdents(87)/ring(88) 设计不再实现）。

复用：`SYS_GETDENTS`(50) /dev 枚举、`SYS_DEV_SET_META`(96) evdev 两步注册第二步（sysfs 建树 + uevent）、`SYS_EPOLL_CREATE/CREATE1/CTL`(86-88) epoll。

### 与其他模块的关系

- `doc/design/sysfs.md`：shim 读 `/sys/class/.../<attr>` 真文件；evdev 两步注册建树
- `doc/design/kernel/netlink.md`：uevent 广播机制（group 注册表、fan-out、drop-oldest）
- `doc/design/evdev.md`：evdev 用户态驱动产出 sysfs 属性 + SHM ring 事件流
- udevd（user/udev/udevd.c，121 行）：netlink 订阅器，阶段 A 仅打印 uevent

## 待完成项

| 项目 | 说明 | 优先级 |
|------|------|--------|
| monitor 实现 | shim `udev_monitor_*` 当前 no-op stubs（`get_fd` 返回 -1、`receive_device` 返回 NULL）。推荐 M1 netlink 直读：shim `socket(AF_NETLINK)` + bind(group=1) + recvmsg 解析 uevent；阶段 B 后期可切 udevd pipe SCM_RIGHTS | 高 |
| udevd 完整 daemon | 121 行骨架仅订阅 netlink 打印。缺：设备属性数据库、AF_UNIX `/run/udev/socket` 服务端、ENUMERATE/MONITOR/QUERY 协议、pipe monitor + SCM_RIGHTS、PCI 扫描补属性。属阶段 B，非 path-seat 阻塞项 | 中 |
| udevd crash 恢复 | init 仅 `waitpid(-1)` 收尸循环，无 udevd 监控 + 重启逻辑 | 中 |
| udevd 集成测试 | 缺 `test_udevd.c`（AF_UNIX 连接、enumerate、monitor 热插拔、crash 恢复） | 中 |
| 内核通知 syscall 测试 | 缺 `test_dev_notify.c` | 中 |
| dev_entries 动态链表 | `dev_entries[MAX_DEV_ENTRIES=32]` 仍为静态数组，`next` 在数组槽位上串链表，未走 kmalloc；上限提升到 128。当前 32 足够覆盖 ~8 设备 | 低 |
| 事件持久化/恢复 | netlink 无持久化，udevd crash 后队列中事件 drop-oldest。Linux udevd 也无内核级持久化，靠 coldplug 扫 `/sys` 重建。接受丢失或加 coldplug | 低 |
| ID_INPUT_MOUSE/TOUCHPAD/SEAT | shim 当前只合成 ID_INPUT/KEYBOARD/KEY（EVIOCGBIT EV_KEY）；缺 MOUSE(REL_X/Y)、TOUCHPAD(BTN_TOOL_FINGER)、SEAT("seat0") | 低 |
