# ap_trampoline.S - AP 16-bit trampoline
# Copied to physical address 0x8000 by BSP.
# AP starts here in 16-bit real mode after SIPI.
#
# Strategy: Set DS=0x0800 so all memory accesses use segment+offset = 0x0800+offset,
# which gives linear address 0x0800+offset = 0x8000+offset. This way we can use
# normal labels without needing runtime fixups.
#
# Layout within the page at 0x8000:
#   0x00-0x3F: 16-bit code (real mode → protected mode)
#   0x40-0x7F: 32-bit code (protected mode, enable PAE/CR3/EFER/PG)
#   0x80-0xBF: 64-bit code (load stack, cpu_id, jump to C entry)
#   0xC0-0xDF: data area (pml4, stack, entry, cpu_id)
#   0xE0-0xFF: GDT (null, code32, data, code64)
#   0x100-0x107: GDTR
#
# GDT selectors:
#   0x00: null
#   0x08: code32 (for real→protected far jump)
#   0x10: data
#   0x18: code64 (for protected→long mode far jump)
#
# Data field offsets (from base 0x8000, accessed as DS:offset with DS=0x0800):
#   0xC0: uint64_t pml4_phys
#   0xC8: uint64_t kernel_stack (virtual)
#   0xD0: uint64_t ap_entry_addr (virtual)
#   0xD8: uint32_t cpu_id

.section .rodata

.global ap_trampoline_start
.global ap_trampoline_end

.code16
ap_trampoline_start:
    cli
    cld

    # Set DS=0x0800 so DS:offset = 0x8000+offset
    movw $0x0800, %ax
    movw %ax, %ds
    xorw %ax, %ax
    movw %ax, %es
    movw %ax, %ss

    # Enable A20 via fast gate (port 0x92)
    inb $0x92, %al
    orb $0x02, %al
    andb $0xfe, %al
    outb %al, $0x92

    # Load GDT (GDTR at offset 0x100)
    lgdt 0x100

    # Enable protected mode
    movl %cr0, %eax
    orl $0x01, %eax
    movl %eax, %cr0

    # Far jump to 32-bit code at 0x8040 (selector 0x08 = code32)
    ljmpl $0x08, $0x8040

.code32
.org 0x40
trampoline_32bit:
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs
    movw %ax, %ss

    # Enable PAE
    movl %cr4, %eax
    orl $0x20, %eax
    movl %eax, %cr4

    # Load CR3 from pml4_phys at 0x80C0
    movl 0x80C0, %eax
    movl %eax, %cr3

    # Enable long mode (EFER.LME + EFER.NXE for NX bit)
    movl $0xC0000080, %ecx
    rdmsr
    orl $0x900, %eax         # bit 8 = LME, bit 11 = NXE
    wrmsr

    # Enable paging
    movl %cr0, %eax
    orl $0x80000000, %eax
    movl %eax, %cr0

    # Far jump to 64-bit code at 0x8080 (selector 0x18 = code64)
    ljmpl $0x18, $0x8080

.code64
.org 0x80
trampoline_64bit:
    # Load kernel stack from physical 0x80C8
    movq $0x80C8, %rax
    movq (%rax), %rsp

    # Load cpu_id from physical 0x80D8 into %edi
    movq $0x80D8, %rax
    movl (%rax), %edi

    # Load ap_entry_c address from physical 0x80D0 and jump
    movq $0x80D0, %rax
    movq (%rax), %rax
    jmp *%rax

1:  hlt
    jmp 1b

# ---- Data area (offset 0xC0) ----
.org 0xC0
    .quad 0    # 0xC0: pml4_phys (BSP fills)
    .quad 0    # 0xC8: kernel_stack (BSP fills)
    .quad 0    # 0xD0: ap_entry_addr (BSP fills)
    .long 0    # 0xD8: cpu_id (BSP fills)
    .long 0    # 0xDC: padding

# ---- GDT (offset 0xE0) ----
.org 0xE0
    .quad 0x0000000000000000       # 0xE0: null
    .quad 0x00CF9A000000FFFF       # 0xE8: code32 (base=0, limit=4GB, G=1, D=1, Code/Read)
    .quad 0x00CF92000000FFFF       # 0xF0: data  (base=0, limit=4GB, G=1, Data/Write)
    .quad 0x00209A0000000000       # 0xF8: code64 (base=0, L=1, Code/Read)

# ---- GDTR (offset 0x100) ----
.org 0x100
    .word 0x1F                     # limit = 31 (4 entries × 8 - 1)
    .long 0x80E0                   # base = 0x80E0

.org 0x108
ap_trampoline_end:

    .section .note.GNU-stack,"",@progbits