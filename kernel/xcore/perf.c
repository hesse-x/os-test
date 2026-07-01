#include "kernel/xcore/perf.h"
#include "kernel/xcore/xtask.h"
#include "kernel/xcore/log.h"
#include "kernel/xcore/sched.h"
#include "kernel/xcore/mem/kasan.h"   // strncpy_from_user
#include "arch/x64/smp.h"
#include "arch/x64/apic.h"
#include "common/errno.h"
#include <string.h>

// ===================== Per-CPU sample buffers =====================
static perf_buf_t perf_buf[MAX_CPUS];
static perf_buf_t perf_nmi_buf[MAX_CPUS];   // NMI 专用，独立于 timer 的 perf_buf
static perf_sched_buf_t perf_sched_buf[MAX_CPUS];
static bool perf_active = false;

// ===================== Test boundary marks =====================
static perf_mark_t perf_marks[PERF_MARK_MAX];
static uint32_t perf_mark_count = 0;

// ===================== Initialization =====================
void perf_init(void) {
    for (int i = 0; i < MAX_CPUS; i++) {
        perf_buf[i].head = 0;
        perf_sched_buf[i].head = 0;
        perf_nmi_buf[i].head = 0;
    }
    // perf_active remains false — only start on sys_perf_ctl(START)
    printk(LOG_INFO, "perf: initialized (2000Hz NMI sampling ready)\n");
}

// ===================== Timer interrupt hook =====================
// Called from timer_handler() every tick (0.5ms @ 2000Hz).
// Records {rip, pid, cpu} into the per-CPU buffer if sampling is active.
// This path runs in interrupt context, must be O(1) — no allocations, no I/O.
// Note: NMI 采样（perf_nmi_handler）频率 = 定时器频率 2000Hz，本 timer 采样
// 仅作为低频补充。实际热点定位以 __nmi__ 段为准。
void perf_timer_handler(trapframe_t *tf) {
    if (!perf_active) return;

    int cpu = get_cpu_local()->cpu_id;
    xtask_t *cur = current_task;

    if (cur == get_cpu_local()->idle_proc) {
        // idle 快照：记录调度器状态
        perf_sched_buf_t *sbuf = &perf_sched_buf[cpu];
        uint32_t idx = sbuf->head;
        if (idx < PERF_SCHED_EVT_MAX) {
            // 统计全局 runnable 数
            uint32_t global_runnable = 0;
            for (int c = 0; c < ncpu; c++) {
                global_runnable += __atomic_load_n(&cpu_locals[c].run_count,
                                                   __ATOMIC_RELAXED);
            }
            sbuf->evts[idx].type = PERF_EVT_IDLE_SNAPSHOT;
            sbuf->evts[idx].cpu = (uint8_t)cpu;
            sbuf->evts[idx].pid = (uint32_t)-1;
            sbuf->evts[idx].ts = sched_clock();
            sbuf->evts[idx].payload[0] = __atomic_load_n(&cpu_locals[cpu].run_count,
                                                         __ATOMIC_RELAXED);
            sbuf->evts[idx].payload[1] = global_runnable;
            sbuf->evts[idx].payload[2] = 0;
            sbuf->head = idx + 1;
        }
        return;
    }

    perf_buf_t *buf = &perf_buf[cpu];
    uint32_t idx = buf->head;

    if (idx < PERF_SAMPLE_MAX) {
        buf->samples[idx].rip = tf->rip;
        buf->samples[idx].pid = cur ? (uint32_t)cur->pid : (uint32_t)-1;
        buf->samples[idx].cpu = (uint32_t)cpu;
        buf->head = idx + 1;           // commit: single volatile write
    }
    // buffer full: silently drop (overflow won't happen under normal test runs)
}

