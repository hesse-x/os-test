#include "kernel.h"
#include "multiboot2.h"
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

extern "C" {
static const uint8_t stack_bottom[8192] STACK_ATTR = {0};

void _start() __attribute__((externally_visible, noreturn, naked));
void _start() {
  uint32_t magic_num;
  uintptr_t addr;
  __asm__ volatile("movl %%eax, %0\n"
                   "movl %%ebx, %1\n"
                   : "=r"(magic_num), "=r"(addr)
                   :
                   :);

  __asm__ volatile(
      // 1. 初始化栈指针（ESP指向栈顶，x86栈向下生长）
      "movl %0, %%esp\n"
      "movl 0xffffff, %%esp\n"

      // 2. 保存Multiboot2传递的参数（压栈供kernel_main使用）
      "movl %2, %%eax\n"
      "movl %3, %%ebx\n"
      "pushl %%ebx\n" // %ebx = Multiboot2信息结构地址
      "pushl %%eax\n" // %eax = Multiboot2魔数（0x36d76289）

      // 3. 调用C语言内核主函数
      "call *%1\n"

      // 4. 内核返回后的空闲循环（永不退出）
      "cli\n"    // 禁用所有可屏蔽中断
      "1: hlt\n" // 暂停CPU（低功耗）
      "jmp 1b\n" // 无限跳回hlt，形成空闲循环

      : // 输出操作数：无
      : "r"(stack_bottom + sizeof(stack_bottom)),
        "r"((uintptr_t)kernel_main & 0xffffff), "r"(magic_num),
        "r"(addr) // 输入操作数
      : "memory", "eax", "ebx" // 破坏描述：告知编译器修改了这些寄存器/内存
  );
}
} // extern C
