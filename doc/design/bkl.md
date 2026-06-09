# BKL 大内核锁设计（步骤 2）

> **历史文档**：BKL 已在步骤 4（细粒度锁拆分）中完全移除。当前锁架构为 per-CPU `scheduler_lock` + 全局 `procs_lock` + `fb_lock` + `irq_owner[]` atomic，详见 [fine_grained_lock.md](fine_grained_lock.md)。

## 概述

在步骤 1 自旋锁原语基础上，实现 Big Kernel Lock (BKL)，使多核下中断处理和系统调用串行化。BSP 持锁执行内核代码，AP timer 只做 EOI 不参与调度，保证共享数据（`procs[]` 等）不被并发访问。

## 设计决策

| # | 决策 | 选择 | 理由 |
|---|------|------|------|
| 1 | 中断控制策略 | 入口 cli + lock，出口 unlock（依赖 IRETQ/SYSRETQ 恢复 IF） | 步骤 2 目标是串行化，不可抢占内核对 7 个短 syscall 足够；步骤 4 拆锁时再考虑可抢占 |
| 2 | 内核态异常处理 | `__alltraps` 检查 CS，仅用户态→内核时 ci + lock | 内核态异常（#PF 等）本就 halt，跳过 lock 避免死锁 |
| 3 | schedule() 与 BKL 交互 | unlock → switch_to → lock | 进程切换期间释放 BKL 让其他 CPU 进入内核，被调度回来后重新获取 |
| 4 | sti 时机 | 不加 sti，依赖 IRETQ/SYSRETQ 原子恢复 RFLAGS.IF | 简化出口路径，unlock 到返回用户态期间 IF=0 更安全 |
| 5 | 汇编中 unlock 实现 | 无参 `extern "C"` 包装函数 `kernel_lock_acquire()` / `kernel_lock_release()` | 避免 inline 无法 call、避免传参 clobber rdi、与 C 路径统一接口 |
| 6 | AP timer 处理 | `timer_handler` 中 `cpu_id != 0` 时只 EOI return | AP 不碰 `procs[]`，不需要 BKL；步骤 3 AP 参与调度时再统一处理 |
| 7 | 新进程首次运行 | `process_entry` 中 `call kernel_lock_acquire` 后 `jmp __trapret` | 新进程从未 acquire BKL，需补一次 acquire 使 `__trapret` 中 release 平衡 |
| 8 | BSP 首次 schedule | `kernel_main` idle 循环前 `kernel_lock_acquire()` + `sti()` | BSP idle 从未 acquire BKL，需补一次；sti 移到 kernel_main 确保 acquire 后才开中断 |
| 9 | isr_init 中的 sti | 删除，移到 kernel_main | 避免 sti 时 BKL 未持有的窗口 |
| 10 | AP sti | 保持不变 | AP 中断路径不碰共享数据，不需要 BKL |
| 11 | schedule() 中的 lock/unlock | 用包装函数 `kernel_lock_release()` / `kernel_lock_acquire()` | 统一接口，未来加调试（计数、断言）改一处 |
| 12 | 汇编 call clobber | call 放在保存后/恢复前，无需额外 save/restore | caller-saved 寄存器已在 trapframe 中保护 |

## 锁平衡验证

所有路径 acquire/release 成对：

| 路径 | acquire | release |
|------|---------|---------|
| 用户态中断 → trap_dispatch → __trapret | `__alltraps` (CS==0x2B) | `__trapret` (CS==0x2B) |
| 用户态中断 → trap_dispatch → schedule → __trapret | `__alltraps` | schedule unlock + `__trapret` |
| 用户态中断 → schedule(无 READY) → __trapret | `__alltraps` | `__trapret` |
| syscall → syscall_dispatch → sysretq | `syscall_fast_entry` | syscall 返回路径 |
| syscall → schedule → sysretq | `syscall_fast_entry` | schedule unlock + syscall 返回路径 |
| 新进程首次运行 → process_entry → __trapret | `process_entry` | `__trapret` |
| BSP idle 首次 schedule | `kernel_main` | schedule unlock + `__trapret` |
| AP timer 中断 | 不 acquire | 不 release（跳过 lock，只 EOI） |
| 内核态异常 | 不 acquire | 不 release（直接 halt） |

## 代码改动清单

| # | 文件 | 改动 |
|---|------|------|
| 1 | `kernel/trap.cc` | 定义 `spinlock_t kernel_lock = {0}` + `kernel_lock_acquire()` / `kernel_lock_release()` 两个 `extern "C"` 包装函数；`timer_handler` 中 AP 判断 `cpu_id != 0` 时只 EOI return；删除 `isr_init()` 末尾 `sti()` |
| 2 | `kernel/trap.h` | 声明 `kernel_lock_acquire()` / `kernel_lock_release()` |
| 3 | `kernel/proc.cc` | `schedule()` 中 `switch_to` 前 `kernel_lock_release()`，后 `kernel_lock_acquire()`；删除末尾 `sti()` |
| 4 | `arch/x64/trapentry.S` | `__alltraps`: CS==0x2B 时 `cli` + `call kernel_lock_acquire`；`__trapret`: CS==0x2B 时 `call kernel_lock_release`；`syscall_fast_entry`: trapframe 构建后 `call kernel_lock_acquire`；syscall 返回路径: 恢复寄存器前 `call kernel_lock_release`；`process_entry`: `jmp __trapret` 前 `call kernel_lock_acquire` |
| 5 | `kernel/kernel.cc` | idle 循环前 `kernel_lock_acquire()` + `sti()` |

## 验证

编译通过 + `-smp 2` 启动，AP idle 但中断处理串行化，系统稳定。
