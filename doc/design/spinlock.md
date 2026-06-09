# 自旋锁设计

> **已实现**。`spin_lock_irqsave` / `spin_unlock_irqrestore` 已添加，用于 per-CPU `scheduler_lock`。

## 概述

内核最小自旋锁原语，作为步骤 1 独立实现，为步骤 2 BKL 提供基础。

## 设计决策

| # | 决策 | 选择 | 理由 |
|---|------|------|------|
| 1 | 原子操作实现 | GCC `__atomic` builtins | freestanding 可用，编译器自动 lowering 为 `xchg`，自动插入内存屏障 |
| 2 | `spin_lock` 是否内含 `cli` | 否 | 中断控制由调用者负责，BKL 步骤 2 在汇编入口做 `cli`，锁本身保持纯自旋语义 |
| 3 | 结构体字段 | 仅 `volatile uint32_t locked` | 步骤 1 只有一把 BKL，调试字段无使用场景，YAGNI |
| 4 | 放置文件 | `kernel/spinlock.h` | 锁是内核同步原语，非架构特定；`utils.h` 已过膨胀 |
| 5 | unlock 内存序 | `__ATOMIC_RELEASE` | 与 lock 端 `__ATOMIC_ACQUIRE` 配对，精确表达 acquire-release 同步语义 |
| 6 | 自旋循环 | `exchange` + `pause` | 2-4 核 BKL 场景无竞争瓶颈，简单优先 |
| 7 | 初始化 | `SPINLOCK_INIT {0}` 宏 | 单字段无复杂初始化逻辑，无需 `spin_init()` |
| 8 | `locked` 类型 | `uint32_t` | x86-64 上生成 `xchgll`，显式表达宽度意图 |

## 实现

```c
struct spinlock_t {
    volatile uint32_t locked;
};

#define SPINLOCK_INIT {0}

static inline void spin_lock(spinlock_t *lk) {
    while (__atomic_exchange_n(&lk->locked, 1, __ATOMIC_ACQUIRE) == 1)
        __asm__ volatile("pause");
}

static inline void spin_unlock(spinlock_t *lk) {
    __atomic_store_n(&lk->locked, 0, __ATOMIC_RELEASE);
}
```

## 验证

编译通过 + 单核启动无回归。
