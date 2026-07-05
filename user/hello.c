/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <time.h>

int main(int argc, char **argv, char **envp) {
  (void)argc;
  (void)argv;
  (void)envp;
  struct timespec start, end;
  timespec_get(&start, TIME_UTC);
  printf("Hello, World!");
  timespec_get(&end, TIME_UTC);
  long sec = end.tv_sec - start.tv_sec;
  long nsec = end.tv_nsec - start.tv_nsec;
  if (nsec < 0) {
    sec--;
    nsec += 1000000000L;
  }
  printf(" [printf %ld.%09ld s]\n", sec, nsec);
  return 0;
}