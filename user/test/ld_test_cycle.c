/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

// 场景 4：闭环检测（a↔b 互 NEEDED）
// 期望：find_loaded 去重静默打破环，进程正常 exit，无 dl: FATAL
int lda_answer(void);
int main(int argc, char **argv, char **envp) {
  (void)argc;
  (void)argv;
  (void)envp;
  if (lda_answer() != 41)
    return 1; // 环打破后 a 仍可用
  return 0;
}
