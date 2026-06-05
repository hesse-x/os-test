#ifndef KERNEL_H
#define KERNEL_H
#include <stddef.h>
#include <stdint.h>

struct boot_info;

extern "C" {
void kernel_main(boot_info *bi);
void kernel_init_finish();
}
#endif // KERNEL_H
