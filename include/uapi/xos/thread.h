#ifndef COMMON_THREAD_H
#define COMMON_THREAD_H

#include <stdint.h>
#include <stddef.h>

// clone arg6 传递的线程清理信息（user 填，kernel 读）
// 不依赖 struct tcb 布局，保持 user/kernel 分层
struct thread_clone_info {
    int      detached;           // 1 = detached thread
    uint64_t tls_page;           // user vaddr of TLS+TCB page (0 if N/A)
    size_t   tls_total;          // size of TLS+TCB mapping
    uint64_t user_stack_base;    // user vaddr of stack base (incl guard)
    size_t   user_stack_size;    // stack+guard total size
};

#endif /* COMMON_THREAD_H */
