#ifndef KERNEL_MEMLAYOUT_H_
#define KERNEL_MEMLAYOUT_H_

#include "stdint.h"

/* This file contains the definitions for memory management in our OS. */

/* global segment number */
#define SEG_KTEXT 1
#define SEG_KDATA 2
#define SEG_UTEXT 3
#define SEG_UDATA 4
#define SEG_TSS 5

/* global descriptor numbers */
#define GD_KTEXT ((SEG_KTEXT) << 3) // kernel text
#define GD_KDATA ((SEG_KDATA) << 3) // kernel data
#define GD_UTEXT ((SEG_UTEXT) << 3) // user text
#define GD_UDATA ((SEG_UDATA) << 3) // user data
#define GD_TSS ((SEG_TSS) << 3)     // task segment selector

#define DPL_KERNEL (0)
#define DPL_USER (3)

#define KERNEL_CS ((GD_KTEXT) | DPL_KERNEL)
#define KERNEL_DS ((GD_KDATA) | DPL_KERNEL)
#define USER_CS ((GD_UTEXT) | DPL_USER)
#define USER_DS ((GD_UDATA) | DPL_USER)

#define E820MAX 20 // number of entries in E820MAP
#define E820_ARM 1 // address range memory
#define E820_ARR 2 // address range reserved

struct e820map {
  int nr_map;
  struct {
    uint64_t addr;
    uint64_t size;
    uint32_t type;
  } __attribute__((packed)) map[E820MAX];
};

// Page Directory Entry
struct pde {
  uint32_t present : 1;            // 存在位
  uint32_t read_write : 1;         // 读/写位
  uint32_t user_supervisor : 1;    // 用户/内核位
  uint32_t page_write_through : 1; // 写直达位
  uint32_t page_cache_disable : 1; // 禁用缓存位
  uint32_t accessed : 1;           // 访问位
  uint32_t reserved : 1;           // 保留位
  uint32_t page_size : 1;          // 页大小位（0表示4KB，1表示4MB）
  uint32_t global : 1;             // 全局页位
  uint32_t available : 3;          // 可用位（供操作系统使用）
  uint32_t page_table_base : 20; // 页表基地址（物理地址的高20位）
} __attribute__((packed));

// Page Table Entry
struct pte {
  uint32_t present : 1;              // 存在位
  uint32_t read_write : 1;           // 读/写位
  uint32_t user_supervisor : 1;      // 用户/内核位
  uint32_t page_write_through : 1;   // 写直达位
  uint32_t page_cache_disable : 1;   // 禁用缓存位
  uint32_t accessed : 1;             // 访问位
  uint32_t dirty : 1;                // 脏位
  uint32_t page_attribute_table : 1; // 页属性表位
  uint32_t global : 1;               // 全局页位
  uint32_t available : 3;            // 可用位（供操作系统使用）
  uint32_t page_frame_base : 20; // 页帧基地址（物理地址的高20位）
} __attribute__((packed));

#endif // KERNEL_MEMLAYOUT_H_
