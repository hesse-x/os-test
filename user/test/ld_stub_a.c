// ld.so 多依赖单元测试 stub：liba.so
// 导出 lda_answer()，内部调 libc strcmp（触发 JUMP_SLOT 跨模块解析）
#include <stddef.h>

int strcmp(const char *a, const char *b); // libc.so 符号

int lda_answer(void) {
  // 调 libc 符号，验证 JUMP_SLOT 解析到 libc.so
  if (strcmp("ld", "ld") != 0)
    return -1;
  return 41; // 线性链场景主 ELF 期望 41
}
