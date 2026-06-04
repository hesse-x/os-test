# 微内核缺失功能实现计划

## 阶段一：用户态基础设施

### 1. GDT 扩展：TSS + 用户态段
- 新增 TSS（Task State Segment），用于 ring 0 → ring 3 栈切换
- GDT 从 3 项扩展到 6 项：null, code, data, user code, user data, TSS
- 新增选择子：`USER_CS = 0x1B`（ring 3 code）、`USER_DS = 0x23`（ring 3 data）、`TSS_SEL = 0x28`
- 加载 TR 寄存器（`ltr`）

### 2. IDT 扩展：用户态可触发的中断门
- `int 0x80` 系统调用入口：DPL 设为 3，允许 ring 3 调用
- 新增 `vector128` stub（vectors.S 中）

### 3. 用户态地址空间
- 每个进程独立 PD，内核空间（高地址）共享映射
- 用户空间从 0x0 起始，暂定 0-4MB 用户区
- 用户进程 ELF 加载 / 简单内存拷贝到用户地址空间

**验证点：** 手动构造一个 ring 3 栈帧，通过 iret 跳入用户态，能执行简单指令并触发 #GP（证明特权级切换成功）

---

## 阶段二：进程与调度

### 4. 进程控制块（PCB）
- 结构体：PID、状态（READY/RUNNING/BLOCKED）、页表、内核栈、用户栈、上下文寄存器、入口点
- 进程表：固定大小数组（如 MAX_PROC=64）
- 当前进程指针 `current_proc`

### 5. 上下文切换
- `switch_to(prev, next)`：保存/恢复 callee-saved 寄存器（ebx, esi, edi, ebp, esp），切换栈
- 时钟中断（PIT 已就绪）驱动调度：`trap` 中检测 vector 32 时调用 `schedule()`
- 简单轮转调度器（Round Robin）

### 6. 进程创建
- `process_create(entry, args)`：分配 PCB、分配内核栈、构建 trapframe（模拟中断返回到用户态）、设置入口地址
- 第一个用户进程：内核初始化末尾创建 init 进程，切换过去

**验证点：** 两个用户进程轮转，各自在屏幕不同位置打印字符

---

## 阶段三：系统调用

### 7. 系统调用框架
- `int 0x80` 入口：用户态通过 `int $0x80` + eax=syscall号 进入内核
- 系统调用分发表：`syscall_table[eax](ebx, ecx, edx, esi, edi)`
- trapframe 中 eax 存返回值

### 8. 基础系统调用
- `sys_putc(char c)` — 输出字符（替代直接调用内核 fb_putc）
- `sys_getpid()` — 获取当前进程 PID
- `sys_yield()` — 主动让出 CPU

**验证点：** 用户态进程通过 `int 0x80` 调用 sys_putc 打印字符

---

## 阶段四：IPC

### 9. IPC 消息传递
- 消息结构：固定大小（如 256 字节），包含发送方 PID + 类型 + 数据
- 同步 IPC：`send(dest, &msg)` 阻塞直到对方 receive，`recv(src, &msg)` 阻塞直到有消息
- 实现：发送方挂入接收方等待队列，接收方无等待者则阻塞

### 10. IPC 系统调用
- `sys_send(pid_t dest, void *msg)` — 发送消息
- `sys_recv(pid_t src, void *msg)` — 接收消息（src=0 表示任意来源）
- `sys_sendrecv(pid_t dest, void *msg)` — 原子发送+接收（RPC 语义）

**验证点：** 两个进程通过 IPC 互相发送消息，屏幕交替打印

---

## 阶段五：用户态服务

### 11. 驱动服务化
- 键盘驱动从内核移至用户态服务进程：通过 IPC 接收中断通知，转换 scancode 后发送给请求进程
- 内核仅保留中断通知机制：IRQ 时向注册的服务进程发送通知消息

### 12. 简单 shell
- 用户态 shell 进程：等待键盘输入，按回车执行命令
- 演示微内核架构：shell → IPC → 驱动服务 → IPC → 回 shell

---

## 依赖关系

```
阶段一(1→2→3) → 阶段二(4→5→6) → 阶段三(7→8) → 阶段四(9→10) → 阶段五(11→12)
```

每阶段末尾有验证点，确保前一层稳固再进入下一层。
