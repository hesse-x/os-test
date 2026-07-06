/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>
#include <xos/syscall_nums.h>

// usable at bootstrap stage: pure syscall, does not depend on GOT/printf
// fd=2 is stderr; fd table retains stdin/stdout/stderr after execve
// ld.md §3.2.4
// hidden: visible across files but not exported to dynamic symbol table, to
// avoid going through PLT (GOT not filled before bootstrap)

__attribute__((visibility("hidden"))) void dl_puts(const char *s) {
  size_t len = 0;
  while (s[len])
    len++;
  // direct syscall, no PLT
  long ret;
  __asm__ volatile("syscall"
                   : "=a"(ret)
                   : "a"(SYS_WRITE), "D"(2), "S"(s), "d"(len)
                   : "rcx", "r11", "memory");
}

// dl_put_hex: prints register values/addresses, usable early in bootstrap
// hexdigits uses a local array (on stack), does not depend on .rodata
// relocation
__attribute__((visibility("hidden"))) void dl_put_hex(uint64_t val) {
  char buf[17];
  const char hexdigits[] = "0123456789abcdef"; // local array, on stack
  for (int i = 15; i >= 0; i--) {
    buf[i] = hexdigits[val & 0xf];
    val >>= 4;
  }
  buf[16] = '\n';
  long ret;
  __asm__ volatile("syscall"
                   : "=a"(ret)
                   : "a"(SYS_WRITE), "D"(2), "S"(buf), "d"(17)
                   : "rcx", "r11", "memory");
}
