#include "kernel/xcore/rcu.h"
#include "kernel/xcore/log.h"
#include "kernel/xcore/xtask.h"
#include "arch/x64/utils.h"   // pause
#include "arch/x64/smp.h"     // ncpu, current_task

rcu_state_t rcu_state;

void rcu_init(void) {
    atomic_set(&rcu_state.global_gen, 0);
    for (int i = 0; i < RCU_MAX_CPUS; i++)
        atomic_set(&rcu_state.cpu_gen[i], 0);
    rcu_state.writer_lock = SPINLOCK_INIT;
}

void synchronize_rcu(void) {
    spin_lock(&rcu_state.writer_lock);
    int new_gen = atomic_add_return(&rcu_state.global_gen, 1);
    int my_cpu = get_cpu_local()->cpu_id;
    // Immediately advance our own cpu_gen — the caller is outside any
    // RCU read-side critical section, so this CPU is already in a
    // quiescent state.  Without this, a busy-waiting synchronize_rcu
    // would never see its own cpu_gen advance (it never passes through
    // rcu_read_unlock while spinning).
    atomic_set(&rcu_state.cpu_gen[my_cpu], new_gen - 1);
    // Wait for all OTHER online CPUs to observe at least new_gen - 1
    for (int i = 0; i < ncpu; i++) {
        if (i == my_cpu) continue;
        int spins = 0;
        while (atomic_read(&rcu_state.cpu_gen[i]) < new_gen - 1) {
            __asm__ volatile("pause");
            if (++spins > 100000000) {
                // Stall watchdog：打印每个 CPU 的落后量 + 当前 task，定位"哪个
                // CPU 没过 grace period + 它在跑什么"。比单行 WARN_ON 信息量大
                // 一个数量级——原 Bug 6 靠人肉猜"哪个 CPU 卡了"，现在直接给出。
                // 纯观测点，不改 RCU 语义（仍 continue waiting）。
                printk(LOG_WARN, "RCU stall: global_gen=%d waiter_cpu=%d\n",
                       new_gen - 1, i);
                for (int c = 0; c < ncpu; c++) {
                    int cg = atomic_read(&rcu_state.cpu_gen[c]);
                    int lag = (new_gen - 1) - cg;
                    xtask_t *t = cpu_locals[c]._cur_proc;
                    const char *stname = t ? "???" : "idle";
                    if (t) {
                        stname = (t->state == RUNNING) ? "RUNNING"
                               : (t->state == BLOCKED) ? "BLOCKED"
                               : (t->state == READY)   ? "READY"
                               : (t->state == ZOMBIE)  ? "ZOMBIE"
                               : "other";
                    }
                    printk(LOG_WARN, "  cpu%d: cpu_gen=%d lag=%d task_pid=%d state=%s\n",
                           c, cg, lag, t ? (int)t->pid : -1, stname);
                }
                WARN_ON(1);
                spins = 0;  // continue waiting after warning
            }
        }
    }
    spin_unlock(&rcu_state.writer_lock);
}
