# 内核调试

记录本 OS 调试相关的**方法论**和**操作手册**。方法论是可复用的诊断套路；操作手册是具体工具的使用方式。

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

---

# 操作手册

## 9. 串口输出与日志

串口输出通过 QEMU `-serial file:log.txt` 写入 `log.txt`。**串口输入已移除**（RX ring/ISR/read 全删），键盘输入通过 `sendkey` 注入（见 §10）。

```bash
# 查看日志
tail -f log.txt
```

优先考虑串口打印定位，QEMU 初始化约 5s + 引导时间，建议等待 10s 以上。

## 10. sendkey 键盘注入（替代串口输入）

串口输入已移除。所有键盘交互通过 QEMU monitor 的 `sendkey` 命令完成。`run.sh` 使用 `-monitor stdio`，在 QEMU 所在的 shell/stdio 直接输入 `sendkey` 命令即可。

**键名映射**（常用键，完整列表见 QEMU 文档）：

| 键 | sendkey 名称 | 说明 |
|----|--------------|------|
| a-z | `a`-`z` | 字母键 |
| 0-9 | `0`-`9` | 数字键 |
| Enter | `ret` | 回车 |
| Backspace | `backspace` | 退格 |
| Tab | `tab` | 制表 |
| Escape | `esc` | 退出 |
| 方向键 | `up`/`down`/`left`/`right` | 方向 |
| Ctrl 组合 | `ctrl-c`/`ctrl-z`/`ctrl-d` | Ctrl+字母 |
| Shift 组合 | `shift-a`/`l-shift`/`r-shift` | Shift+键 |
| Alt 组合 | `alt-a`/`l-alt`/`r-alt` | Alt+键 |
| F1-F12 | `f1`-`f12` | 功能键 |
| Space | `spc` | 空格 |

**使用技巧**：
- `sendkey` 每次只发一个键事件（按下+释放），多字符输入需逐个 `sendkey`
- tmux 场景：`tmux send-keys -t qemu 'sendkey l' Enter`
- 脚本化批量输入：`for k in l s ret; do tmux send-keys -t qemu "sendkey $k" Enter; sleep 0.1; done`
- GDB 中断（Ctrl-C）仍通过 tmux `C-c` 发给 gdb session，不要用 `sendkey ctrl-c`

## 11. 常见错误信号

- **Page Fault (#PF)**：检查地址映射和空指针
- **General Protection (#GP)**：检查段选择子、IOPL、MSR 访问
- **Triple Fault (#DF)** → QEMU 重启：通常是 TSS IST 栈或 IDT 未正确设置

## 12. Debug 模式（栈回溯）

`./build.sh -d` 启用 `-g -fno-omit-frame-pointer`，异常时打印完整寄存器 + RBP 链栈回溯（最多 16 帧）。

```bash
./build.sh -d
./run.sh            # 串口输出自动写 log.txt
cat log.txt            # 找 BACKTRACE 段
addr2line -e build/myos.elf -f -C 0xFFFFFFFF8010XXXX
```

## 13. GDB 远程调试

```bash
./run.sh -s            # 启用 GDB 服务器
gdb -ex "target remote localhost:1234" build/myos.elf
```

用户态地址解析：
```bash
addr2line -e build/init.elf -f -C 0x400245
```

## 14. tmux + QEMU + GDB 自动化调试

```bash
rm -f log.txt
tmux new-session -d -s qemu './run.sh -s 2>&1'
tmux new-session -d -s gdb 'gdb -ex "target remote localhost:1234" build/myos.elf'
tmux send-keys -t gdb 'continue' Enter
sleep 20
tmux send-keys -t gdb '' C-c          # Ctrl-C 中断
tmux send-keys -t gdb 'bt' Enter
tmux capture-pane -t gdb -p
addr2line -e build/init.elf -f -C 0x400245  # 用户态地址解析
# 注入键盘输入（通过 QEMU monitor）
tmux send-keys -t qemu 'sendkey l' Enter    # 单个字母
tmux send-keys -t qemu 'sendkey ret' Enter  # Enter 回车
tmux send-keys -t qemu 'sendkey s' Enter && tmux send-keys -t qemu 'sendkey h' Enter && tmux send-keys -t qemu 'sendkey l' Enter && tmux send-keys -t qemu 'sendkey ret' Enter  # "shl" + Enter
# 脚本化批量输入
for k in l s ret; do tmux send-keys -t qemu "sendkey $k" Enter; sleep 0.1; done
# 查看串口日志
cat log.txt
tmux kill-session -t gdb; tmux kill-session -t qemu
```

注：tmux send-keys 只能发按键到对应 session 的 stdio。QEMU monitor 在 qemu session 的 stdio，键盘输入通过 `sendkey` monitor 命令注入到 qemu session。串口输入已移除，不再需要 socat serial session。
