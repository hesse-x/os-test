# libinput 集成层

## 当前架构设计

libinput 集成层运行在 terminal 进程地址空间，提供键盘事件处理。数据路径：

```
USB xHCI → HID SHM → evdev 驱动 → SHM ring → ringbuf_fops read() → libevdev 垫片
  → libinput → terminal (libinput_dispatch / libinput_get_event)
```

terminal 为动态 ELF（PT_INTERP → ld.so），运行时加载 `libc.so` + `libinput.so`。

### 设计决策

| # | 决策 | 选择 | 理由 |
|---|------|------|------|
| 1 | libinput 后端模式 | path-seat（`libinput_path_create_context`） | 无需 udev 设备枚举和热插拔监听，垫片量最小；terminal 启动时手动添加 `/dev/input/event0` |
| 2 | libinput 接入方式 | 编译 libinput 上游源码为 `libinput.a`/`libinput.so`，不修改 `third_party/libinput` 一行代码 | 所有适配通过垫片头文件 + include path 转发实现，vendor 代码保持可同步 |
| 3 | 垫片实现位置 | `user/lib/evdev-shim/` + `user/lib/udev-shim/`，在 libinput 编译的 include path 中优先匹配 | `#include <libudev.h>` 和 `#include <libevdev/libevdev.h>` 直接解析到垫片，无需 -D 宏或符号链接 |
| 4 | 内核事件读取路径 | `ringbuf_fops`（`kernel/bsd/sysfs.c : ringbuf_read`），consumer 通过 `read(fd)` 从 evdev SHM ring 拉取 | 复用现有 SHM 机制，不引入额外 ring buffer 拷贝；`ringbuf_fops.read` 阻塞等待使用 kernel wq（当前返回 -EAGAIN，TODO 项） |
| 5 | 键盘 modifier 管理 | terminal 进程内 `key_to_ascii` 处理（Shift/Ctrl/Alt/CapsLook 表驱动映射） | libinput 只提供 raw key code（`libinput_event_keyboard_get_key`），不处理字符映射；保持与 Linux evdev 一致的层级分离 |
| 6 | quirk 策略 | 编译时 `-DLIBINPUT_QUIRKS_DIR="/usr/share/libinput"`，磁盘映像安装 `10-generic-keyboard.quirks` | libinput 初始化时需要 quirks 目录存在，否则 `libinput_path_create_context` 返回 NULL |
| 7 | 动态链接 | terminal 为动态 ELF，`DT_NEEDED libc.so` + `DT_NEEDED libinput.so` | libinput.so 自身编译时 `-fvisibility=hidden` + `LIBINPUT_EXPORT` 标记公开 API；ld.so 运行时解析依赖 |
| 8 | libevdev 垫片粒度 | 仅实现 keyboard 子集（`new_from_fd`/`next_event`/`has_event_code`/`get_name`/`set_clock_id`/`get_fd`/`get_abs_info`），其余 stubs | libinput 内部在多处引用 libevdev 函数（含 keyboard 不走的路径），未实现的函数在 `user/lib/libinput-compat/compat.c` + `stubs.c` 中空桩 |

### 核心数据结构

**libinput 上下文（terminal 内，未封装）**：
  `li : struct libinput*` — path-seat 上下文，通过 `libinput_path_create_context` 创建
  `li_fd : int` — libinput 内部 epoll fd，通过 `libinput_get_fd(li)` 获取，用于 poll
  `device : struct libinput_device*` — `/dev/input/event0` 设备，通过 `libinput_path_add_device(li, "/dev/input/event0")` 添加

**libevdev 垫片内部状态（user/lib/evdev-shim/evdev.c）**：
  `evdev` — 内部 `struct libevdev` 结构体
    fd : int — `/dev/input/event0` 的打开 fd
    name : char[64] — 设备名（`EVIOCGNAME` 获取）
    id : struct input_id — bustype/vendor/product/version（`EVIOCGID` 获取）
    caps : uint8_t[EV_CNT][96] — 能力位图（`EVIOCGBIT` 获取）
    clock_id : int — 时钟类型（`CLOCK_MONOTONIC`）

