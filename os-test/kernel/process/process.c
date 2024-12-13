#include "os-test/kernel/process/process.h"
void save_context(struct context *ctx) {
  // 保存当前上下文
  asm volatile("mov %%eax, %0\n"
               "mov %%ebx, %1\n"
               "mov %%ecx, %2\n"
               "mov %%edx, %3\n"
               "mov %%esi, %4\n"
               "mov %%edi, %5\n"
               "mov %%ebp, %6\n"
               "mov %%esp, %7\n"
               "mov (%%esp), %8\n"
               : "=m"(ctx->eax), "=m"(ctx->ebx), "=m"(ctx->ecx), "=m"(ctx->edx),
                 "=m"(ctx->esi), "=m"(ctx->edi), "=m"(ctx->ebp), "=m"(ctx->esp),
                 "=m"(ctx->eip)
               :
               : "memory");
}

void restore_context(struct context *ctx) {
  // 恢复上下文
  asm volatile("mov %0, %%eax\n"
               "mov %1, %%ebx\n"
               "mov %2, %%ecx\n"
               "mov %3, %%edx\n"
               "mov %4, %%esi\n"
               "mov %5, %%edi\n"
               "mov %6, %%ebp\n"
               "mov %7, %%esp\n"
               "push %8\n"
               "ret\n"
               :
               : "m"(ctx->eax), "m"(ctx->ebx), "m"(ctx->ecx), "m"(ctx->edx),
                 "m"(ctx->esi), "m"(ctx->edi), "m"(ctx->ebp), "m"(ctx->esp),
                 "m"(ctx->eip)
               : "memory");
}
