#ifndef CPU_IDT_H_
#define CPU_IDT_H_

#include <stdint.h>

/* Segment selectors */
#define KERNEL_CS 0x08
#define IDT_ENTRIES 256

/* Functions implemented in idt.c */
void set_idt_gate(int n, uint32_t handler);
void set_idt();

#endif // CPU_IDT_H_
