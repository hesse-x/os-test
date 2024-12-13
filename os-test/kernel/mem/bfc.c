#include "stdbool.h"

#include "os-test/kernel/mem/bfc.h"
#include "os-test/kernel/mem/memlayout.h"
#include "os-test/utils/os_utils.h"

static inline bool is_contiguous(struct block *cur, struct block *next) {
  char *tc = (char *)get_ptr(cur);
  char *tn = (char *)next;
  return (tn - tc) == cur->size;
}

void *bfc_alloc(size_t size) {
  size = ALIGN(size, PAGE_SIZE);
  struct block *current = free_list;
  struct block *best_fit = NULL;

  // 遍历空闲链表，找到最佳适应的块
  while (current != NULL) {
    if (current->is_free && current->size >= size) {
      if (best_fit == NULL || current->size < best_fit->size) {
        best_fit = current;
      }
    }
    current = current->next;
  }

  if (best_fit == NULL) {
    // 没有找到合适的块
    return NULL;
  }

  // 如果找到的块大小正好等于所需大小，直接分配
  if (best_fit->size == size) {
    best_fit->is_free = 0;
    return (void *)(best_fit->addr);
  }

  // 否则，分割块
  struct block *new_block = (struct block *)((char *)best_fit + sizeof(struct block) + size);
  new_block->size = best_fit->size - size - sizeof(struct block);
  new_block->is_free = 1;
  new_block->next = best_fit->next;

  best_fit->size = size;
  best_fit->is_free = 0;
  best_fit->next = new_block;

  return get_ptr(best_fit);
}

void bfc_free(void *ptr) {
  if (ptr == NULL) {
    return;
  }

  struct block *free_block = (struct block *)ptr - 1;
  free_block->is_free = 1;

  // 合并相邻的空闲块
  struct block *current = free_list;
  while (current != NULL) {
    if (current->is_free && current->next != NULL && current->next->is_free && is_contiguous(current, current->next)) {
      current->size += sizeof(struct block) + current->next->size;
      current->next = current->next->next;
    }
    current = current->next;
  }
}
