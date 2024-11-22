.code16
probe_memory:
    push %ds
    push %es
    mov $0x9000, %eax
    mov %eax, %ds
    mov %eax, %es 
    mov $0, %eax
    movl $0, (%eax)
    xorl %ebx, %ebx
    movw $0x4, %di 
start_probe:
    movl $0xE820, %eax
    movl $20, %ecx
    movl $0x534d4150, %edx
    int $0x15
    jnc cont
    mov $0, %eax
    movw $12345, (%eax)
    jmp finish_probe
cont:
    addw $20, %di 
    incl 0x0
    cmpl $0, %ebx
    jnz start_probe
finish_probe:
    pop %ds
    pop %es
    ret
