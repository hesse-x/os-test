#ifndef KERNEL_H
#define KERNEL_H
#include <stddef.h>
#include <stdint.h>
extern "C" {
void kernel_main(int32_t magic_num, uintptr_t addr);
}
#endif // KERNEL_H
