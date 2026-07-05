/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <unistd.h>

int main(int argc, char **argv, char **envp) {
  (void)argc;
  (void)argv;
  (void)envp;
  double a = 3.14, b = 2.71, c;
  c = a * b;
  printf("fpu test: %.2f * %.2f = %.4f\n", a, b, c);
  if (c > 8.5 && c < 8.6) {
    printf("fpu test: PASS\n");
    return 0;
  }
  printf("fpu test: FAIL\n");
  return 1;
}
