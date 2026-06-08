# 微内核实现进度

## 已完成

### 阶段一：用户态基础设施 ✅
> 原始设计见 [ring3_switch.md](ring3_switch.md)（x86-32 版本）

- GDT 扩展：TSS + 用户态段（x64: 7项 GDT，TSS 跨两个 slot）
- IDT 扩展：vector128 (int 0x80) syscall 入口，flags=0xEE
- trapframe_t 扩展：64位版本（RAX-R15 + trapno + err_code + RIP + CS + RFLAGS + RSP + SS）
- syscall_entry / syscall_ret：独立入口，pushall 风格
- kernel_init_finish：禁用 bump 分配器

### 阶段二：进程与调度 ✅
> 原始设计见 [process_scheduler.md](process_scheduler.md)（x86-32 版本）

- PCB (proc_t)：pid, state, k_rsp, k_stack_top, cr3, entry, wait_event
- switch_to：保存/恢复 callee-saved (rbx, rbp, r12-r15) + 切换 RSP + 切换 CR3
- process_entry：jmp __trapret → iretq 到用户态
- schedule()：轮询扫描 READY 进程
- idle 进程：PID=0，boot stack，hlt 循环

### 阶段三：系统调用 + 用户分页 + 用户栈 ✅
> 原始设计见 [sys_call.md](sys_call.md)（x86-32 版本）

- 4个系统调用：putc(0), getpid(1), yield(2), getc(3)
- 独立 PML4 + CR3 切换（每进程 PML4，共享 PML4[511] 内核映射）
- 用户栈：0x00007FFFFFFFD000（canonical 低半区）
- BLOCKED 状态 + WAIT_KBD 键盘阻塞/唤醒

### 阶段四：Shell + ATA PIO + ELF Loader ✅
> 原始设计见 [shell.md](shell.md)（x86-32/ELF32 版本）

- ATA PIO LBA28 驱动（从盘 0xF0，QEMU 第二个 IDE 设备）
- ELF64 Loader（Elf64_Ehdr/Phdr，4级页表映射）
- process_create_elf：从 ELF64 加载创建进程
- Shell 程序：user/shell.cc，getc/putc 循环
- Shell 构建流程：g++ -m64 → ld -m elf_x86_64 -Ttext 0x400000 → disk.img LBA 1
- Framebuffer 滚动

### 阶段零：x86-64 迁移 ✅
> 设计见 [x64_migration.md](x64_migration.md)

- 32位 x86 → 64位 x86-64 全面迁移
- Multiboot2/GRUB → UEFI stub bootloader (boot/stub.c)
- -fPIE/GOTOFF → -mcmodel=kernel/RIP-relative
- VMA_BASE 0xC0000000 → 0xFFFFFFFF80000000
- 2级页表 → 4级页表 (PML4→PDPT→PD，2MB huge pages)
- ELF32 → ELF64
- 所有 arch/x86/ → arch/x64/

### UEFI 引导 ✅
> 设计见 [uefi.md](uefi.md)

- EFI stub bootloader (BOOTX64.EFI)
- FAT32 启动映像 (mkimg.sh)
- GOP framebuffer + RSDP 传递
- boot_info 结构体传递内核启动参数

### 多核 SMP — Per-CPU 基础设施 ✅

- `cpu_local_t` 结构体（cpu_id, apic_id, _cur_proc, lapic_base, kernel_stack, tss_rsp0, run_count）
- GS base 机制：`set_cpu_local()` 写 MSR_KERNEL_GS_BASE，`swapgs` 切换，`get_cpu_local()` 读 MSR_GS_BASE
- `current_proc` 宏改为 per-CPU 访问（通过 GS base）
- Per-CPU GDT + TSS：`per_cpu_gdt[MAX_CPUS][7]`、`per_cpu_tss[MAX_CPUS]`
- `smp_init_cpu()`：初始化 per-CPU 数据，`smp_apply_cpu()`：lgdt + swapgs + ltr 加载

### 多核 SMP — APIC 替换 PIC ✅

