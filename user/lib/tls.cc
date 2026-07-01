// user/lib/tls.cc — TLS template snapshot + main thread TCB init + cancel handler
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include "sys/tls.h"
#include "pthread.h"
#include "common/syscall.h"
#include "common/mman.h"
#include "common/signal.h"

// 全局 TLS 模板信息单例（pthread_create 读取）
extern "C" struct tls_info __g_tls_info = {0};

// 链接器符号（user_linker.ld 定义）
// 链接脚本中 `__xxx = SIZEOF(...)` 的符号，其地址即值，故声明为 char[]，
// 用 (size_t)__xxx 取地址作为数值，避免生成对该地址的内存读取。
extern "C" char __tls_template_start[];
extern "C" char __tls_template_end[];
extern "C" char __tdata_size[];
extern "C" char __tbss_size[];
extern "C" char __tdata_align[];
extern "C" char __tbss_align[];
extern "C" char __tls_align[];

// 读链接器符号填 __g_tls_info（进程级模板元数据）
// 对应 ld.md §3.5.3 collect_tls_from_linker_symbols
extern "C" void __libc_tls_init_first(void) {
    __g_tls_info.tdata_template = (void *)__tls_template_start;
    __g_tls_info.tdata_size = (size_t)__tdata_size;
    __g_tls_info.tbss_size = (size_t)__tbss_size;
    __g_tls_info.alignment = (size_t)__tls_align;
    if (__g_tls_info.alignment < 8) __g_tls_info.alignment = 8;
    __g_tls_info.size = __g_tls_info.tdata_size + __g_tls_info.tbss_size;
    // 按 alignment 对齐 tls_block
    if (__g_tls_info.alignment > 0) {
        __g_tls_info.size = (__g_tls_info.size + __g_tls_info.alignment - 1)
                            & ~(__g_tls_info.alignment - 1);
    }
}

// Allocate TLS block + TCB for a thread (main or child).
// Returns TCB pointer (= FS_BASE). tls_page_out/tls_total_out for later munmap.
static struct tcb *alloc_tls_block(void **tls_page_out, size_t *tls_total_out) {
    size_t tdata_size = __g_tls_info.tdata_size;
    size_t tbss_size = __g_tls_info.tbss_size;
    size_t tls_align = __g_tls_info.alignment;
    if (tls_align < 8) tls_align = 8;

    size_t tls_block = tdata_size + tbss_size;
    if (tls_align > 0) {
        tls_block = (tls_block + tls_align - 1) & ~(tls_align - 1);
    }
    size_t page_size = 4096;
    size_t total = tls_block + sizeof(struct tcb);
    total = (total + page_size - 1) & ~(page_size - 1);

    void *tls_page = mmap(NULL, total, PROT_READ | PROT_WRITE,
                          MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (tls_page == MAP_FAILED) return NULL;
    memset(tls_page, 0, total);

    if (tdata_size > 0) {
        memcpy(tls_page, __g_tls_info.tdata_template, tdata_size);
    }

    struct tcb *tcb = (struct tcb *)((char *)tls_page + tls_block);
    tcb->self = tcb;
    tcb->clear_tid_addr = &tcb->tid;
    tcb->cancel_state = PTHREAD_CANCEL_ENABLE;
    tcb->cancel_type = PTHREAD_CANCEL_DEFERRED;
    tcb->cleanup_head = NULL;
    tcb->detached = 0;
    tcb->start_routine = NULL;
    tcb->arg = NULL;
    tcb->tls_page = tls_page;
    tcb->tls_total = total;

    if (tls_page_out) *tls_page_out = tls_page;
    if (tls_total_out) *tls_total_out = total;
    return tcb;
}

// 主线程 TLS 初始化：填 __g_tls_info + 分配 TCB + 设 FS_BASE + set_tid_address + 注册 cancel handler
extern "C" void __libc_tls_init(void) {
    __libc_tls_init_first();

    void *tls_page;
    size_t tls_total;
    struct tcb *tcb = alloc_tls_block(&tls_page, &tls_total);
    if (!tcb) return;
    tcb->tid = (pid_t)sys_gettid();
    tcb->tls_page = NULL;   // 主线程：退出时不 munmap
    tcb->tls_total = 0;

    sys_arch_prctl(ARCH_SET_FS, (int64_t)tcb);
    sys_set_tid_address((uint64_t)&tcb->tid);

    // 注册 cancel check handler（内核投递 SIGCANCEL 时调用）
    sys_pthread_set_cancel_handler((uint64_t)__pthread_cancel_check);
}

extern "C" struct tcb *__pthread_current_tcb(void) {
    void *self;
    __asm__ volatile("movq %%fs:0, %0" : "=r"(self));
    return (struct tcb *)self;
}

// SIGCANCEL handler — kernel via deliver_signal.
// cancel_state in TCB decides: ENABLE → exit, DISABLE → return.
extern "C" void __pthread_cancel_check(int sig) {
    (void)sig;
    struct tcb *tcb = __pthread_current_tcb();
    if (tcb->cancel_state == PTHREAD_CANCEL_ENABLE) {
        // Phase 3（第 3 步）改为 pthread_exit(PTHREAD_CANCELED)
        // 此步 pthread_exit 尚未实现，先用 sys_exit
        sys_exit(-1);
        __builtin_unreachable();
    }
    // DISABLE: return to sigreturn trampoline, resume original context
}
