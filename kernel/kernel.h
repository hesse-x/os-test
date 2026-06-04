#ifndef KERNEL_H
#define KERNEL_H
#include <stddef.h>
#include <stdint.h>
extern "C" {
void kernel_main(int32_t magic_num, uintptr_t addr);
void kernel_init_finish();
}
#endif // KERNEL_H
