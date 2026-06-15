# Shell 设计

## 概述

Shell 是交互式用户进程，从 stdin pipe（fd 0）读键盘输入，向 stdout pipe（fd 1）写输出，通过 `sys_msg` 变长 IPC 与 fs_driver 通信执行文件操作。不匹配内置命令时当作文件路径执行（`spawn` + `waitpid`）。

## 启动

Shell 启动后轮询 `device_lookup(DEV_FS)` 等待 fs_driver 注册就绪（1ms 超时 `sys_recv`），找到后进入命令循环。

```c
while ((fs_pid = device_lookup(DEV_FS)) < 0) {
    struct recv_msg m;
    recv(&m, NULL, 0, 1);
}
```

## 行编辑

`readline(buf, len)` 从 fd 0 逐字符读取，支持退格（ASCII 8）删除前一字符并回显 `BS + SPACE + BS`。回车结束输入。

## 命令

| 命令 | 说明 |
|------|------|
| `ls [-l] [path]` | 列目录。`-l` 显示权限/硬链接/属主/大小/日期。默认列 cwd |
| `cat <path>` | 读文件打印内容 |
| `cd [path]` | 切换目录。无参数切到 `/` |
| `pwd` | 打印工作目录 |
| `touch <path>` | 创建空文件 / 更新已存在文件时间戳 |
| `mkdir <path>` | 创建目录 |
| `r LBA [COUNT]` | 裸扇区 hex dump（调试用） |
| `h` | 帮助 |
| `<path>` | 路径执行（替代旧 `run` 命令） |

## 路径执行

未匹配内置命令时当作文件路径执行：

1. `build_abs_path(token)` 得到绝对路径
2. `fs_request(FILE_CMD_OPEN, path)` 打开文件
3. 循环 `FILE_CMD_READ` 读取整个文件到 malloc 缓冲区
4. ELF magic 校验（`\x7fELF`）
5. `spawn(elf_buf, file_size, 0)` + `waitpid` 同步等待
6. `free(elf_buf)`

错误信息：open 失败 → `file not found`；ELF 校验失败 → `not an executable file`。

语义：不带 `./` 前缀也当路径执行（如 `hello.elf` 等同于 `./hello.elf`），不实现 PATH 搜索。

## FS IPC 协议

Shell 通过 `sys_msg` 向 fs_driver 发送 `file_req`（固定大小）请求，接收 `file_resp`（变长，header + data[]）响应。

### file_req

```c
struct file_req {
    uint32_t cmd;            // 命令编号
    char     path[256];      // 文件路径
    uint32_t flags;          // open 标志
    uint32_t fs_fd;          // fs_driver 端 fd
    uint64_t offset;         // 读偏移
    uint32_t count;          // 读长度
    uint32_t lba;            // raw_read LBA
    uint32_t readdir_offset; // readdir 分页偏移
    uint32_t readdir_count;  // readdir 分页数量
};
```

### file_resp

```c
struct file_resp {
    int32_t  status;       // 状态码（0 或 -errno）
    uint32_t fd;           // open 返回的 fd
    uint64_t file_size;    // 文件大小
    uint32_t count;        // 实际读取字节数
    uint32_t total;        // readdir 实际条目数
    uint8_t  data[];       // 变长数据
};
```

### 命令

| cmd | 名称 | 请求字段 | 响应字段 |
|-----|------|---------|---------|
| 1 | open | path, flags | status, fd, file_size |
| 2 | read | fs_fd, offset, count | status, count, data[] |
| 3 | write | fs_fd, offset, count, data[] | status（stub，返回 ENOSYS） |
| 4 | close | fs_fd | status |
| 5 | readdir | path, readdir_offset, readdir_count | status, total, count, data[] |
| 6 | create | path | status |
| 7 | mkdir | path | status |
| 8 | raw_read | lba | status, count, data[] |

协议与 libc `file.cc` 和 fs_driver 共享，定义在 `common/shm.h`。

## 工作目录

Shell 维护 `static char cwd[256]`，初始值 `/`。`build_abs_path(rel, abs)` 将相对路径解析为绝对路径：以 `/` 开头的直接使用，否则拼接 cwd + `/` + rel。`cd` 命令通过 `readdir(offset=0, count=1)` 校验目标是否为目录。

## ls -l 格式

```
drwxr-xr-x 2 root root    0 Jun 14 10:30 boot
-rw-r--r-- 1 root root 4096 Jun 14 10:30 hello.elf
```

字段映射：权限（目录 `drwxr-xr-x` / 普通 `-rw-r--r--` / 只读 `-r--r--r--`）、硬链接数（目录 2 / 文件 1）、属主属组固定 `root`、大小 `file_size`、日期时间 `wrt_date + wrt_time` → `Mon DD HH:MM` 格式。

## readdir 分页

`fs_dirent` 占 272 字节，单次最多返回 30 个条目。Shell 循环 `offset=0; offset+=total` 直到 `total < 30`。

## 与 Terminal 的关系

Shell 和 Terminal 是独立进程，通过内核 pipe 通信（kernel_main 创建）：

- pipe_stdin：terminal fd 1(W) → shell fd 0(R)
- pipe_stdout：shell fd 1(W) → terminal fd 0(R|O_NONBLOCK)

数据流：键盘 → kbd_driver → kbd_ring → terminal → sys_write(1) → stdin pipe → shell sys_read(0)；shell printf → sys_write(1) → stdout pipe → terminal → VT100 → cell buffer → KMS ring → kms_driver → framebuffer。

Terminal 设计详见 [terminal_split.md](terminal_split.md)。
