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

## 阶段二补充 ✅ 已完成

> 见 [process_scheduler.md](process_scheduler.md)

- TSS.esp0：schedule() 中更新为 `next->k_stack_top` ✅
- 用户页分配 + 用户程序加载：硬编码 `jmp $`（2字节），映射到 0x400000 ✅
- 用户栈分配：阶段二不分配（ESP 假值），阶段三 syscall 时再分配 ✅
- 删除 test_ring3 临时代码 ✅
- 用户态 PD：**暂用全局 page_directory**（独立 PD + CR3 切换后续实现）✅

---

## 阶段三补充（基于阶段一决策）

> 原计划已整合到阶段三完整方案中，见 [sys_call.md](sys_call.md)

- syscall 入口优化：~~pushal → 只保存 callee-saved~~ → **保持 pushal，与 __alltraps 一致**
- syscall_dispatch 实现：系统调用分发表 + 参数传递（从 tf 提取参数传入）
- IRQ/ISR 入口分离：暂不实现，推迟到有性能需求时
- 独立 PD + CR3 切换：阶段三实现，边做边排查之前 crash bug
- 用户栈分配：栈映射到 0xBFFFE000，ESP=0xBFFFF000
- sys_getc 阻塞：wait_event 字段 + 扫描 procs[]

---

## 阶段二：进程与调度 ✅ 已完成

> 详细方案见 [process_scheduler.md](process_scheduler.md)

### 前置清理 ✅
- 删除 `test_ring3()` 及调用（kernel.cc）
- 删除 `#PF from ring 3` 特殊处理（trap.cc）
- 修复 `syscall_ret` 恢复顺序 bug（trapentry.S）：改为与 `__trapret` 一致的 popal→pop段寄存器→skip trapno/err_code→iret

### 4. 进程控制块（PCB） ✅
- `proc_t`：pid、state（READY/RUNNING）、k_esp、k_stack_top、cr3、entry
- `procs[MAX_PROC=64]` 固定数组 + `current_proc` 指针
- `proc_init()`：pid=-1 表示空闲；**schedule() 添加 current_proc==nullptr 检查**（timer 中断在 init_idle_proc 前可触发）

### 5. 上下文切换 ✅
- `switch_to(prev, next)`：push/pop callee-saved + 切换 ESP（trapentry.S）
- `process_entry()`：`jmp __trapret`，新进程首次恢复路径
- `schedule()`：环形扫描找 READY 进程 → 更新 tss.esp0 → 更新状态 → switch_to
- idle 进程（PID 0）：boot stack + 全局 PD，hlt 循环兜底
- **关键修复：idle 在 kernel_main 的 `while(1) hlt` 循环中等待中断**

### 6. 进程创建 ✅
- `process_create(entry)`：分配 PCB + 8KB 内核栈 + 用户代码页 + PT
- 用户代码：`jmp $`（2字节，0xEB 0xFE），映射到 0x400000（PD[1] + PT[0]，flags 0x07=User）
- 内核栈手工构建 trapframe + switch_to 恢复帧
- 用户栈不分配（ESP 假值 0xBFFFFFFC）
- **Page 地址转换修复**：`bfc_alloc.alloc_page()` 返回 Page 描述符指针，实际物理地址需用 `(p - BFCAllocator::frames) * PAGE_SIZE` 计算，不能直接用 `PHY_ADDR(Page*)`
- **tss 导出**：paging.cc 中 `tss` 从 static 改为 extern，paging.h 增加声明

### 当前简化
- **所有进程共用全局 page_directory**，不分配独立 PD、不切 CR3
- 独立 PD + CR3 切换暂未实现（拷贝内核 PDE 后切 CR3 仍 crash，后续排查）
- `BFCAllocator` 新增全局实例 `bfc_alloc`，供 process_create 调用非静态成员 alloc_page

**验证结果：** QEMU 串口输出轮转信息 `schedule: proc 0 → 1 → 2 → 0`，两个用户进程在 ring 3 轮转执行

---

## 阶段三：系统调用 + 用户分页 + 用户栈

> 详细方案见 [sys_call.md](sys_call.md)

### 7. 系统调用框架
- syscall_entry 保持 pushal（与 __alltraps 一致），syscall_dispatch 用 trapframe_t*
- syscall 分发表：`syscall_table[eax](ebx, ecx, edx, esi, edi)`，从 tf 提取参数传入
- 返回值写 tf->eax，syscall_ret 的 popal 自动恢复

### 8. 基础系统调用（4个）
- `sys_putc(char c)` — 输出字符（0）
- `sys_getpid()` — 获取当前进程 PID（1）
- `sys_yield()` — 主动让出 CPU，直接调 schedule()（2）
- `sys_getc()` — 读键盘输入，缓冲区空则 BLOCKED 阻塞等待（3）

