#ifndef KERNEL_PROCESS_PROCESS_H_
#define KERNEL_PROCESS_PROCESS_H_
struct context {
  uint32_t eip;
  uint32_t esp;
  uint32_t ebx;
  uint32_t ecx;
  uint32_t edx;
  uint32_t esi;
  uint32_t edi;
  uint32_t ebp;
};

struct proc_struct {
  int pid;
  struct context ctx;
};
void save_context(struct context *ctx);
void restore_context(struct context *ctx);
#endif // KERNEL_PROCESS_PROCESS_H_