### 垫片层

#### libudev 垫片（user/lib/udev-shim/）

libinput path-seat 模式只用到 `udev_new` + `udev_unref`（后两者在 path-seat 路径实际不走，但 libinput 初始化流程引用了 `udev_new` 符号）。

接口声明在 `user/lib/udev-shim/libudev.h`，实现在 `user/lib/udev-shim/udev.c`。

当前实现（commit 66f5cc0 起补全）：
- `udev_new()` — 返回非空上下文（path-seat 不依赖其内容，但需非 NULL）
- `udev_unref()` — no-op
- `udev_device_new_from_syspath` / `udev_device_new_from_devnum` / `udev_device_new_from_subsystem_sysname` — 按 syspath/devnum/subsystem+sysname 构造设备对象
- `udev_device_get_sysattr_value` — 真读 sysfs：`open("<syspath>/id/<attr>")` + `read()`（input 子系统用）
- `udev_device_get_syspath` / `get_sysname` / `get_devnode` / `get_subsystem` — 路径/名称访问器
- `udev_enumerate_new` / `add_match_subsystem` / `add_match_sysname` / `scan_devices` / `get_list_entry` — `scan_devices` 扫描 `/dev/input` 设备（opendir + stat + 键盘判别）
- `udev_monitor_new_from_netlink` — 占位（接收 fd 由 netlink uevent 通道驱动，详见 [kernel/netlink.md](kernel/netlink.md)）

回归测试：`user/test/test_libudev.c`（TEST-gated ELF）。

#### libevdev 垫片（user/lib/evdev-shim/）

垫片接口声明在 `user/lib/evdev-shim/libevdev/libevdev.h`，实现在 `user/lib/evdev-shim/evdev.c`。

关键实现：
- `libevdev_new_from_fd(fd, &evdev)` — open `/dev/input/event0`，通过 `ioctl EVIOCGNAME`/`EVIOCGID`/`EVIOCGBIT` 填充内部状态，返回 0
- `libevdev_get_name(evdev)` — 返回内部 name
- `libevdev_has_event_code(evdev, type, code)` — 查内部 caps_bitmap
- `libevdev_next_event(evdev, flags, &ev)` — `read(fd, &ev, sizeof(ev))` 从 ringbuf_fops 读取 24B `struct input_event`；返回 0 成功 / -EAGAIN 无数据
- `libevdev_set_clock_id(evdev, clock_id)` — 存储 clock_id
- `libevdev_get_fd(evdev)` — 返回内部 fd
- `libevdev_get_abs_info(evdev, code)` — 返回 NULL（键盘无 ABS 轴）
- `libevdev_free(evdev)` — close fd + free

### 内核事件读取路径

kernel/bsd/sysfs.c : ringbuf_fops（`struct file_operations`）
  .read = ringbuf_read — 从 inode->shm 的 SHM ring buffer 读取 `struct input_event`（24B），按 file offset（ring cursor）遍历。cursor 落后 head 超过 capacity 时跳到 head。cursor==head 时阻塞等待（当前返回 -EAGAIN，TODO 阻塞实现）
  .poll = ringbuf_poll — cursor != head 时返回 POLLIN
  .close = ringbuf_close — 向 evdev 驱动发送 RINGBUF_CLOSE 通知（当前 no-op）
  .mmap = ringbuf_mmap — 仅 driver_pid 可 mmap（当前返回 -ENOSYS）

devtmpfs 中 FD_DEV 注册时自动识别 SHM-backed 设备并挂 ringbuf_fops（kernel/bsd/devtmpfs.c : devtmpfs_open，SHM-backed 分支 `f->f_op = &ringbuf_fops`）。

### 终端集成