- MADT 解析（boot/stub.c）：提取 LAPIC 基地址 + I/O APIC 基地址 + 各 CPU APIC ID
- `pic_disable()`：mask 全部 8259A IRQ
- LAPIC 使能：MMIO 映射 + MSR 启用 + SVR software enable
- I/O APIC 配置：24 个 GSI 重定向项，timer(GSI 0) + keyboard(GSI 1) unmask 路由到 BSP
- EOI：`outb(0x20, 0x20)` → `lapic_write(LAPIC_EOI, 0)`
- LAPIC Timer：PIT 校准 + 周期模式启动，替代 PIT 定时中断

### 多核 SMP — AP 启动 ✅

- AP trampoline（`arch/x64/ap_trampoline.S`）：16位实模式入口，实模式→保护模式→长模式→`ap_entry_c`
- INIT IPI + SIPI 启动协议（`smp_boot_aps()`）：INIT → 10ms → SIPI x2 → 200us
- `ap_entry_c()`：加载 GDT/IDT，设置 GS base，初始化本 CPU TSS + LAPIC，启用中断
- `init_ap_idle()`：每个 AP 创建独立 idle 进程
- `pick_cpu()`：基于 run_count 的简单负载均衡

### SYSCALL/SYSRET 快速系统调用 ✅

- MSR 设置：STAR(0x08/0x0B)、LSTAR(syscall_fast_entry)、SFMASK(IF+TF)、EFER.SCE
- `syscall_fast_entry`：swapgs → 读 tss_rsp0 切栈 → 构建 trapframe → call syscall_dispatch
- SYSRET 返回：从 trapframe 恢复寄存器 → RCX=rip, R11=rflags → swapgs → sysretq
- 调用约定：RAX=syscall#，args=RDI/RSI/RDX/R10/R8/R9（Linux 风格）
- `map_user_page_direct` 增加 flags 参数，用户栈页映射时加 PTE_NX
- int 0x80 路径已移除，仅支持 SYSCALL/SYSRET

### TSS IST 栈 ✅

- 每 CPU 3 个独立 IST 栈（4KB）：IST1=NMI(#2), IST2=Double Fault(#8), IST3=Machine Check(#18)
- `smp_init_cpu()` 中通过 bfc_alloc 分配并填入 `tss.ist[0/1/2]`
- `set_idt_gate()` 增加 ist 参数，IDT 门描述符 IST index 字段生效

### NX 位 ✅

- `enable_nx()`：设置 CR4.NXDE(bit5) + EFER.NXE(bit11)
- PTE 标志常量化：PTE_PRESENT/PTE_RW/PTE_USER/PTE_PS/PTE_NX
- 用户栈页映射时加 PTE_NX（不可执行）
- AP trampoline 中 EFER 写入增加 NXE 位

## 当前状态

内核已完整运行：UEFI 引导 → 内核初始化 → AP 启动 → 加载 shell.elf → 启动 shell 用户进程。BSP 运行调度循环，AP 进入 idle 循环。

## 未完成

### 多核 SMP — 调度与锁

- [ ] 自旋锁（`spinlock_t`：原子 test-and-set + `pause` + `cli`）
- [ ] 大内核锁（BKL）：`__alltraps`/`syscall_entry` 入口加锁，`__trapret`/`syscall_ret` 出口释放
- [ ] 全局调度器加 `scheduler_lock` 保护
- [ ] `procs[]` 进程表加 `procs_lock` 保护
- [ ] 键盘唤醒路径加锁保护
- [ ] AP 进入调度循环，参与 round-robin 调度

### 多核 SMP — 细粒度锁优化

- [ ] 拆分 BKL：调度器独立锁
- [ ] 拆分 BKL：进程表独立锁
- [ ] 拆分 BKL：键盘/驱动独立锁
- [ ] 验证并发正确性

### 其他扩展

| 功能 | 依赖 | 状态 |
|------|------|------|
| 运行时 IPI | LAPIC | [ ] reschedule / TLB shootdown |
| IPC 消息传递 | 无 | [ ] 用户态服务化基础 |
| 文件系统 | ATA | [ ] 替代硬编码 LBA |
| 更多 shell 命令 | 无 | [ ] echo, help, clear, pid |
| MSI / MSI-X | APIC | [ ] PCIe 设备中断 |
