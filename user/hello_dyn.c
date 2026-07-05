#include <errno.h>
#include <stdio.h>
#include <time.h>

int main(int argc, char **argv, char **envp) {
  (void)argc;
  (void)argv;
  (void)envp;
  struct timespec start, end;
  timespec_get(&start, TIME_UTC);
  printf("Hello, Dynamic World!");
  timespec_get(&end, TIME_UTC);
  long sec = end.tv_sec - start.tv_sec;
  long nsec = end.tv_nsec - start.tv_nsec;
  if (nsec < 0) {
    sec--;
    nsec += 1000000000L;
  }
  printf(" [printf %ld.%09ld s]\n", sec, nsec);
  // 引用 errno 验证 TCB 模式在动态链接下写读一致（ld.so §3.4.5）。
  // errno 是宏 → __errno_location() 返回 &TCB.errno_val，主 ELF 与 libc.so
  // 共享当前线程的同一份 errno。若 errno 仍是全局变量，动态链接下会写读分离
  // （bug.md BUG-LD-002）。
  printf("errno=%d\n", errno);
  return 0;
}
