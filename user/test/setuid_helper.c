/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

/* setuid_helper.c — execve S_ISUID/S_ISGID 验证用的辅助 ELF。
 *
 * 本 OS 的测试框架(test_runner)以 root fork+execve 跑每个 /test/X.elf。要验证
 * "execve 一个 setuid 位文件后子进程 euid 切到 inode owner"，需要一个独立的
 * helper ELF：主测试 chmod 04755 + chown 给目标 uid，fork 子 setuid(2000) drop
 * root，子 execve 本 helper；helper 用退出码把 geteuid/getegid/getuid 报回父
 * 进程断言。
 *
 * 单 helper 服务多用例：argv[1] 选模式
 *   "euid"  → _exit(geteuid() & 0x7f)
 *   "egid"  → _exit(getegid() & 0x7f)
 *   "uid"   → _exit(getuid() & 0x7f)   (real uid 应保持调用者)
 *   "gid"   → _exit(getgid() & 0x7f)
 * 退出码掩 0x7f 使 WEXITSTATUS 无歧义（uid/gid 测试值均 < 128）。
 *
 * 必须 freestanding 友好：不依赖 argc/argv 之外的环境，失败 _exit(127)。 */
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