// ===================== NMI sampling handler =====================
// 走 NMI 上下文（不可屏蔽），能采到关中断/持锁/CPU 密集循环代码。
// 用独立 perf_nmi_buf 避免与 perf_timer_handler 的 perf_buf head 竞争
// （NMI 可打断正在写 perf_buf 的 timer_handler）。
// 每个 CPU 的 timer_handler 自己发 self-NMI（perf_nmi_kick），实现全核采样。
//
// NMI 上下文约束：不持任何锁、不调 printk/schedule/wake_process、
// 只读写 per-CPU 静态 perf_nmi_buf + 调 lapic_send_self_nmi（仅写 LAPIC MMIO）。
void perf_nmi_handler(trapframe_t *tf) {
    if (!perf_active) return;

    int cpu = get_cpu_local()->cpu_id;
    xtask_t *cur = current_task;
    if (!cur || cur == get_cpu_local()->idle_proc) return;

    perf_buf_t *buf = &perf_nmi_buf[cpu];
    uint32_t idx = buf->head;
    if (idx < PERF_SAMPLE_MAX) {
        buf->samples[idx].rip = tf->rip;
        buf->samples[idx].pid = (uint32_t)cur->pid;
        buf->samples[idx].cpu = (uint32_t)cpu;
        buf->head = idx + 1;
    }
}

// 在 timer_handler 中调用：每个调度 tick 发 1 次 self-NMI IPI。
// perf_active=false 时为 no-op（由 sys_perf_ctl STOP 置 false）。
void perf_nmi_kick(void) {
    if (!perf_active) return;
    lapic_send_self_nmi();
}

// ===================== wake_lost event recording =====================
void perf_record_wake_lost(xtask_t *target) {
    if (!perf_active) return;
    int cpu = get_cpu_local()->cpu_id;
    perf_sched_buf_t *sbuf = &perf_sched_buf[cpu];
    uint32_t idx = sbuf->head;
    if (idx < PERF_SCHED_EVT_MAX) {
        sbuf->evts[idx].type = PERF_EVT_WAKE_LOST;
        sbuf->evts[idx].cpu = (uint8_t)cpu;
        sbuf->evts[idx].pid = (uint32_t)(current_task ? current_task->pid : 0);
        sbuf->evts[idx].ts = sched_clock();
        sbuf->evts[idx].payload[0] = (uint64_t)target->pid;
        sbuf->evts[idx].payload[1] = (uint64_t)target->wait_event;
        sbuf->evts[idx].payload[2] = 0;
        sbuf->head = idx + 1;
    }
}

// ===================== Aggregation table for dump =====================
#define PERF_AGG_BUCKETS 1024   // power of 2 (mask modulus)
#define PERF_TOP_N       10     // top-N per test segment (small — many segments)

typedef struct {
    uint64_t rip;
    uint32_t pid;
    uint32_t count;
} perf_agg_t;

static perf_agg_t perf_agg[PERF_AGG_BUCKETS];

