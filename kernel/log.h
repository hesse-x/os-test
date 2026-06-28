#ifndef KERNEL_LOG_H
#define KERNEL_LOG_H

#include <stdarg.h>

// Log levels
#define LOG_DEBUG   0  // dev debug (default off)
#define LOG_INFO    1  // normal flow key points
#define LOG_WARN    2  // recoverable anomaly
#define LOG_ERROR   3  // severe error (system can continue)
#define LOG_PANIC   4  // unrecoverable (system must stop)

// Global log level threshold (default LOG_WARN, debug build = LOG_DEBUG)
extern int log_level;

// printk(level, fmt, ...) — level-filtered serial output
// LOG_PANIC level unconditionally outputs and calls panic()
void printk(int level, const char *fmt, ...);

// panic(fmt, ...) — print reason + registers + stack trace + halt forever
void panic(const char *fmt, ...);

// dump_stack_trace() — walk RBP chain, print up to 16 kernel frames
void dump_stack_trace(void);

// BUG_ON(cond): cond true → panic (unrecoverable invariant violation)
#define BUG_ON(cond) \
    do { if (cond) panic("BUG_ON: %s at %s:%d", #cond, __FILE__, __LINE__); } while(0)

// WARN_ON(cond): cond true → printk(LOG_WARN) + return condition value
#define WARN_ON(cond) ({ \
    __typeof__(cond) __ret_warn_on = (cond); \
    if (__ret_warn_on) printk(LOG_WARN, "WARN_ON: %s at %s:%d", #cond, __FILE__, __LINE__); \
    __ret_warn_on; \
})

// ASSERT(cond): debug build = BUG_ON, release build = no-op
#ifdef NDEBUG
#define ASSERT(cond) ((void)0)
#else
#define ASSERT(cond) BUG_ON(!(cond))
#endif

#endif /* KERNEL_LOG_H */
