#include "stdio.h"
#include "common/syscall.h"
#include "arch/x64/utils.h"

extern "C" int main(void);

extern "C" void _start() {
    /* stdout/stderr are already initialized via static constructors
     * or static initialization; call stdio_init as a placeholder
     * for future dynamic initialization (e.g. sys_write setup) */
    fflush(stdout);

    int ret = main();

    fflush(stdout);
    sys_exit(ret);
}