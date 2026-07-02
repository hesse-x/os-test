// user/lib/start_main.cc — __libc_start_main 统一启动（同源双产物）
// ld.md §3.5.2 / plan_ld2b3 T11
//
// libc.a 编译 -DDYNAMIC=0（静态路径调 __libc_tls_init）
// libc.so 编译 -DDYNAMIC=1（动态路径调 collect_tls_from_link_map + __libc_tls_init_rest）

#include "stdio.h"
#include "stdlib.h"
#include "sys/tls.h"
#include <unistd.h>
#include "common/syscall.h"

typedef void (*init_func_t)(void);
extern "C" void __libc_run_init_array(init_func_t *start, init_func_t *end);
extern "C" void __libc_run_fini_array(init_func_t *start, init_func_t *end);
extern "C" void __libc_run_atexit(void);

// 静态路径 TLS 初始化（tls.cc）：读链接器符号填 __g_tls_info + alloc TCB + FS_BASE + ...
extern "C" void __libc_tls_init(void);
extern "C" void __libc_tls_init_rest(void);  // alloc TCB + FS_BASE + set_tid_address + cancel handler
extern "C" struct tls_info __g_tls_info;

// 动态路径：ld.so 导出的 link_map 链表
#if DYNAMIC
#include "sys/link_map.h"
extern "C" struct tls_info collect_tls_from_link_map(struct link_map *lmap);
#endif

// atexit 回调无参数，用静态变量记录 fini 范围
static init_func_t *g_fini_start;
static init_func_t *g_fini_end;
extern "C" void __libc_fini_array_trampoline(void) {
    __libc_run_fini_array(g_fini_start, g_fini_end);
}

// 统一启动函数，同源双产物
// 参数：main, argc, argv, init_array 范围 [init_start, init_end),
//       fini_array 范围 [fini_start, fini_end)
// （原 SysV ABI 的 init/fini/rtld_fini/stack_end 不再使用，复用寄存器传范围）
extern "C" int __libc_start_main(int (*main)(int, char**, char**),
                                  int argc, char **argv,
                                  init_func_t *init_start, init_func_t *init_end,
                                  init_func_t *fini_start, init_func_t *fini_end) {
    // 1. TLS 模板发现 + 主线程 TCB 分配（静态/动态分流）
#if DYNAMIC
    // 动态：遍历 _dl_link_map 合并 PT_TLS 填 __g_tls_info，再 alloc TCB
    __g_tls_info = collect_tls_from_link_map(_dl_link_map);
    __libc_tls_init_rest();
#else
    // 静态：__libc_tls_init 全包（first 填 __g_tls_info + rest alloc TCB + ...）
    __libc_tls_init();
#endif

    // 2. 跑 .init_array
    __libc_run_init_array(init_start, init_end);

    // 3. 注册 .fini_array 到 atexit
    g_fini_start = fini_start;
    g_fini_end = fini_end;
    atexit(__libc_fini_array_trampoline);
    // rtld_fini（ld.so 的 fini）本方案 ld.so 不注册，传 NULL

    // 4. 算 envp
    char **envp = argv + argc + 1;

    // 5. 跑 main
    int ret = main(argc, argv, envp);

    // 6. exit → 跑 atexit handlers（含 .fini_array）→ sys_exit_group（杀全部线程）
    fflush(stdout);
    __libc_run_atexit();
    sys_exit_group(ret);
    __builtin_unreachable();
}
