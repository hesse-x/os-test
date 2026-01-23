#ifndef MEMORY_H
#define MEMORY_H

#include "multiboot2.h"
#include "macro.h"
#include <stddef.h>
#include <stdint.h>

// ===================== 常量定义 =====================
#define PAGE_SIZE 4096 // 4KB页帧大小
#define KERNEL_LMA_BASE 0x100000
#define KERNEL_VMA_BASE 0xC0100000
#define PHY_ADDR(addr) ((uintptr_t)addr & 0xffffff)
#define PTX_SHIFT 12
#define PHY_TO_PAGE(addr) (addr >> PTX_SHIFT)
#define GET_PAGE_NUM(len) (ALIGN_UP(len, PAGE_SIZE) / PAGE_SIZE)

// ===================== 页帧描述符 =====================
// 页帧状态
enum class PageStatus : int8_t {
  FREE,    // 空闲
  USED,    // 已使用（如内核占用）
  RESERVED // 保留（硬件/BIOS占用）
};

// 单个页帧的描述符（记录状态）
struct Page {
  PageStatus status;
  size_t cont_page_num;
  Page *prev;
  Page *next;
};

struct BFCAllocator {
  void init();
  Page *alloc_page(size_t n);
  Page *free_page(Page *page, size_t n);
  size_t free_page_nums() const;

  static Page *frames;
  static Page *free_list;
};

// ===================== 全局变量声明 =====================
// 页帧描述符数组（存储所有页帧的状态）
// 系统总页帧数
extern size_t total_page_frames;

// ===================== 函数声明 =====================
// 解析Multiboot2内存映射信息
void init_memory(multiboot_tag_mmap *mmap);

// void print_memory_info();
#endif // MEMORY_H
