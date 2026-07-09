/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef KERNEL_XCORE_MEM_EXTABLE_H
#define KERNEL_XCORE_MEM_EXTABLE_H

#include <stdint.h>

// Forward declaration of the arch trapframe (defined in arch/x64/trap.h).
struct trapframe;

// Exception-table entry: a (fault_rip, fixup_rip) pair. Emitted into the
// __ex_table section by the _ASM_EXTABLE macro below. Searched linearly by
// fixup_exception(): if the faulting instruction at fault_rip matches an
// entry, the handler rewrites tf->rip to fixup_rip and the CPU resumes there
// (the fixup label sets %rax = -EFAULT and returns), instead of panicking.
struct exception_table_entry {
  uint64_t insn;  // faulting instruction RIP
  uint64_t fixup; // fixup label RIP to resume at
};

// _ASM_EXTABLE(insn_label, fixup_label)
// Emit one (insn, fixup) pair into __ex_table. Used inside an inline-asm block
// to mark a single load/store instruction (at local label insn_label) as
// recoverable: on a fault, control transfers to fixup_label.
//
// .pushsection/.previous isolate the entry into __ex_table so it does not
// perturb the surrounding .text stream. 1b/0b are backrefs to the preceding
// local labels '1:' (faulting insn) and '0:' (fixup) within the same asm.
#define _ASM_EXTABLE(insn_label, fixup_label)                                  \
  " .pushsection __ex_table, \"a\"\n"                                          \
  " .quad " insn_label "\n"                                                    \
  " .quad " fixup_label "\n"                                                   \
  " .popsection\n"

// Search the exception table for an entry whose .insn == fault_rip.
// Returns the matching .fixup RIP, or 0 if no match (caller must panic).
// fault_rip is tf->rip for a kernel-mode #PF/#GP.
uint64_t fixup_exception(struct trapframe *tf);

#endif /* KERNEL_XCORE_MEM_EXTABLE_H */