// Aggregate samples in [head_lo[cpu], head_hi[cpu]) range across all CPUs and
// emit top-N. Used for per-test-segment dumps.
static void perf_dump_segment(const char *label, uint32_t head_lo[MAX_CPUS],
                              uint32_t head_hi[MAX_CPUS], uint64_t *tx_full) {
    for (int i = 0; i < PERF_AGG_BUCKETS; i++) perf_agg[i].count = 0;

    uint32_t total = 0;
    for (int cpu = 0; cpu < ncpu; cpu++) {
        perf_buf_t *buf = &perf_buf[cpu];
        uint32_t lo = head_lo[cpu], hi = head_hi[cpu];
        if (hi > PERF_SAMPLE_MAX) hi = PERF_SAMPLE_MAX;
        if (lo > hi) lo = hi;
        for (uint32_t i = lo; i < hi; i++) {
            uint64_t rip = buf->samples[i].rip;
            uint32_t pid = buf->samples[i].pid;
            uint32_t h = ((pid >> 2) ^ (uint32_t)rip) & (PERF_AGG_BUCKETS - 1);
            for (int k = 0; k < PERF_AGG_BUCKETS; k++) {
                uint32_t idx = (h + k) & (PERF_AGG_BUCKETS - 1);
                if (perf_agg[idx].count == 0) {
                    perf_agg[idx].rip = rip;
                    perf_agg[idx].pid = pid;
                    perf_agg[idx].count = 1;
                    break;
                }
                if (perf_agg[idx].rip == rip && perf_agg[idx].pid == pid) {
                    perf_agg[idx].count++;
                    break;
                }
            }
            total++;
        }
    }

    printk(LOG_INFO, "PERF: [%s] samples=%u tx_full=%lu\n",
           label, total, *tx_full);
    for (int rank = 0; rank < PERF_TOP_N; rank++) {
        uint32_t max_idx = 0, max_count = 0;
        for (int i = 0; i < PERF_AGG_BUCKETS; i++) {
            if (perf_agg[i].count > max_count) {
                max_count = perf_agg[i].count;
                max_idx = i;
            }
        }
        if (max_count == 0) break;
        printk(LOG_INFO, "PERF:   rank=%d pid=%u rip=0x%lX count=%u (%lu%%)\n",
               rank + 1, perf_agg[max_idx].pid, perf_agg[max_idx].rip,
               max_count, total ? (uint64_t)max_count * 100 / total : 0);
        perf_agg[max_idx].count = 0;
    }
    *tx_full = 0;   // reset after attribution
}

// ===================== Dump all samples via serial =====================
static void perf_dump_all(void) {
    // If we have marks, dump per-test segments; else dump one whole range.
    if (perf_mark_count == 0) {
        uint32_t lo[MAX_CPUS], hi[MAX_CPUS];
        for (int cpu = 0; cpu < ncpu; cpu++) {
            lo[cpu] = 0;
            hi[cpu] = perf_buf[cpu].head;
        }
        uint64_t tx_full = 0;
        extern volatile uint64_t serial_tx_full_count;
        tx_full = serial_tx_full_count;
        serial_tx_full_count = 0;
        printk(LOG_INFO, "PERF: total tx_full=%lu\n", (uint64_t)tx_full);
        perf_dump_segment("__all__", lo, hi, &tx_full);

        // NMI buffer 整体聚合输出（不分段，NMI 采样频率高、量大）
        {
            uint32_t nlo[MAX_CPUS], nhi[MAX_CPUS];
            for (int cpu = 0; cpu < ncpu; cpu++) {
                nlo[cpu] = 0;
                nhi[cpu] = perf_nmi_buf[cpu].head;
            }
            uint64_t ntx = 0;
            perf_dump_segment("__nmi__", nlo, nhi, &ntx);
        }
        printk(LOG_INFO, "PERF: perf_dump_end\n");
        return;
    }

    // Walk marks: segment i = [mark[i-1].head, mark[i].head). mark[0] is start.
    uint32_t prev_head[MAX_CPUS];
    for (int cpu = 0; cpu < ncpu; cpu++) prev_head[cpu] = 0;

    for (uint32_t m = 0; m < perf_mark_count; m++) {
        perf_mark_t *mk = &perf_marks[m];
        if (!mk->valid) continue;

        // Segment label: test name (or "__end__" for the final mark)
        char label[PERF_MARK_NAME];
        __builtin_memcpy(label, mk->name, PERF_MARK_NAME);
        label[PERF_MARK_NAME - 1] = '\0';

        uint64_t tx_full = 0;
        extern volatile uint64_t serial_tx_full_count;
        tx_full = serial_tx_full_count;
        perf_dump_segment(label, prev_head, mk->buf_head, &tx_full);

        // Advance prev_head for next segment
        for (int cpu = 0; cpu < ncpu; cpu++)
            prev_head[cpu] = mk->buf_head[cpu];
    }

    // NMI buffer 整体聚合输出（不分段，NMI 采样频率高、量大）
    {
        uint32_t nlo[MAX_CPUS], nhi[MAX_CPUS];
        for (int cpu = 0; cpu < ncpu; cpu++) {
            nlo[cpu] = 0;
            nhi[cpu] = perf_nmi_buf[cpu].head;
        }
        uint64_t ntx = 0;
        perf_dump_segment("__nmi__", nlo, nhi, &ntx);
    }
    printk(LOG_INFO, "PERF: perf_dump_end\n");

    // 调度事件 dump (sched events — output volume is small, keep as-is)
    uint32_t evt_total = 0;
    uint32_t idle_true = 0, idle_false = 0, wake_lost = 0;
    for (int cpu = 0; cpu < ncpu; cpu++) {
        perf_sched_buf_t *sbuf = &perf_sched_buf[cpu];
        uint32_t n = sbuf->head;
        for (uint32_t i = 0; i < n; i++) {
            perf_sched_evt_t *e = &sbuf->evts[i];
            switch (e->type) {
            case PERF_EVT_IDLE_SNAPSHOT:
                if (e->payload[1] == 0) idle_true++; else idle_false++;
                break;
            case PERF_EVT_WAKE_LOST:
                wake_lost++;
                break;
            }
            evt_total++;
        }
    }
    printk(LOG_INFO, "PERF: sched_evt_total=%u idle_true=%u idle_false=%u wake_lost=%u\n",
           evt_total, idle_true, idle_false, wake_lost);
}

