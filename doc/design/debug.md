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

## 15. PERF 采集与分析

PERF 构建用于采集启动阶段和测试用例的 wall time。它与 Debug 构建用途不同：Debug 构建侧重异常现场和栈回溯，PERF 构建使用 Release 优化，同时保留调试符号、frame pointer 和 ELF build ID。

### 15.1 一键运行

```bash
tools/perf-run.sh
```

该命令依次执行：

1. `./build.sh --perf`，生成 512 MiB PERF 镜像和 `build/perf-symbols/` 符号副本。
2. 启动 QEMU/KVM，串口仍写入 `log.txt`。
3. 等待 collector 正常导出；如果串口连续 60 秒无变化，则停止 QEMU 并恢复最近一次持久化 checkpoint。
4. 从磁盘的 MBR 分区表定位 FAT32 根分区，提取 raw/metadata 并生成报告。

runner 正常结束后 collector 会先完成 raw/metadata 的 `fsync + rename + sync`，
然后调用 `XOS_PERF_REQUEST_EXIT`；PERF QEMU 配置中的 `isa-debug-exit` 只在这个
时刻退出，host 不会在 guest 仍写盘时提取镜像。默认 watchdog 从 boot TSC 起
10 分钟：timer 只发布超时请求，collector 下一次轮询在进程上下文冻结、导出
`complete=false` 结果并走相同受控退出路径，IRQ/NMI 中不会进入 VFS。

默认结果目录是 `build/perf-results/`。也可以指定另一个目录：

```bash
tools/perf-run.sh build/perf-results/run-001
```

不要使用 `repeat.sh` 代替 `perf-run.sh` 做 PERF 采集。`repeat.sh` 看到 Test Runner 的 Summary 后会立即停止 QEMU，可能早于 collector 的最终 `fsync` 和 rename。

### 15.2 checkpoint 与硬卡死恢复

collector 在启动 runner 后立即落一份 checkpoint，之后每 60 秒执行一次：

```text
冻结当前 committed 边界（采集继续）
  -> 覆盖写 perf.raw.tmp / meta.tmp
  -> fsync
  -> sync
```

正常结束时，runner 退出钩子将数据冻结为最终快照，collector 再原子发布 `perf.raw` 和 `metadata.json`。如果 guest 在测试中完全停止调度，进程定时器也无法运行，但磁盘上仍保留上一次 checkpoint。此时报告会显示 `complete: false`，这是可分析的中途快照，不是解析失败。

串口中可用以下命令确认持久化进度：

```bash
rg 'perf: (checkpointed|exported)' log.txt
```

典型输出：

```text
perf: checkpointed 5456 bytes, 221 records, complete=0 status=-1
perf: exported 5888 bytes, 239 records, complete=1 status=0
```

如果 QEMU 已经由其他方式运行并停止，可以单独提取：

```bash
tools/perf-extract.sh build/disk.img build/perf-results/manual
```

必须在 QEMU 停止后提取，避免读取正在修改的 FAT 镜像。提取器不使用固定 LBA，而是验证 MBR、分区范围和 FAT32 分区类型。当前 guest FAT 实现可能让 `fsck.fat -n` 报告目录元数据问题；提取器会给出 warning，并继续以只读方式提取，raw 自身仍必须通过边界和 CRC 校验。

### 15.3 输出文件

| 文件 | 用途 |
|------|------|
| `perf.raw` | 原始二进制记录；包含 ABI header、24-byte records、footer 和 CRC32 |
| `metadata.json` | ABI、完成状态、结束原因、TSC 频率、记录数和 committed bytes |
| `perf-report.txt` | 适合直接阅读的 kernel Top 热点、启动阶段及逐测试耗时 |
| `perf-summary.json` | 含 build ID、采样质量、热点和阶段耗时的结构化结果 |
| `perf.folded` | 带权重的完整 kernel/user 调用链，可交给 FlameGraph 的 `flamegraph.pl` |
| `perf-speedscope.json` | 可直接拖入 speedscope.app 查看按样本权重排序的热点 |
| `perf-trace.json` | Chrome/Perfetto Trace，可查看 phase/test 时间轴 |
| `qemu-host.log` | QEMU host 侧输出；串口输出仍在仓库根目录 `log.txt` |

