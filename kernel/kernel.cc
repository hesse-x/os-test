// Higher-half内核，-fPIE编译
// kernel_main: 虚拟地址运行，init_mem + 串口 + framebuffer 输出
#include <stddef.h>
#include <stdint.h>

#include "common/macro.h"
#include "kernel/kernel.h"
#include "kernel/mem/alloc.h"
#include "kernel/serial.h"
#include "driver/fb.h"
#include "kernel/trap.h"
#include "driver/kbd.h"
#include "arch/x86/multiboot2.h"
#include "arch/x86/paging.h"

static void kbd_echo(char c) {
  fb_putc(c, 0xFFFFFF);
}

// [TEST] 验证 ring 0 → ring 3 切换能力，后续删除
static void test_ring3() {
  // 在内核栈上手动构造 iret 帧
  // iret 依次 pop: EIP, CS, EFLAGS, ESP, SS
  // 栈帧从低地址到高地址: EIP → SS
  __asm__ volatile(
    "movl $0x1000, -20(%esp)\n\t"      /* EIP: not-present 地址 */
    "movl $0x1B, -16(%esp)\n\t"        /* CS: user code, RPL=3 */
    "movl $0x202, -12(%esp)\n\t"       /* EFLAGS: IF=1 */
    "movl $0xBFFFFFFC, -8(%esp)\n\t"   /* ESP: 随机用户栈指针 */
    "movl $0x23, -4(%esp)\n\t"         /* SS: user data, RPL=3 */
    "subl $20, %esp\n\t"
    "iret\n\t"
  );
  // 不应到达此处——iret 进入 ring 3 后触发 #PF
}

extern "C" {

void kernel_init_finish() {
  // 清除 identity map（PD[0] 设为 not present）
  page_directory[0] = 0;
  flush_tlb();

  // 禁止 bump 分配器
  bump_disable();
}

void kernel_main(int32_t magic_num, uintptr_t addr) {
  init_mem(addr);

  serial_init();
  isr_init();
  kernel_init_finish();

  if (magic_num == MULTIBOOT2_BOOTLOADER_MAGIC) {
    serial_puts("OK\n");
  } else {
    serial_puts("FAIL\n");
  }

  // [TEST] ring 3 验证
  test_ring3();
  // 不会到达此处
}
}