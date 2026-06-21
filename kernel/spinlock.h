#ifndef KERNEL_SPINLOCK_H
#define KERNEL_SPINLOCK_H

#include <stdint.h>

typedef struct spinlock_t {
    volatile uint32_t locked;
} spinlock_t;

#define SPINLOCK_INIT (spinlock_t){0}

static inline void spin_lock(spinlock_t *lk) {
    while (__atomic_exchange_n(&lk->locked, 1, __ATOMIC_ACQUIRE) == 1)
        __asm__ volatile("pause");
}

static inline void spin_unlock(spinlock_t *lk) {
    __atomic_store_n(&lk->locked, 0, __ATOMIC_RELEASE);
}

static inline void spin_lock_irqsave(spinlock_t *lk, uint64_t *flags) {
    uint64_t f;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(f));
    *flags = f;
    spin_lock(lk);
}

static inline void spin_unlock_irqrestore(spinlock_t *lk, uint64_t flags) {
    spin_unlock(lk);
    __asm__ volatile("pushq %0; popfq" : : "r"(flags));
}

#endif // KERNEL_SPINLOCK_H
