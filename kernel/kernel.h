#ifndef KERNEL_H
#define KERNEL_H
#include <stddef.h>
#include <stdint.h>
#include "kernel/sparse.h"

typedef struct boot_info boot_info;

void kernel_main(boot_info *bi);
void kernel_init_finish();
#endif // KERNEL_H
