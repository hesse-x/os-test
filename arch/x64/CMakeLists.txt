add_kernel_object(arch_x64
    SOURCES paging.c trap.c smp.c apic.c
    ASM_SOURCES start.S vectors.S trapentry.S ap_trampoline.S
)
