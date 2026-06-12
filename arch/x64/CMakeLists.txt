add_kernel_object(arch_x64
    SOURCES paging.cc trap.cc smp.cc apic.cc
    ASM_SOURCES start.S vectors.S trapentry.S ap_trampoline.S
)
