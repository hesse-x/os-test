# Perf — 统计采样性能分析

## 概述

内核始终支持 perf 采样（无编译开关）。500 µs LAPIC 定时器中断 → 每 CPU 写入 `{rip, pid, cpu}` 到静态环形缓冲区 → `perf.elf` 包装进程：调 `sys_perf_ctl(START)` 启动采样 → spawn 目标程序 → waitpid → 调 `sys_perf_ctl(STOP_DUMP)` → 串口输出原始样本 → 离线 `perf-report.py` 解析。

**使用方式**：
```bash
./build.sh && ./run.sh
# 等 QEMU 启动（~5s），测试自动跑完，查看 log.txt
python3 scripts/perf-report.py log.txt
```

**初始化序列**：

```
init
  ├─ kbd_driver
  └─ perf.elf
       ├─ sys_perf_ctl(START)         → 开始 2000Hz 采样
       ├─ spawn("/test/test_runner.elf")
       ├─ waitpid(...)                ← 等待所有测试跑完
       └─ sys_perf_ctl(STOP_DUMP)     → 停止采样 + 串口 dump
```

## 设计决策

| # | 决策 | 选择 | 理由 |
|---|------|------|------|
| 1 | 编译开关 | **无**。perf 始终编译进内核 | 运行时开销仅采样活跃时的 `if (perf_active)` 判断，可忽略 |
| 2 | 构建标志 | 移除 `--perf` 和 `--test` 标志 | test ELFs 始终构建进 image；perf 功能始终可用 |
| 3 | 定时器频率 | **永久 2000 Hz**（500 µs 间隔） | schedule() 频率 20 倍不影响 0.05% 开销；信号响应和 xHCI 轮询更灵敏 |
| 4 | 缓冲区 | Per-CPU 数组 `65536` 条目 | 2000Hz × 30s = 60k < 65k；静态分配避免 kmalloc |
| 5 | 采样数据 | `{rip, pid, cpu}` = 16 字节 | 中断路径最小写入；pid 区分进程 |
| 6 | 启动控制 | `perf_active` 布尔锁，默认 false | `sys_perf_ctl(START)` 开启；`sys_perf_ctl(STOP)` 关闭 |
| 7 | 采样触发 | `perf.elf` 调 `sys_perf_ctl(START)` | 仅采样 perf.elf 的子孙进程（perf 自身的启动代码不受采样影响，等它调 START 后才开启） |
| 8 | 输出通道 | 串口（`printk` PERF: 前缀行） | 零依赖，`log.txt` 自动收集 |
| 9 | 符号解析 | 离线 `addr2line`（`myos.elf` + user ELFs） | `.symtab` 保留中；无需内核内符号查询 |

## 架构

```
                 ┌─────────────────────────────┐
                 │ LAPIC 定时器 (2000 Hz)       │
                 └──────────┬──────────────────┘
                            │ 中断
                            ▼
                 ┌─────────────────────────────┐
                 │ timer_handler(tf)            │
                 │   tick++                     │
                 │   lapic_eoi()                │
                 │   if (perf_active)           │
                 │     perf_record(tf)          │
                 │   ...timer_queue...          │
                 │   schedule()  用户态抢占      │
                 └─────────────────────────────┘

                    ┌───────┴───────┐
                    ▼               ▼
            Per-CPU buf[0]    Per-CPU buf[1]
            65536 entries     65536 entries
            16 bytes each     16 bytes each

                    │                │
                    └───────┬───────┘
                            │ sys_perf_ctl(STOP_DUMP)
                            ▼
                 ┌─────────────────────────────┐
                 │ perf_dump_all()              │
                 │   for each cpu:              │
                 │     printk("PERF: ...")      │
                 └─────────────────────────────┘
                            │
                            ▼ log.txt
                 ┌─────────────────────────────┐
                 │ PERF: cpu=0 pid=2 rip=0x... │
                 │ PERF: cpu=1 pid=3 rip=0x... │
                 │ ...                         │
                 │ PERF: samples_total=8723    │
                 │ PERF: dump_end              │
                 └─────────────────────────────┘
                            │
                            ▼
                 ┌─────────────────────────────┐
                 │ perf-report.py              │
                 │   addr2line → symbol count   │
                 │   → flat profile             │
                 └─────────────────────────────┘
```

## 数据结构

```c
// kernel/xcore/perf.h
#define PERF_SAMPLE_MAX 65536

typedef struct {
    uint64_t rip;       // 指令地址
    uint32_t pid;       // current_task->pid, idle 为 -1
    uint32_t cpu;       // CPU ID
} __attribute__((packed)) perf_sample_t;

typedef struct {
    volatile uint32_t head;              // 下一个写入位置
    uint32_t _pad;                       // 对齐
    perf_sample_t samples[PERF_SAMPLE_MAX];
} __attribute__((aligned(64))) perf_buf_t;
```

