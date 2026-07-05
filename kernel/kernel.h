#ifndef KERNEL_H
#define KERNEL_H
#include "boot/boot.h"
#include <stddef.h>
#include <stdint.h>

void kernel_main(boot_info *bi);
void kernel_init_finish(void);

// Layered init functions (defined in their own .c files)
void xcore_init(boot_info *bi);
void driver_init(void);
void bsd_init(void);
#endif // KERNEL_H
