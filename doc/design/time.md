# 时间子系统设计

## 概述

为用户态 C 程序提供标准计时接口。内核暴露两个 syscall，libc 封装 C11 `timespec_get()` 和 C99 `clock()`。

## 内核侧

### 新增 syscall

| 编号 | 名称 | 签名 | 返回值 | 说明 |
|------|------|------|--------|------|
| 22 | `sys_gettime` | `uint64_t sys_gettime()` | `sched_clock()` 纳秒 | 全局单调时钟 |
| 23 | `sys_clock` | `uint64_t sys_clock()` | 当前进程 `cpu_time_ns` | per-process CPU 时间 |

NR_SYSCALL 从 22 改为 24（后续 sys_msg/sys_msg_resp 使 NR_SYSCALL 达到 26）。

### per-process CPU 时间记账

**proc_t 新增字段**（`kernel/proc.h`）：
```c
uint64_t cpu_time_ns;   // 累计 CPU 时间（纳秒）
uint64_t last_sched;    // 上次被调度时的 sched_clock() 值
```

**记账逻辑**（`kernel/proc.cc` 的 `schedule()`）：
- 切出 prev 前：`prev->cpu_time_ns += sched_clock() - prev->last_sched`
- 切入 next 后：`next->last_sched = sched_clock()`
- idle 进程也正常记账，不做特殊判断
- 记账在 `scheduler_lock` 保护区内执行，`sched_clock()` 只读 TSC，不增加锁持有时间

**最终记账**（`kernel/trap.cc` 的 `sys_exit`）：
- 在设 ZOMBIE 之前：`proc->cpu_time_ns += sched_clock() - proc->last_sched`
- 确保进程最后一次运行时间不丢失

**初始化**（`kernel/proc.cc` 的 `process_create_elf`）：
- `cpu_time_ns = 0`
- `last_sched = 0`（首次调度时由 schedule 设为当前 sched_clock 值）

## 用户态 libc

### 新建 `user/include/time.h`

```c
#ifndef _TIME_H
#define _TIME_H

#include <stddef.h>

typedef long time_t;
typedef long clock_t;

#define CLOCKS_PER_SEC 1000000

struct timespec {
    time_t tv_sec;
    long   tv_nsec;
};

#define CLOCK_MONOTONIC 1
#define TIME_UTC        1

int timespec_get(struct timespec *ts, int base);
clock_t clock(void);

#endif
```

### 新建 `user/lib/time.cc`

**`timespec_get(ts, base)`**：
- `base != TIME_UTC`：返回 0
- 调 `sys_gettime()` 获取纳秒值
- `ts->tv_sec = ns / 1000000000`
- `ts->tv_nsec = ns % 1000000000`
- 返回 `base`

**`clock()`**：
- 调 `sys_clock()` 获取 `cpu_time_ns`
- 返回 `(clock_t)(cpu_time_ns / 1000)`（纳秒→微秒，匹配 CLOCKS_PER_SEC=1000000）

### CMake 集成

`time.cc` 加入 libc target `c` 的源文件列表（`user/CMakeLists.txt`）。

## 语义说明

- `timespec_get(TIME_UTC)` 返回的是系统启动后的单调时间，不是 wall time（内核无 RTC）
- `clock()` 返回的是进程实际占用的 CPU 时间，不含被抢占或阻塞的时间
- `time()` / `gettimeofday()` 暂不实现（无 RTC 硬件支撑）
