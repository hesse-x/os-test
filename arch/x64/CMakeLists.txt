# Copyright (c) 2026 hesse
#
# SPDX-License-Identifier: MIT

add_kernel_object(arch_x64
    SOURCES paging.c trap.c smp.c apic.c rdrand.c
    ASM_SOURCES start.S vectors.S trapentry.S ap_trampoline.S
)