terminal 进程（user/driver/terminal_libinput.cc）事件处理流程：

1. `libinput_path_create_context(&interface, NULL)` — 创建 path-seat 上下文，`interface` 提供 `open_restricted`/`close_restricted` 回调
2. `libinput_log_set_handler(li, libinput_log)` — 注册日志回调（输出到 stderr）
3. `libinput_path_add_device(li, "/dev/input/event0")` — 添加键盘设备，libinput 内部通过回调 `open_restricted` 打开 `/dev/input/event0`
4. `li_fd = libinput_get_fd(li)` — 获取 libinput 内部 epoll fd 用于 poll
5. 主循环：
   - `libinput_dispatch(li)` — libinput 内部从 fd 读取 evdev 事件并处理
   - `libinput_get_event(li)` — 循环取所有待处理事件
   - 类型为 `LIBINPUT_EVENT_KEYBOARD_KEY` 时：提取 `key` + `state`，调 `key_to_ascii` 转换
   - ldisc 处理（ISIG/ICANON/ECHO 同原有逻辑）
   - `libinput_event_destroy(ev)` — 释放事件对象
6. 键盘事件处理完后 `read(master_fd)` 读 shell 输出，VT100 渲染
7. `poll([li_fd, master_fd], 2, -1)` 阻塞等待下一轮输入

备注：terminal ldisc 的行编辑（Backspace/行缓冲/Ctrl-C/Ctrl-D/Ctrl-Z）与原有 terminal.cc 逻辑一致，modifier 管理（Shift/Ctrl/Alt/CapsLook）在 `key_to_ascii` 内表驱动实现，libinput 不参与字符映射。

### 编译集成

**libinput 源文件编译**（user/CMakeLists.txt : add_user_lib(input) + add_user_lib(libinput_so)）：

编译 `third_party/libinput/src/` 下的 28 个 C 源文件，包含：
- libinput.c / evdev.c / evdev-fallback.c — 核心事件分发
- path-seat.c — path 后端
- timer.c / util-*.c — 工具函数
- quirks.c — quirk 加载
- filter-*.c — 滤波器（键盘只走 NULL stubs）

编译标志：
- `-DLIBINPUT_QUIRKS_DIR="/usr/share/libinput"` — quirks 目录路径
- `-DHAVE_QUIRKS` — 启用 quirk 支持
- `-Wno-*` — 关闭上游代码在 freestanding 环境的非致命警告

include 路径顺序：
1. `include/uapi/` + `include/uapi/compat/` — `<linux/input.h>` compat 转发
2. `third_party/libinput/include` — libinput 自身头文件
3. `third_party/libinput/src` — 内部头文件（`libinput-private.h` 等）
4. `user/lib/evdev-shim/` — `<libevdev/libevdev.h>` 垫片
5. `user/lib/udev-shim/` — `<libudev.h>` 垫片
6. `user/lib/libinput-config/` — `config.h`（`HAVE_*` 定义）
7. `user/lib/libinput-compat/` — 滤波器、libevdev、插件 stubs

垫片源文件（`user/lib/evdev-shim/evdev.c` + `user/lib/udev-shim/udev.c`）和 compat 源文件（`user/lib/libinput-compat/compat.c` + `stubs.c`）直接编译进 `libinput.a`/`libinput.so`，与 libinput 上游源码同库。

**terminal 链接**（user/CMakeLists.txt）：
- 静态构建：`LINK_LIBS c` + `libinput.a` 包含在 disk.img
- 动态构建：`add_user_dyn_elf(terminal ... LINK_LIBS c input)` → `DT_NEEDED libc.so` + `DT_NEEDED libinput.so`

### quirks 文件

libinput 初始化时读取 `/usr/share/libinput/` 下的 `.quirks` 文件。磁盘映像中安装 `third_party/libinput/quirks/` 目录内的关键文件：