// ===================== Syscall handler =====================
// Signature matches syscall_fn_t: cmd (rdi), arg1 (rsi) used for MARK name ptr.
int64_t sys_perf_ctl(int64_t cmd, int64_t arg1, int64_t _2, int64_t _3, int64_t _4, int64_t _5) {
    (void)_2; (void)_3; (void)_4; (void)_5;
    switch (cmd) {
    case PERF_CTL_START:
        // Clear all per-CPU buffers and marks
        for (int i = 0; i < MAX_CPUS; i++) {
            perf_buf[i].head = 0;
            perf_sched_buf[i].head = 0;
            perf_nmi_buf[i].head = 0;
        }
        perf_mark_count = 0;
        for (int i = 0; i < PERF_MARK_MAX; i++) perf_marks[i].valid = false;
        __atomic_signal_fence(__ATOMIC_SEQ_CST);
        perf_active = true;
        pit_nmi_start();   // 2000Hz NMI 采样（self-IPI NMI，每 timer tick 1 个）
        printk(LOG_INFO, "perf: sampling started (2000Hz NMI)\n");
        return 0;

    case PERF_CTL_STOP:
        perf_active = false;
        pit_nmi_stop();
        printk(LOG_INFO, "perf: sampling stopped\n");
        return 0;

    case PERF_CTL_STOP_DUMP:
        perf_active = false;
        pit_nmi_stop();
        printk(LOG_INFO, "perf: sampling stopped, dumping samples...\n");
        perf_dump_all();
        return 0;

    case PERF_CTL_MARK: {
        // Record a test boundary: snapshot sched_clock + per-CPU buffer heads.
        // arg1 = user pointer to test name (may be NULL → "__end__").
        if (!perf_active) return -EINVAL;
        if (perf_mark_count >= PERF_MARK_MAX) return -ENOMEM;

        perf_mark_t *mk = &perf_marks[perf_mark_count];
        mk->ts = sched_clock();
        for (int cpu = 0; cpu < ncpu; cpu++)
            mk->buf_head[cpu] = perf_buf[cpu].head;

        if (arg1 == 0) {
            __builtin_memcpy(mk->name, "__end__", 8);
        } else {
            long n = strncpy_from_user(mk->name, (const char __user *)arg1,
                                       PERF_MARK_NAME - 1);
            if (n < 0) return -EFAULT;
            mk->name[n] = '\0';
        }
        mk->valid = true;
        perf_mark_count++;
        return 0;
    }

    default:
        return -EINVAL;
    }
}
