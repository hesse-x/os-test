#ifndef GDT_H
#define GDT_H
#include "stdint.h"

struct segdesc {
    unsigned sd_lim_15_0 : 16;      // low bits of segment limit
    unsigned sd_base_15_0 : 16;     // low bits of segment base address
    unsigned sd_base_23_16 : 8;     // middle bits of segment base address
    unsigned sd_type : 4;           // segment type (see STS_ constants)
    unsigned sd_s : 1;              // 0 = system, 1 = application
    unsigned sd_dpl : 2;            // descriptor Privilege Level
    unsigned sd_p : 1;              // present
    unsigned sd_lim_19_16 : 4;      // high bits of segment limit
    unsigned sd_avl : 1;            // unused (available for software use)
    unsigned sd_rsv1 : 1;           // reserved
    unsigned sd_db : 1;             // 0 = 16-bit segment, 1 = 32-bit segment
    unsigned sd_g : 1;              // granularity: limit scaled by 4K when set
    unsigned sd_base_31_24 : 8;     // high bits of segment base address
} __attribute__ ((packed));


struct pseudodesc {
    uint16_t pd_lim;        // Limit
    uint32_t pd_base;      // Base address
} __attribute__ ((packed));

void update_gdt(uint32_t new_offset, uint32_t gdt_addr);

void print_gdt(struct pseudodesc *gdt);
void print_ptr(void *ptr);
#endif // GDT_H
