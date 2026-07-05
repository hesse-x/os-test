// .init_array / .fini_array 遍历
// ld.md §3.5.2, §6.4 任务 4 / plan_ld2b3 T10
//
// 动态链接下 __init_array_start/end 是 hidden 符号（不进 .dynsym），
// libc.so 无法跨对象引用。改为由 crt0 取主 ELF 符号地址，通过
// __libc_start_main 参数传入范围。

typedef void (*init_func_t)(void);

// 跑 .init_array（C++ 全局构造器等）
void __libc_run_init_array(init_func_t *start, init_func_t *end) {
  for (init_func_t *f = start; f < end; f++) {
    if (*f)
      (*f)();
  }
}

// 跑 .fini_array（exit 时逆序）
void __libc_run_fini_array(init_func_t *start, init_func_t *end) {
  for (init_func_t *f = end - 1; f >= start; f--) {
    if (*f)
      (*f)();
  }
}
