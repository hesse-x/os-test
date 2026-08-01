/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

// setuid_helper.c — auxiliary ELF for execve S_ISUID/S_ISGID verification.
//
// test_runner fork+execve's each /test/X.elf as root. To verify "execve of a
// setuid-bit file sets the child euid to the inode owner", a separate helper
// ELF is needed: the main test chmod 04755 + chown to the target uid, forks a
// child that setuid(2000) drops root, then execve's this helper; the helper
// reports geteuid/getegid/getuid back to the parent via its exit code.
//
// One helper serves multiple cases; argv[1] selects the mode:
//   "euid"  → _exit(geteuid() & 0x7f)
//   "egid"  → _exit(getegid() & 0x7f)
//   "uid"   → _exit(getuid() & 0x7f)   (real uid stays the caller's)
//   "gid"   → _exit(getgid() & 0x7f)
// Exit code masked with 0x7f so WEXITSTATUS is unambiguous (test uid/gid <
// 128).
//
// Must be freestanding-friendly: depend only on argc/argv, _exit(127) on
// failure.
#include <stddef.h>
#include <unistd.h>

int main(int argc, char **argv) {
  if (argc < 2)
    _exit(127);
  const char *mode = argv[1];
  if (mode[0] == 'e' && mode[1] == 'u' && mode[2] == 'i' && mode[3] == 'd' &&
      mode[4] == '\0')
    _exit((int)(geteuid() & 0x7f));
  if (mode[0] == 'e' && mode[1] == 'g' && mode[2] == 'i' && mode[3] == 'd' &&
      mode[4] == '\0')
    _exit((int)(getegid() & 0x7f));
  if (mode[0] == 'u' && mode[1] == 'i' && mode[2] == 'd' && mode[3] == '\0')
    _exit((int)(getuid() & 0x7f));
  if (mode[0] == 'g' && mode[1] == 'i' && mode[2] == 'd' && mode[3] == '\0')
    _exit((int)(getgid() & 0x7f));
  _exit(127);
}
