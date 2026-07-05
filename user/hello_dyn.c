/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

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
  // Reference errno to verify the TCB mode reads/writes consistently under
  // dynamic linking (ld.so §3.4.5). errno is a macro -> __errno_location()
  // returns &TCB.errno_val; the main ELF and libc.so share the same errno
  // of the current thread. If errno were still a global variable, dynamic
  // linking would split writes from reads (bug.md BUG-LD-002).
  printf("errno=%d\n", errno);
  return 0;
}
