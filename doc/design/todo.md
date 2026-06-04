# 微内核缺失功能实现计划

## 阶段一：用户态基础设施 ✅ 已完成

> 详细方案见 [ring3_switch.md](ring3_switch.md)

### 1. GDT 扩展：TSS + 用户态段 ✅
- 新增 TSS（Task State Segment），用于 ring 0 → ring 3 栈切换
- TSS 为静态全局变量（放 .bss，和 gdt_entries 同理）
- GDT 从 3 项扩展到 6 项：null, code, data, user code, user data, TSS
- 新增选择子：`USER_CS = 0x1B`（ring 3 code）、`USER_DS = 0x23`（ring 3 data）、`TSS_SEL = 0x28`
- 加载 TR 寄存器（`ltr %w0`），TSS 描述符 type=0x89（Available），ltr 后 CPU 自动标记 Busy
- TSS.esp0 初始设为 boot stack top（虚拟地址）；阶段二进程切换时改为 per-process 更新（见下方补充）

### 2. IDT 扩展：用户态可触发的中断门 ✅
- `int 0x80` 系统调用入口：flags=0xEE（interrupt gate, DPL=3）
- 新增 `vector128` stub（vectors.S 中），走独立 syscall_entry 入口（不走 __alltraps）
- IDT_ENTRIES 从 48 改为 49
- `set_idt_gate` 新增 flags 参数（默认 0x8E），vector128 传入 0xEE

### 3. trapframe_t 扩展 ✅
- 增加 `esp` 和 `ss` 字段（struct 尾部，eflags 之后），与 ucore 一致
- **不手动 push SS/ESP**：esp/ss 仅在特权级变化时由 CPU 自动 push，`__alltraps` 和 `__trapret` 保持原逻辑不变
- ring 0 中断时 esp/ss 不在栈上，不应访问
- 新增 `syscall_entry/syscall_ret`（独立于 __alltraps/__trapret），段寄存器用 `pushl`（32 位），iret 自动处理特权级变化
- 新增 `syscall_dispatch` stub（kernel/trap.cc）

### 4. 新增基础库 ✅
- `arch/x86/lib.h` + `lib.cc`：`memcpy` 等（freestanding 无标准库）
- 拷贝 PDE 等场景需要，后续按需补充 memset/memmove

### 5. 引导清理 ✅
- `kernel_init_finish()`：清除 PD[0] identity map（not present + flush_tlb）+ 禁止 bump 分配器
- bump_alloc 加 `cli;hlt` 检查，防止初始化后误用

### 6. ring 3 验证测试 ✅
- 不分配用户页/用户栈，EIP 指向 not-present 地址（0x1000），ESP=0xBFFFFFFC
- iret 进入 ring 3 → 立刻触发 #PF
- trap_dispatch 检测 #PF from ring 3（检查 tf->cs & 0x3 == 3）→ 串口输出 → halt
- 测试代码内联在 kernel.cc，注释标注临时代码
- **修正方案中 test_ring3 内联汇编栈顺序**：iret 从低到高 pop EIP→SS，负偏移编号需反转

**验证结果：** QEMU 串口输出 `Ring 3 switch verified! #PF at EIP=0x00001000`，特权级切换成功

---

## 阶段二补充（基于阶段一决策）

- TSS.esp0：进程切换时更新为 `next_proc->kernel_stack_top`（替代阶段一的 boot stack top）
- 用户页分配 + 用户程序加载：分配物理页映射到用户地址空间，拷贝用户代码
- 用户栈分配：为每个用户进程分配真实用户栈页（阶段一用随机 ESP，不分配）
- 删除 test_ring3 临时代码
- 用户态 PD：PD[0] not present，内核区域拷贝 PDE（supervisor-only），用户区域 User 位

---

## 阶段三补充（基于阶段一决策）

- syscall 入口优化：pushal → 只保存 callee-saved（ebx, esi, edi, ebp）+ eax 返回值占位
- syscall_dispatch 实现：系统调用分发表 + 参数传递
- IRQ/ISR 入口分离：ISR（异常）走 __alltraps，IRQ（硬件中断）走更精简路径

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
