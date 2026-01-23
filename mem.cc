#include "mem.h"
#include "common.h"

// ===================== 全局变量定义 =====================
// BFCAllocator 静态成员初始化
Page *BFCAllocator::frames = NULL;
Page *BFCAllocator::free_list = NULL;

// ===================== BFCAllocator 实现 =====================
void BFCAllocator::init() {
  // 等待调用 init_memory 来初始化
}

Page *BFCAllocator::alloc_page(size_t n) {
  if (n == 0 || free_list == NULL) {
    return NULL;
  }

  // 遍历 free_list 寻找足够大的连续空闲块
  Page *cur = free_list;
  Page *prev = NULL;

  while (cur != NULL) {
    if (cur->cont_page_num >= n) {
      // 找到合适的块
      if (cur->cont_page_num == n) {
        // 完全匹配，从链表中移除
        if (prev == NULL) {
          free_list = cur->next;
        } else {
          prev->next = cur->next;
        }
        if (cur->next != NULL) {
          cur->next->prev = prev;
        }
        cur->status = PageStatus::USED;
        return cur;
      } else {
        // 块更大，分割它
        size_t remaining = cur->cont_page_num - n;
        cur->cont_page_num = n;
        cur->status = PageStatus::USED;

        // 创建新的空闲块
        Page *new_block = cur + n;
        new_block->status = PageStatus::FREE;
        new_block->cont_page_num = remaining;
        new_block->prev = prev;
        new_block->next = cur->next;

        if (prev == NULL) {
          free_list = new_block;
        } else {
          prev->next = new_block;
        }
        if (cur->next != NULL) {
          cur->next->prev = new_block;
        }

        cur->prev = NULL;
        cur->next = NULL;
        return cur;
      }
    }
    prev = cur;
    cur = cur->next;
  }

  return NULL; // 未找到足够大的块
}

Page *BFCAllocator::free_page(Page *page, size_t n) {
  if (page == NULL || n == 0) {
    return NULL;
  }

  // 设置页面为空闲
  page->status = PageStatus::FREE;
  page->cont_page_num = n;

  // 如果 free_list 为空，直接插入
  if (free_list == NULL) {
    page->prev = NULL;
    page->next = NULL;
    free_list = page;
    return page;
  }

  // 在 free_list 中寻找合适的位置插入（按地址排序）
  Page *cur = free_list;
  Page *prev = NULL;

  while (cur != NULL && cur < page) {
    prev = cur;
    cur = cur->next;
  }

  // 插入到 prev 和 cur 之间
  page->prev = prev;
  page->next = cur;

  if (prev == NULL) {
    free_list = page;
  } else {
    prev->next = page;
  }

  if (cur != NULL) {
    cur->prev = page;
  }

  // 尝试与前一个块合并
  if (prev != NULL && prev + prev->cont_page_num == page) {
    prev->cont_page_num += page->cont_page_num;
    prev->next = cur;
    if (cur != NULL) {
      cur->prev = prev;
    }
    page = prev; // 更新为合并后的块
  }

  // 尝试与后一个块合并
  if (cur != NULL && page + page->cont_page_num == cur) {
    page->cont_page_num += cur->cont_page_num;
    page->next = cur->next;
    if (cur->next != NULL) {
      cur->next->prev = page;
    }
  }

  return page;
}

size_t BFCAllocator::free_page_nums() const {
  size_t total = 0;
  Page *cur = free_list;

  while (cur != NULL) {
    total += cur->cont_page_num;
    cur = cur->next;
  }

  return total;
}

// ===================== Multiboot2内存映射解析 =====================
size_t get_total_page_num(multiboot_tag_mmap *mmap) {
  uint64_t max_phys_addr = 0;
  const multiboot_mmap_entry *entry = mmap->entries;
  for (int32_t i = 0; i < mmap->entry_size; i++) {
    uint64_t entry_addr = entry[i].addr;
    uint64_t entry_len = entry[i].len;
    uint32_t entry_end = entry_addr + entry_len;

    if (entry_end > max_phys_addr) {
      max_phys_addr = entry_end;
    }
  }
  return GET_PAGE_NUM(max_phys_addr);
}

