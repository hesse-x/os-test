#ifndef MACRO_H
#define MACRO_H

#define ALIGN_UP(val, aligned) ((val + aligned - 1) & ~(aligned - 1))
#define ALIGN_DOWN(val, aligned) ((val) & ~(aligned - 1))
#define KERNEL_VMA_BOUNDARY 0xFFFFFFFF80000000ULL

#endif // MACRO_H
