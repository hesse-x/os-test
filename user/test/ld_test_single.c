// 场景 1：单依赖回归（NEEDED=libc.so）
// 验证递归加载对单依赖退化为 no-op，不破坏 hello_dyn 现状
#include <stdio.h>
int main(int argc, char** argv, char** envp) {
    (void)argc; (void)argv; (void)envp;
    if (printf("ld_single: ok\n") < 0) return 1;  // libc.so JUMP_SLOT
    return 0;
}
