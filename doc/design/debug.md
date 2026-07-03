# 内核调试方法论

记录调试本 OS 卡死/调度类问题时验证有效的诊断手法。这些手法是**方法论**而非具体 bug 修复——具体 bug 见 `bug.md`，本文只留可复用的套路。

## 1. 第一原则：诊断输出必须极度克制

**Why：** QEMU chardev `logfile=log.txt` 的写入路径是串行的。一次大缓冲 dump（如 256 行 sched_log）就能把写入路径堵住，dump 之后 log.txt 停止刷新，现象表现为"卡死无输出"，把人引向"卡死"或"日志刷屏"的误判，绕很久。

**How to apply：**
- 周期性诊断（timer / idle 循环里）只打**每核一行计数**或**单字符进度**，绝不打全表。
- 要 dump 大状态就只 dump 一次（gated by `static int dumped` 或 per-CPU 数组），且只 dump 目标进程单条状态。
- 偶发事件（wake / steal / 迁移）可打一行，但 gate 住只打首次命中。

## 2. 单字符进度链：定位"卡在哪一步"

适用于循环里多阶段串行执行、卡点未知的场景（如 `idle_entry` 的 `rcu→reap→steal→schedule→sti→hlt`）。

**做法：** 给每个阶段分配一个字符，进阶段前打印该字符（不打换行）。log 里最后一个字符即卡点。

```
idle_entry 每轮：
  rcu_read_lock/unlock  → 'I'  (已进 idle)
  reap_hook             → 'r'
  try_steal_task        → 'Q'  (试图偷)
  schedule              → '>'/'<'（prev/next 切换）或 'E'（空队列 return）
  sti; hlt              → 'H'
```

健康日志：`IrQ>IrQ>...IrQH IrQ>...`（循环往复，最后落到 `H` hlt）。
卡死日志：`IrQEEEEEEE...`（停在 `E` = schedule 反复走空队列分支）→ 直接锁定"schedule 取不到任务"。

**约束：** 只在特定 prev（如 `prev==idle`）时打印，避免普通进程切换时刷屏。**用完即删**——这是临时定位工具，不常驻代码（噪音 + 可能堵 logfile）。

## 3. per-CPU 计数器：区分"本核哑火" vs "全局停摆"

全局 `tick++` / 全局 sched 计数**不能**证明本核在跑。SMP 下某核 timer 停摆、sched 停摆都要靠 per-CPU 计数分别观测。

```c
// cpu_local_t 加字段（仅 debug，#ifndef NDEBUG 门控）
uint32_t timer_count;   // 本核 timer 中断次数
uint32_t sched_count;   // 本核 schedule() 调用次数
uint8_t  idle_phase;    // idle 循环当前阶段（1..6）

// timer_handler 里 timer_count++
// schedule() 入口 sched_count++、出口前 idle_phase = 当前阶段
```

每 N tick 打一行轻量 DIAG：
```
tick=2000: cpu0:timer=149,sched=58,phase=4   cpu1:timer=152,sched=1,phase=6
```

判读：
- `timer` 涨但 `sched` 停 → 中断源正常，调度路径卡住（抢占点/锁/空队列）。
- `timer` 都停 → 中断控制器/某核 halt 异常。
- `phase` 长期不变 → 卡在该阶段（phase=4 即卡在 schedule()）。

## 4. 看门狗（watchdog）：把"被绕过"变成"打一行告警"

适用于"标志置了但不被消费"类 bug（如 `need_resched` 置位但 `schedule()` 不执行）。这类 bug 的特征签名是**标志长期不清零**。watchdog 把这个落差变成自动告警。

**典型：preempt-stall watchdog**（已实装，`kernel/xcore/trap.c` timer_handler，`#ifndef NDEBUG` 门控）：

```c
// cpu_local_t: uint32_t preempt_stall_ticks;
// schedule() 入口清 need_resched → 下个 tick watchdog 计数归零
if (current_task && current_task->need_resched) {
    if (++cl->preempt_stall_ticks >= 100) {  // ~1s 未兑现
        printk(LOG_WARN, "PREEMPT-STALLED cpu%d pid%d ... run_queue ready=%d\n", ...);
    }
} else {
    cl->preempt_stall_ticks = 0;
}
```

健康态：`need_resched` 每次都被 `schedule()` 兑现 → 计数恒 0 → 永不打印。
卡死态：1s 内自动打印，消息直接点明"抢占点被绕过 + ready 队列里有几个任务在等"。

**通用模式：** 任何"置标志位 → 某出口消费"的设计，都可以加一个 per-CPU tick 计数看门狗，标志持续 N tick 未清即告警。release 构建 `#ifndef NDEBUG` 排除，零开销。

## 5. 持锁遍历验证"在队列里"

`!list_empty(&run_node)` 不可信——`run_node` 悬挂、或在别的 CPU 队列上时也非空。

**做法：** 持目标 CPU 的 `scheduler_lock`，从该 CPU 的 `run_queue` 头遍历找目标 pid，命中记下标。这才是"真在队列里"的证据。

**字段语义对齐：** 打印"是否找到"用布尔 `found=1/0` 或 `found=yes@idx0`，**不要**用 `-1` 哨兵再打成数字——`found=0` 会被误读成"没找到"（其实 0 可能是"在 idx0 找到"），把证据方向读反。bug.md Bug 2 就栽在这上面。

## 6. 偏移量陷阱：用 `offsetof`，别硬编码

读 `cpu_local_t` / `xtask_t` 的字段时，**永远用 `offsetof(Type, field)`**，不要凭记忆硬编码偏移（如以为 `run_queue` 在 `+0x38`，实际 `+0x38` 是 `run_count`，读"next/prev"读成计数的高字节 → 误判内存损坏）。

`nm build/myos.elf | grep cpu_locals` 可查数组大小反推 `sizeof(cpu_local_t)`；写个 freestanding offsetof 小程序验证最稳。

## 7. 诊断代码的生命周期

- **常驻**：watchdog 类（异常才打一行，零干扰）、per-CPU 计数器（可 `#ifndef NDEBUG` 常驻）。
- **用完即删**：单字符进度链、持锁遍历校验、临时 WAKE-AUDIT / SCHED-MISSING。这些是定位工具，定位完即清除，不留噪音。
- **门控**：所有诊断一律 `#ifndef NDEBUG` 或 `LOG_DEBUG` 级别，release 构建零开销、零输出。

## 8. 相关

- 调度器具体结构与卡点阶段定义见 `doc/design/kernel/schedule.md`（idle_entry / schedule / try_steal_task）。
- 锁竞争诊断见 `doc/design/kernel/kernel_lock.md`。
- 已定位并修复的 bug（含各诊断手法的实战出处）见 `bug.md`。
- 串口连接 / GDB / tmux 自动化等通用 debug 操作见 `CLAUDE.md` §5 debug。