符号文件按 ELF build ID 保存在：

```text
build/perf-symbols/
build/perf-symbols/build-id-manifest.tsv
```

raw 和 metadata 是本次运行的数据，`perf-symbols/` 是与本次构建匹配的符号副本。不要拿另一次构建的 ELF 做地址解析。

### 15.4 阅读报告

```bash
sed -n '1,160p' build/perf-results/perf-report.txt
python3 -m json.tool build/perf-results/perf-summary.json | less
```

报告开头的 `Kernel hotspots` 是优化 kernel 时的主要入口。每行给出函数占
全部 kernel 样本的比例、样本数和源码位置；报告工具会核对 raw 内嵌的完整
SHA-1 build ID、`build/perf-symbols/build-id-manifest.tsv` 和 `myos.elf` 三者，
一致后才批量调用 `addr2line`，避免误用同名但不同构建的符号文件。需要交互
查看时：

```bash
# 浏览器中打开 https://www.speedscope.app/ 后拖入该文件
ls build/perf-results/perf-speedscope.json

# Chrome chrome://tracing 或 Perfetto UI 中加载
ls build/perf-results/perf-trace.json

# 已安装 FlameGraph 时生成传统 SVG
flamegraph.pl build/perf-results/perf.folded > build/perf-results/perf.svg
```

采样器优先使用 architectural PMU 的 unhalted cycles overflow，以每 CPU 约
1000 Hz 的独立 PMI/NMI 采样；NMI 使用 IST1，从 trapframe 的 RIP/RBP 开始做
最多 32 帧的有界 frame-pointer unwind。用户栈通过页表和 direct-map 读取，
kernel 栈只允许当前 task、IRQ 或 NMI 栈范围，过程中不分配、不打印、不取锁，
也不会触发用户缺页。PMU 不可用时自动退回每 CPU 约 100 Hz 的 LAPIC timer。

报告中的 `sampling.source` 会明确显示 `pmu_nmi`、`lapic_timer` 或 `mixed`。
只有纯 PMU 结果才标记 `confidence: high` 和
`interrupts_disabled_visible: true`；fallback 无法观察长时间关中断区间，必须按
degraded 数据解释。在 Linux/KVM host 上还应确认：

```bash
cat /sys/module/kvm/parameters/enable_pmu
```

输出 `N` 表示 host 管理员禁用了 KVM vPMU，guest 会按设计使用 timer fallback，
并不是采集器初始化失败。`lost_samples` 和 `trace_lost` 应为 0；非零表示固定
容量的 callchain 或调度/因果 critical-event 表已满。高频业务 IRQ 不写逐次
BEGIN/END 记录，而在 kernel 内按 CPU/vector 精确累计 count、total cycles 和
max cycles，因此 IRQ storm 不会挤掉调度时间线。

重点字段：

