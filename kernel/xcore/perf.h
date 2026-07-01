#ifndef KERNEL_XCORE_PERF_H
#define KERNEL_XCORE_PERF_H

#include <stdint.h>
#include <stdbool.h>
#include "arch/x64/trap.h"      // trapframe_t
#include "arch/x64/smp.h"       // MAX_CPUS, get_cpu_local, ncpu
#include "kernel/xcore/xtask.h" // xtask_t

// ===================== Perf sample buffer =====================

#define PERF_SAMPLE_MAX 65536   // 2000Hz × ~32s = 64k, 安全上限

typedef struct {
    uint64_t rip;       // 指令地址（用户态 0x40xxxx / 内核态 0xFFFFFFFF80xxxxxx）
    uint32_t pid;       // 当前进程 PID（idle 为 (uint32_t)-1）
    uint32_t cpu;       // CPU ID（per-CPU buffer 标识）
} __attribute__((packed)) perf_sample_t;

typedef struct {
    volatile uint32_t head;              // 下一个写入条目（中断中只写此变量）
    uint32_t _pad;                       // 对齐到 8 字节
    perf_sample_t samples[PERF_SAMPLE_MAX];
} __attribute__((aligned(64))) perf_buf_t;

// ===================== Perf sched event buffer (idle / wake_lost) =====================

typedef enum {
    PERF_EVT_PC = 0,        // 现有 PC 采样（保持不变）
    PERF_EVT_IDLE_SNAPSHOT, // idle 时调度器快照
    PERF_EVT_WAKE_LOST,     // wake_process 条件不匹配
} perf_evt_type_t;

typedef struct {
    uint8_t  type;          // perf_evt_type_t
    uint8_t  cpu;
    uint16_t pad;
    uint32_t pid;
    uint64_t ts;            // sched_clock()
    uint64_t payload[3];    // 类型相关数据
} __attribute__((packed)) perf_sched_evt_t;

#define PERF_SCHED_EVT_MAX 8192

typedef struct {
    volatile uint32_t head;
    uint32_t _pad;
    perf_sched_evt_t evts[PERF_SCHED_EVT_MAX];
} __attribute__((aligned(64))) perf_sched_buf_t;

// ===================== Perf control commands (for sys_perf_ctl) =====================
#define PERF_CTL_START     0   // 清空 buffer 并开启采样
#define PERF_CTL_STOP      1   // 停止采样（不 dump）
#define PERF_CTL_STOP_DUMP 2   // 停止采样 + 串口 dump 全部样本

// ===================== Functions =====================

// 初始化 per-CPU buffer（xcore_init 中调用）
void perf_init(void);

// 由 timer_handler 在每 tick 调用（perf_active 时记录采样）
void perf_timer_handler(trapframe_t *tf);

// NMI 采样入口（由 trap_dispatch 在 vector 2 时调用）
// 独立于 perf_timer_handler 的 100Hz 采样，走 NMI 上下文（不可屏蔽），
// 能采到关中断/持锁/CPU 密集循环代码。使用独立 perf_nmi_buf 避免与
// perf_buf 的 head 竞争。BSP 在此处转发 NMI IPI 给所有 AP（全核采样）。
void perf_nmi_handler(trapframe_t *tf);

// 在 timer_handler 中调用：每个调度 tick 发 1 次 self-NMI IPI。
// perf_active=false 时为 no-op。
void perf_nmi_kick(void);

// 由 xcall_dispatch 分发 SYS_PERF_CTL（通过 syscall_fn_t 6-arg 指针调用）
int64_t sys_perf_ctl(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);

// 记录 wake_process 唤醒丢失事件（条件不匹配的 BLOCKED 进程）
void perf_record_wake_lost(xtask_t *target);

// ===================== Test boundary marks (for per-test segmentation) =====================
#define PERF_MARK_MAX   64      // max tests + 1
#define PERF_MARK_NAME  32      // max test name length (incl. NUL)

typedef struct {
    uint64_t ts;                 // sched_clock() at mark time
    uint32_t buf_head[MAX_CPUS]; // per-CPU buffer head snapshot at mark time
    char     name[PERF_MARK_NAME]; // test name ("__end__" for end mark)
    bool     valid;
} perf_mark_t;

#endif // KERNEL_XCORE_PERF_H