### 9. 独立 PD + CR3 切换
- 每个进程分配独立 PD 页，拷贝内核 PDE（PD[768..1023]），用户 PDE 各自设置
- switch_to 时切 CR3（next->cr3），写 CR3 自动 flush TLB
- idle 进程仍用全局 page_directory 物理地址
- 之前 CR3 crash bug 边做边排查

### 10. 用户栈分配
- 每个进程分配 1 页用户栈，映射到 0xBFFFE000（PD[767], PT[1023]）
- ESP = 0xBFFFF000（栈区起始，向下增长）
- trapframe 中 esp 从假值改为真实栈地址

### 11. 进程状态扩展
- `proc_state_t` 增加 `BLOCKED` 状态
- `proc_t` 增加 `wait_event_t` 字段（WAIT_NONE / WAIT_KBD）
- schedule() 只扫描 READY 进程，跳过 BLOCKED
- kbd IRQ handler 扫描 procs[] 唤醒 BLOCKED+WAIT_KBD 进程

### 用户地址空间布局
```
0x400000   代码区（PD[1], 1 page+）
0x600000   堆区（预留，brk 按需扩展）
0xBFFFE000 栈区（PD[767], 1 page）
0xC0000000 VMA_BASE（内核, PD[768+]）
```

**验证点：** 用户态进程通过 int 0x80 调用 sys_putc 打印字符；shell 进程通过 sys_getc 等待键盘输入并回显

---

## 阶段四：Shell + ATA PIO + ELF Loader

> 详细方案见 [shell.md](shell.md)

### 12. ATA PIO 磁盘驱动
- LBA28 读扇区：`ata_read_lba(lba, count, buf)`，~60 行
- 端口 0x1F0-0x1F7，inw 读 16-bit data 寄存器
- QEMU `-drive file=disk.img,format=raw,if=ide`

### 13. ELF32 Loader
- 解析 ELF32 static binary，支持多 PT_LOAD 段
- 按 p_vaddr 映射用户页，BSS 清零
- 返回入口地址，供 process_create_elf 使用

### 14. Shell 进程
- shell.asm：getc → putc 回显 + 回车换行，零新 syscall
- nasm 编译 → ELF32 → 写入磁盘映像 / module tag 加载
- 短期：Multiboot2 module tag 获取 shell.elf；长期：ATA PIO 从磁盘读取

### 15. framebuffer 滚动
- fb_putc 处理 \n 到底部时 memmove 上移 + 清最后一行
- 纯内部实现，不暴露新接口

### 16. process_create_elf
- 新增 `process_create_elf(elf_data, size)` 独立接口
- 内部调用 elf_load 映射段 + 分配用户栈 + 构建陷阱帧

### 后续：IPC（阶段四原计划，shell 之后按需实现）

### 9. IPC 消息传递（原阶段四）
- 消息结构：固定大小（如 256 字节），包含发送方 PID + 类型 + 数据
- 同步 IPC：`send(dest, &msg)` 阻塞直到对方 receive，`recv(src, &msg)` 阻塞直到有消息
- 实现：发送方挂入接收方等待队列，接收方无等待者则阻塞

### 10. IPC 系统调用（原阶段四）
- `sys_send(pid_t dest, void *msg)` — 发送消息
- `sys_recv(pid_t src, void *msg)` — 接收消息（src=0 表示任意来源）
- `sys_sendrecv(pid_t dest, void *msg)` — 原子发送+接收（RPC 语义）

**验证点：** 两个进程通过 IPC 互相发送消息，屏幕交替打印

---

## 阶段五：用户态服务

### 驱动服务化（IPC 实现后）
- 键盘驱动从内核移至用户态服务进程：通过 IPC 接收中断通知，转换 scancode 后发送给请求进程
- 内核仅保留中断通知机制：IRQ 时向注册的服务进程发送通知消息

---

## 阶段零：x86-64 迁移

> 从32位 x86 全面迁移到 x86-64（arch/x86 → arch/x64）

### 已确定的设计决策

| 决策项 | 选择 | 理由 |
|--------|------|------|
| 内核位数 | 64位 | 32位 x86 已过时，学习64位更贴近实际 |
| 启动协议 | multiboot2 + GRUB + 32→64 trampoline | 最小改动，和 UEFI 迁移解耦 |
| higher-half 基址 | `0xFFFFFFFF80000000` | Linux 标准，配合 `-mcmodel=kernel` 高效寻址 |
| 初始页表 | 2MB huge pages，identity + higher-half 双映射 | 和现有双映射策略一致，trampoline 只需3个页表 |
| GDT 策略 | trampoline 最小 GDT，完整 GDT 在 kernel_main | 和现有 enable_page/gdt_init 分步思路一致 |
| TSS | 只设 RSP0，不用 IST | 和现有逻辑对应，简单 |
| trapframe | pushall 风格扩展到64位 | 和现有 __alltraps/__trapret 思路一致 |
| 系统调用 | syscall/sysret | 64位标配，int 0x80 是遗留路径 |
| 中断控制器 | 暂保留 PIC | 和 APIC 迁移/SMP 解耦 |

