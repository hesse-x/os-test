#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include <unistd.h>
#include "common/syscall.h"
#include "sys/tls.h"

extern "C" int main(void);

// TCB 结构（与 thread.md / ld.md §3.5.2 一致）
struct tcb {
    struct tcb *self;          // %fs:0 返回此地址
    int tid;                   // 线程 ID
    void *clear_tid_addr;      // set_tid_address 地址（主线程 NULL）
    int cancel_state;
    int cancel_type;
    void *tsd[128];            // TSD values
};

extern "C" __attribute__((naked)) void _start() {
    __asm__ volatile(
        "andq $-16, %%rsp\n\t"
        "subq $8, %%rsp\n\t"
        "jmp __libc_start\n\t"
        :::);
}

// 临时启动路径：TLS 初始化 + main + exit
// 阶段 2b+3 替换为 __libc_start_main（ld.md §3.5）
extern "C" void __libc_start() {
    // 1. 读链接器符号填 tls_info
    __libc_tls_init();

    // 2. 分配主线程 TLS 块（variant II 布局）
    struct tls_info *ti = &__g_tls_info;
    if (ti->size > 0) {
        // 分配 TLS 块 + TCB + 对齐 padding
        size_t block_sz = ti->size;
        // 按 alignment 对齐分配
        size_t alloc_sz = block_sz + sizeof(struct tcb) + ti->alignment;
        char *raw = (char *)malloc(alloc_sz);
        // 对齐 TLS 块起始地址
        uintptr_t base = (uintptr_t)raw;
        if (ti->alignment > 0)
            base = (base + ti->alignment - 1) & ~((uintptr_t)ti->alignment - 1);
        char *tls_block = (char *)base;
        // FS_BASE 指向 TLS 块末尾（variant II）
        char *fs_base = tls_block + block_sz;
        struct tcb *tcb = (struct tcb *)fs_base;
        tcb->self = tcb;
        tcb->tid = (int)sys_getpid();
        tcb->clear_tid_addr = 0;
        tcb->cancel_state = 0;  // PTHREAD_CANCEL_ENABLE
        tcb->cancel_type = 0;   // PTHREAD_CANCEL_DEFERRED
        // 拷贝 tdata 模板，BSS 清零
        if (ti->tdata_size > 0)
            memcpy(tls_block, ti->tdata_template, ti->tdata_size);
        if (ti->tbss_size > 0)
            memset(tls_block + ti->tdata_size, 0, ti->tbss_size);
        // 设 FS_BASE
        sys_arch_prctl(ARCH_SET_FS, (int64_t)fs_base);
    }

    // 3. 跑 main
    fflush(stdout);
    int ret = main();
    fflush(stdout);
    _exit(ret);
}
