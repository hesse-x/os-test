#include "stdio.h"
#include <sys.h>

extern "C" int main(void);
extern "C" void __libc_tls_init();

extern "C" void _start() {
    // Initialize TLS (FS_BASE + set_tid_address for main thread)
    __libc_tls_init();

    fflush(stdout);

    int ret = main();

    fflush(stdout);
    sys_exit_group(ret);
}