### 待实现清单

#### 1. 构建体系迁移
- [ ] toolchain: `-m32` → `-m64`，目标 `elf64-x86-64`
- [ ] CMakeLists.txt: CXX_FLAGS 加 `-m64 -mcmodel=kernel -mno-red-zone -mno-sse`
- [ ] linker script: `OUTPUT_FORMAT("elf64-x86-64")`，VMA 改 `0xFFFFFFFF80000000`，LMA AT() 重算
- [ ] do_link.cmake: `ld -m elf_x86_64` 适配64位

#### 2. 32→64 trampoline（arch/x64/trampoline.S）
- [ ] 入口仍在32位保护模式（multiboot2 交付状态）
- [ ] 设置临时 PML4 + PDPT + PD（2MB huge pages，identity + higher-half）
- [ ] 启用 PAE（CR4.PAE）、设置 CR3、启用 EFER.LME、启用 CR0.PG
- [ ] 最小 GDT（null + 64-bit code + 64-bit data）+ lgdt + ljmp 到64位代码
- [ ] 跳转到64位 `_start`

#### 3. 入口与启动（arch/x64/start.S）
- [ ] 64位 `_start`：接收 multiboot2 参数，设置64位栈，调用 `enable_page`/`kernel_main`

#### 4. 分页（arch/x64/paging.cc / paging.h）
- [ ] 4级页表结构：PML4 → PDPT → PD → PT
- [ ] `enable_page`：用64位页表项格式（bit63 NX, bit7 PS 等）
- [ ] `extend_mapping`：适配64位物理地址（uintptr_t → uint64_t 物理地址）
- [ ] `bump_alloc`：返回虚拟地址（0xFFFFFFFF80000000 + offset）
- [ ] `flush_tlb`：重写 CR3 写入适配64位

#### 5. GDT + TSS（arch/x64/paging.cc）
- [ ] 完整 GDT：null / code64 / data64 / user code64 / user data64 / TSS_low / TSS_high
- [ ] 64位 TSS：128字节，RSP0/RSP1/RSP2 + 7个 IST 入口 + IOPB
- [ ] TSS 描述符跨两个 GDT slot（128位）
- [ ] gdt_init 在 kernel_main 中调用

#### 6. IDT + 中断（arch/x64/trap.cc / trap.h）
- [ ] 64位 IDT 入口：16字节门描述符（IST 字段可用）
- [ ] vectors.S：48个向量桩，适配64位（push qword 等）
- [ ] trapentry.S：`__alltraps` push 所有16个通用寄存器 + 保存 DS/ES 等段寄存器
- [ ] trapframe_t：扩展为64位版本（RAX-R15 + trapno + err_code + RIP + CS + RFLAGS + RSP + SS）
- [ ] `__trapret`：恢复寄存器 + `iretq`
- [ ] PIC 重映射逻辑基本不变（端口 I/O 相同）

#### 7. 系统调用（syscall/sysret）
- [ ] MSR 设置：STAR / LSTAR / SFMASK / CSTAR
- [ ] `syscall_entry`：swapgs → 保存 RSP → 加载内核栈 → 保存寄存器 → 调用 syscall_dispatch
- [ ] `syscall_ret`：恢复寄存器 → swapgs → sysretq
- [ ] syscall 分发表适配64位调用约定（参数从 trapframe 提取）

#### 8. 内核模块适配
- [ ] kernel.cc：kernel_main 签名适配64位
- [ ] serial.cc：端口 I/O 不变（outb/inb），适配64位地址
- [ ] mem/alloc.cc：物理地址 uint32_t → uint64_t，BFC 分配器适配
- [ ] kbd.cc：基本不变
- [ ] fb.cc：framebuffer 地址从 uint32_t → uint64_t
- [ ] 进程/调度：trapframe_t 64位化，switch_to 适配64位 callee-saved 寄存器
- [ ] ELF loader：ELF32 → ELF64

#### 9. Multiboot2 header（arch/x64/boot.cc）
- [ ] 保持 multiboot2 header（GRUB 仍用 multiboot2 协议加载）
- [ ] framebuffer tag 等照旧

### 后续迁移（x64 稳定后）
- [ ] APIC（Local APIC + I/O APIC）替代 PIC（SMP 前置条件）
- [ ] TSS IST 配置（NMI / double fault 独立栈）
- [ ] UEFI 原生启动（EFI stub 或独立 bootloader）
- [ ] 多核 SMP 支持
- [ ] NX 位（页表项 bit63）按需启用

---

## 依赖关系

```
阶段零(构建→trampoline→分页→GDT/IDT→中断→syscall→模块适配) → 稳定后(APIC→IST→UEFI→SMP)
```

---

## 微内核缺失功能实现计划