void init_frames(multiboot_tag_mmap *mmap) {
  size_t total_page_num = get_total_page_num(mmap);
  uintptr_t real_kernel_end = PHY_ADDR(kernel_end) + total_page_num * sizeof(Page);

  Page *frames = (Page *)PHY_ADDR(kernel_end);
  for (int64_t i = 0; i < total_page_num; i++) {
    frames[i].status = PageStatus::RESERVED;
    frames[i].cont_page_num = 1;
    frames[i].prev = NULL;
    frames[i].next = NULL;
  }

  const multiboot_mmap_entry *entry = mmap->entries;
  for (int32_t i = 0; i < mmap->entry_size; i++) {
    uint64_t entry_addr = entry[i].addr;
    uint64_t entry_len = entry[i].len;

    int64_t page_num = GET_PAGE_NUM(entry_len);
    int64_t page_idx = PHY_TO_PAGE(ALIGN_UP(entry_addr, PAGE_SIZE));
    Page *cur_frames = frames + page_idx;
    for (int64_t i = 0; i < page_num; i++) {
      cur_frames[i].status = PageStatus::FREE;
    }
  } 

  int64_t kernel_page_idx = PHY_TO_PAGE(KERNEL_LMA_BASE);
  int64_t kernel_size = ((uintptr_t)kernel_end) - KERNEL_VMA_BASE + total_page_num * sizeof(Page);
  int64_t kernel_page_num = GET_PAGE_NUM(kernel_size);
  Page *kernel_page_start = frames + kernel_page_idx;
  for (int64_t i = 0; i < kernel_page_num; i++) {
    kernel_page_start[i].status = PageStatus::RESERVED;
  }

  // 0: find first free
  // 1: find contiguous page
  int state = 0;
  Page *prev = NULL;
  Page **cur_page = &BFCAllocator::free_list;
  size_t cont_page_num = 0;
  for (int64_t i = 0; i < total_page_num; i++) {
    PageStatus status = BFCAllocator::frames[i].status;
    switch (state) {
      case 0: {
       if (status == PageStatus::FREE) {
         state = 1;
         *cur_page = BFCAllocator::frames + i;
         (*cur_page)->prev = prev;
         prev = *cur_page;
         cont_page_num += 1;
       }
       break;
      }
      case 1: {
       if (status != PageStatus::FREE) {
         state = 0;
         (*cur_page)->cont_page_num = cont_page_num;
         cont_page_num = 0;
         cur_page = &((*cur_page)->next);
       } else {
         cont_page_num += 1;
       }
       break;
      }
    }
  }
}

void enable_page() {
}

// ===================== 打印内存信息 =====================
// void print_memory_info() {
//     serial_puts("\n=== Memory Summary ===\n");
//     serial_puts("Max physical address: ");
//     serial_puthex(max_phys_addr);
//     serial_puts("Kernel physical end: ");
//     serial_puthex(kernel_phys_end);
//     serial_puts("Total page frames: ");
//     serial_puthex(total_page_num);
// 
//     // 统计各类页帧数量
//     uint32_t free = 0, used = 0, reserved = 0;
//     for (uint32_t i = 0; i < total_page_num; i++) {
//         switch (BFCAllocator::frames[i].status) {
//             case PAGE_FRAME_FREE: free++; break;
//             case PAGE_FRAME_USED: used++; break;
//             case PAGE_FRAME_RESERVED: reserved++; break;
//         }
//     }
// 
//     serial_puts("Free page frames: ");
//     serial_puthex(free);
//     serial_puts("Used page frames: ");
//     serial_puthex(used);
//     serial_puts("Reserved page frames: ");
//     serial_puthex(reserved);
// }
