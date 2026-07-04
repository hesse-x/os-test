#ifndef USER_SYS_TLS_H
#define USER_SYS_TLS_H

#include <stddef.h>
#include <sys/cdefs.h>

#ifdef __cplusplus
extern "C" {
#endif

struct thread_entry;

// TLS 模板信息（variant II 布局）
// 阶段一裁剪到最小：pthread_create 只需拷贝合并后的模板，
// 不需 per-object 偏移。未来 dlopen + __tls_get_addr 再加 per-object 信息。
struct tls_info {
    void *tdata_template;       // TLS 模板（合并后的 tdata 拷贝源）
    size_t tdata_size;          // tdata 总大小
    size_t tbss_size;           // tbss 总大小
    size_t alignment;           // 最大对齐
    size_t size;                // 总大小（tdata + tbss + padding，variant II 块大小）
};

// 全局单例，pthread_create 读取
extern struct tls_info __g_tls_info;

// 初始化函数（静态路径：读链接器符号填 __g_tls_info）
void __libc_tls_init(void);

#include "pthread.h"

// TCB structure — placed at FS_BASE, %fs:0 returns &TCB.self
// variant II layout: [tls_block (.tdata+.tbss)] [TCB]
// FS_BASE points to TCB. TLS vars accessed via %fs:(-offset).
struct tcb {
    void *self;                          // points to this tcb
    pid_t tid;                           // thread ID (kernel writes via clear_tid)
    void *clear_tid_addr;                // for CLONE_CHILD_CLEARTID + futex_wake on exit
    int cancel_state;                    // PTHREAD_CANCEL_ENABLE/DISABLE
    int cancel_type;                     // PTHREAD_CANCEL_DEFERRED/ASYNCHRONOUS
    void *tsd[PTHREAD_KEYS_MAX];         // TSD values (pthread_key)
    __pthread_cleanup_handler_t *cleanup_head;
    int detached;
    struct thread_entry *entry;          // back-pointer to slot (NULL for main thread)
    // === child thread start info (set by pthread_create, read by __pthread_start) ===
    void *(*start_routine)(void *);
    void *arg;
    void *tls_page;                      // for munmap on exit (NULL for main thread)
    size_t tls_total;                    // for munmap on exit
    int errno_val;                       // per-thread errno（__errno_location 返回 & 此字段）
};

LIBC_EXPORT struct tcb *__pthread_current_tcb(void);

// Allocate TLS block + TCB for a thread (main or child).
// Returns TCB pointer (= FS_BASE). tls_page_out/tls_total_out for later munmap.
// Exported for pthread_create (pthread.cc) to call for child threads.
LIBC_EXPORT struct tcb *alloc_tls_block(void **tls_page_out, size_t *tls_total_out);

// Called by __libc_tls_init (static path) and __libc_start_main (dynamic path)
// after __g_tls_info is filled. Allocates main thread TCB + sets FS_BASE +
// set_tid_address + registers cancel handler.
LIBC_EXPORT void __libc_tls_init_rest(void);

// Cancel check handler — registered via sys_pthread_set_cancel_handler.
// Called by kernel deliver_signal on SIGCANCEL. Reads TCB cancel_state:
//   ENABLE  → exit (phase 2: sys_exit; phase 3: pthread_exit(PTHREAD_CANCELED))
//   DISABLE → returns (sigreturn restores context)
LIBC_EXPORT void __pthread_cancel_check(int sig);

#ifdef __cplusplus
}
#endif

#endif // USER_SYS_TLS_H