- `10-generic-keyboard.quirks` — 通用键盘 quirk（必须，否则 `libinput_path_create_context` 返回 NULL）
- `10-generic-mouse.quirks` — 通用鼠标 quirk（非必须，但 libinput 会尝试加载）

quirk 路径通过编译时 `-DLIBINPUT_QUIRKS_DIR` 硬编码，运行时不可配置。

### 事件类型

terminal 仅使用以下 libinput 事件类型：

| 事件类型 | 对应枚举值 | terminal 处理 |
|----------|-----------|--------------|
| 键盘按键 | `LIBINPUT_EVENT_KEYBOARD_KEY` | `key_to_ascii` + ldisc 处理 |
| 设备添加 | `LIBINPUT_EVENT_DEVICE_ADDED` | 忽略（terminal 不感知设备生命周期） |
| 设备移除 | `LIBINPUT_EVENT_DEVICE_REMOVED` | 忽略 |

### 数据结构引用

- libinput 事件结构体：`third_party/libinput/src/libinput-private.h`
- libevdev 垫片实现：`user/lib/evdev-shim/evdev.c`
- libudev 垫片实现：`user/lib/udev-shim/udev.c`
- terminal libinput 集成：`user/driver/terminal_libinput.cc`
- 内核 ringbuf_fops 读取路径：`kernel/bsd/sysfs.c : ringbuf_read`
- compat stubs：`user/lib/libinput-compat/stubs.c` / `compat.c`

### 数据流

```
HID 驱动 → evdev SHM ring (struct input_event[256], 24B per event)
  → ringbuf_fops.read → libevdev_next_event (read fd)
    → libinput_dispatch (evdev.c evdev_process_event)
      → LIBINPUT_EVENT_KEYBOARD_KEY
        → terminal: key_to_ascii + ldisc + write(master_fd)
```

### 与其他模块的关系

- **evdev 驱动**：evdev 进程维护 SHM ring buffer，libinput 作为 consumer 通过 `read(/dev/input/event0)` 拉取事件。详见 [evdev.md](evdev.md)
- **terminal**：libinput 集成在 terminal 进程内，作为键盘输入源。详见 [terminal.md](terminal.md)
- **libc**：libinput.so 运行时依赖 libc.so（clock_gettime、malloc、字符串等）。详见 [libc.md](libc.md)
- **ld.so**：dynamic ELF 加载 libinput.so + libc.so。详见 [ld.md](ld.md)
- **devtmpfs**：SHM-backed FD_DEV 自动挂 ringbuf_fops。详见 [vfs.md](vfs.md)

## 待完成项

| 项目 | 说明 | 优先级 |
|------|------|--------|
| ringbuf_read 阻塞等待 | 当前 cursor==head 返回 -EAGAIN，terminal poll 轮询。需通过 evdev notify + wait_queue 实现真正的阻塞读，消除 poll 忙等 | 中 |
| udev-seat 模式 | path-seat 之外添加 `libinput_udev_create_context` 支持：需 udev enumerate（已实现 `scan_devices`）+ netlink monitor（`udev_monitor_new_from_netlink` 占位）。当前 path-seat 已覆盖 terminal 键盘场景 | 低 |
| evdev multi-device | 多输入设备（event0..eventN）场景下 libinput 多 device 共存和路由 | 低 |
| tablet / touchpad dispatch stubs | stubs.c 中空桩的滤波器（9 个）和 dispatch（tablet/touchpad/totem 共 10 个）在接入非键盘设备时需真实实现 | 低 |
| ringbuf_close 通知 evdev | `ringbuf_close` 中 RINGBUF_CLOSE 消息发送当前为 no-op，consumer 崩溃后 evdev 无法感知 | 低 |
| st_rdev + lookup_devt | libinput 内部使用 `stat.st_rdev` 反查设备类型，当前未映射 | 低 |
| libinput 自测集成 | 宿主机上跑 libinput test suite（keyboard/path/device 子集）验证在 os-test ABI 下的正确性 | 低 |
