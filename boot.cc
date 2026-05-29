#include "kernel.h"
#include "multiboot2.h"
#include "mem.h"
#include <stdint.h>

#define BOOT_HEADER_ATTR __attribute__((section(".multiboot"), aligned(8)))
#define STACK_ATTR __attribute__((section(".stack"), aligned(16)))

constexpr size_t header_length = sizeof(multiboot_header) +
                                 sizeof(multiboot_header_tag_framebuffer) +
                                 sizeof(multiboot_header_tag);
constexpr size_t checksum = -(MULTIBOOT2_HEADER_MAGIC + 0 + header_length);

static const multiboot_header header BOOT_HEADER_ATTR = {
    .magic = MULTIBOOT2_HEADER_MAGIC,
    .architecture = 0,
    .header_length = header_length,
    .checksum = checksum};

static const multiboot_header_tag_framebuffer frame_tag BOOT_HEADER_ATTR = {
    .type = MULTIBOOT2_H_TAG_FRAMEBUFFER,
    .flags = 0,
    .size = sizeof(multiboot_header_tag_framebuffer),
    .width = 0,
    .height = 0,
    .depth = 0,
};

static const multiboot_header_tag end_tag BOOT_HEADER_ATTR = {
    .type = MULTIBOOT2_H_TAG_END,
    .flags = 0,
    .size = sizeof(multiboot_header_tag)};

// 页目录和页表，放在.bss段，4KB对齐
__attribute__((aligned(4096))) static uint32_t page_directory[1024];
__attribute__((aligned(4096))) static uint32_t page_table[1024];

extern "C" {
// 引导栈，C linkage + extern外部链接供start.S引用（C++ const默认内部链接）
extern const uint8_t stack_bottom[8192] STACK_ATTR = {0};

// boot_main: 在物理地址运行，设置分页后切换到高地址
void boot_main(int32_t magic_num, uintptr_t addr) {
  // GOTOFF自动给出物理地址（因boot_main在物理地址运行）

  // 清零PD和PT
  for (int i = 0; i < 1024; i++) {
    page_directory[i] = 0;
    page_table[i] = 0;
  }

  // 填充PT：物理 0x0-0x3FFFFF → 4KB页，present + writable
  for (int i = 0; i < 1024; i++) {
    page_table[i] = (i * 4096) | 0x03;
  }

  // PD[0] = PT物理地址 | flags（identity map: virt 0-4MB → phys 0-4MB）
  page_directory[0] = ((uintptr_t)page_table) | 0x03;

  // PD[768] = PT物理地址 | flags（higher-half: virt 0xC0000000-0xC0400000 → phys 0-4MB）
  page_directory[768] = ((uintptr_t)page_table) | 0x03;

  // 启用分页并切换到高地址
  __asm__ volatile(
      "movl %0, %%cr3\n"        // CR3 ← PD phys addr
      "movl %%cr0, %%eax\n"
      "orl $0x80000000, %%eax\n" // enable PG bit
      "movl %%eax, %%cr0\n"
      "movl %1, %%esp\n"        // ESP ← stack virt addr
      "jmp *%2\n"               // EIP ← kernel_main VMA
      :
      : "r"((uintptr_t)page_directory), // GOTOFF → phys
        "r"((uintptr_t)stack_bottom + 8192 +
            VMA_BASE),                  // GOTOFF → phys, + VMA_BASE → virt
        "r"((uintptr_t)kernel_main) // R_386_32 → VMA（外部符号不走GOTOFF）
      : "eax", "memory");
}
} // extern C