同一 CPU 上的定时器中断不会嵌套，`head` 只需 volatile 防止编译器优化读-改-写。

## 关键代码路径

### 中断路径（timer_handler 内）

```c
if (perf_active) {
    int cpu = get_cpu_local()->cpu_id;
    perf_buf_t *buf = &perf_buf[cpu];
    uint32_t idx = buf->head;
    if (idx < PERF_SAMPLE_MAX) {
        buf->samples[idx].rip = tf->rip;
        buf->samples[idx].pid = current_task ? current_task->pid : (uint32_t)-1;
        buf->samples[idx].cpu = (uint32_t)cpu;
        buf->head = idx + 1;
    }
}
```

`perf_active` 默认为 false。调用 `sys_perf_ctl(START)` 后置 true 开始记录。

### 定时器频率

`apic_init()`（BSP）和 `ap_entry_c`（AP）中均改为：

```c
lapic_write(LAPIC_TIMER_ICR, lapic_timer_ticks_calibrated / 20);  // 2000 Hz
```

`sys_perf_ctl(STOP_DUMP)` **不恢复** 100 Hz（频率永久保持 2000 Hz，perf 非活跃时不额外开销）。

### Syscall

```c
#define SYS_PERF_CTL  69

#define PERF_CTL_START     0   // 清空 buffer + 开启采样
#define PERF_CTL_STOP      1   // 停止采样，不 dump
#define PERF_CTL_STOP_DUMP 2   // 停止采样 + 串口 dump
```

`NR_XCORE_SYSCALL` 扩展为 70，在 xcore 层直接分发。

### Dump 输出

```
PERF: cpu=0 pid=2 rip=0xFFFFFFFF80101234
PERF: cpu=0 pid=3 rip=0x400567
PERF: cpu=1 pid=3 rip=0x400ABC
...
PERF: perf_samples_total=8723
PERF: perf_dump_end
```

## 文件变更清单

| 文件 | 改动 |
|------|------|
| `doc/design/perf.md` | 本文档 |
| `common/syscall_nums.h` | +`SYS_PERF_CTL 69`，+`NR_SYSCALL 70` |
| `common/syscall.h` | +`sys_perf_ctl()` inline 封装 |
| `arch/x64/apic.c` | 改 BSP timer 为 `ticks / 20`（2000 Hz） |
| `arch/x64/smp.c` | 改 AP timer 为 `ticks / 20` |
| `kernel/xcore/perf.h` | 新建：数据结构 + 声明 |
| `kernel/xcore/perf.c` | 新建：`perf_init`, `perf_timer_handler`, `sys_perf_ctl`, `perf_dump_all` |
| `kernel/xcore/trap.c` | `timer_handler()` 中插 `perf_timer_handler()` 调用 |
| `kernel/xcore/trap.h` | +`NR_XCORE_SYSCALL 70`，+`sys_perf_ctl` 声明 |
| `kernel/xcore/CMakeLists.txt` | +`perf.c` |
| `kernel/xcore/init.c` | +`perf_init()` |
| `user/perf/perf.cc` | 新建：perf.elf（START → spawn → waitpid → STOP_DUMP） |
| `user/CMakeLists.txt` | +perf.elf，移除 `if(TEST)` 保护 |
| `init/init.c` | 改 spawn perf.elf 替代 terminal+shell |
| `init/CMakeLists.txt` | 移除 `if(TEST)` 保护 |
| `build.sh` | 移除 `--perf`、`--test` 标志 |
| `build_script/mkdisk.sh` | 始终复制 test ELFs + perf.elf |
| `scripts/perf-report.py` | 新建：离线解析脚本 |

## 偏移分析脚本（perf-report.py）

从 `log.txt` 提取 `PERF:` 行，按 `cpu:pid:rip` 聚合，使用 `addr2line` 将 RIP 解析为函数名：

- RIP ≥ `0xFFFFFFFF80000000` → 内核地址 → `addr2line -e build/myos.elf -f`
- RIP < `0xFFFFFFFF80000000` → 用户地址 → 区分 ELF 通过地址范围和 pid 推断

输出 flat profile：

```
===== Flat Profile =====
Total samples: 8723 (kernel: 3421, user: 5302)

用户态热点（5302 samples）:
  samples  share   function
    1200  22.6%   memcpy
     800  15.1%   strcmp
     500   9.4%   printf
     ...

内核态热点（3421 samples）:
  samples  share   function
     800  23.4%   schedule
     500  14.6%   trap_dispatch
     300   8.8%   fat32_read
     ...
```

## 限制

- 无调用链（flat profile 仅显示函数热度，需人工关联上下文）
- 无 PMU 事件（仅 PC 采样，无 cache miss / branch miss 计数）
- 缓冲区满时静默丢弃尾部样本
- idle 进程不计入采样（`current_task` 为空或 idle 时 `pid=-1`）
- 无硬件调用栈展开（为 V2 预留）
