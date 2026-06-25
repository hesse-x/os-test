#include "stdio.h"
#include <unistd.h>

extern "C" int main(void);

extern "C" void _start() {
    fflush(stdout);

    int ret = main();

    fflush(stdout);
    _exit(ret);
}