- `complete`：`true` 表示目标正常结束并完成最终导出；`false` 表示 checkpoint/incomplete session。
- `end_reason`：`0` 是运行中 checkpoint，`1` 是目标正常退出，`2` 是非零退出，`3` 是信号退出，`4` 是手工冻结，`5` 是 watchdog。
- `duration_ms`：从 boot TSC 到本次快照边界的 wall time，不是 CPU time。
- `record_count`：本次快照包含的 committed records 数量。
- `sampling.sample_hits`：进入热点统计的 kernel RIP 样本总数。
- `sampling.lost_samples`：聚合表冲突后无法容纳的样本数，健康结果应为 0。
- `sampling.samples_per_cpu`：各 CPU 的 PMU/timer 样本数，用于发现未启用的 AP。
- `sampling.nmi_count`、`handler_cycles`：PMI 次数和采集 handler 自身开销。
- `sampling.truncated_callchains`：因坏 frame、未映射页、非执行地址或深度上限而截断的链数。
- `top_kernel_hotspots`：按符号聚合并从高到低排序的 kernel 样本。
- `scheduling.cpus`：每 CPU busy/idle、利用率和上下文切换次数。
- `scheduling.top_tasks`：按重建 running interval 汇总的高 CPU task。
- `scheduling.wake_latency`：`TASK_WAKE -> 该 task 下一次 switch-in` 的调度唤醒延迟分布及逐项证据。
- `irqs`：按 CPU/vector/owner 汇总的业务 IRQ 次数、总 handler 时间和最长 handler 时间；timer/PMI 不进入该表。
- `causal_chains`：按带子系统命名空间的 cookie 连接同步 IPC 及 AHCI/xHCI/virtio-GPU submit/IRQ-vector/complete/wake；重复 cookie 会按每次 submit 拆成独立 instance，`complete=false` 表示 checkpoint 截断。
- `trace_errors`：未知 phase、栈不匹配、重复 END 或损坏记录；健康结果应为空。
- `phases[].inclusive_ms`：阶段自身和所有子阶段的总 wall time。
- `phases[].exclusive_ms`：扣除已知子阶段后的 wall time，用于判断耗时是在本阶段本体还是子阶段。
- `tests[].status`：`pass`、`fail`、`skip`、`crash` 或 `running`。checkpoint 最后一个测试经常是 `running`，表示快照发生时它尚未 END。
- `tests[].duration_ms`：对应 BEGIN/END marker 之间的 wall time；`running` 项没有完整时长。

快速汇总测试状态：

```bash
python3 - <<'PY'
import collections
import json

with open("build/perf-results/perf-summary.json", encoding="utf-8") as stream:
    summary = json.load(stream)
counts = collections.Counter(test["status"] for test in summary["tests"])
print("complete:", summary["complete"])
print("duration_ms:", round(summary["duration_ms"], 3))
print("records:", summary["record_count"])
print("tests:", dict(counts))
PY
```

对比两次运行时优先比较相同 phase/test 的耗时，不要只比较总时长。先确认两份结果的 `complete`、测试集合、失败项和 build ID 一致，再判断性能回归；checkpoint 与完整结果不能直接当作同口径样本。

### 15.5 raw 校验与故障判读

也可以直接重新解析已经提取的 raw：

```bash
tools/perf-report.py build/perf-results/perf.raw \
  --metadata build/perf-results/metadata.json \
  --output-dir build/perf-results/reparsed
```

解析器把 raw 当作不可信输入，检查 magic、ABI version、endian、header/footer 大小、记录边界和对齐、sequence、timestamp、header CRC、record CRC，以及 metadata 的 boot TSC/TSC frequency/record count。出现 CRC 或边界错误时不要继续分析耗时，应先确认 QEMU 是否仍在运行、镜像是否完整同步、raw 是否来自匹配的 session。

当前版本已经覆盖：

- `_start -> kernel_main` 及 paging/GDT/higher-half early phases；
- 每个测试的 BEGIN/END 和 pass/fail/skip/crash 状态；
- PMU/NMI（或显式 timer fallback）RIP 采样及最多 32 帧的有界调用链；
- 每 CPU switch/block/wake 时间线、busy/idle、top task 和 wake latency；
- 不受 IRQ storm 影响的业务 IRQ 聚合，以及 IPC/AHCI/xHCI/virtio-GPU 因果链；
- folded、Speedscope 和 Chrome/Perfetto 输出；
- 完整退出冻结、启动时 checkpoint、每分钟 checkpoint、10 分钟 watchdog 和受控 QEMU 退出。

它仍不会把 timer fallback 结果描述成关中断区间可见；只有报告明确显示
`pmu_nmi` 且 `interrupts_disabled_visible=true` 时，才能用采样判断长 IRQ-off
路径。完整 malformed/golden corpus、故障注入矩阵和穷举式端到端验证不属于
本轮 kernel 优化工具的范围。
