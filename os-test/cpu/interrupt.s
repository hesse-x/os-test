.code32
# Defined in isr.c
.extern isr_handler
.extern irq_handler

# Common ISR code
isr_common_stub:
  # 1. Save CPU state
  pusha                # Pushes edi,esi,ebp,esp,ebx,edx,ecx,eax
  movw %ds, %ax        # Lower 16-bits of eax = ds.
  pushl %eax           # save the data segment descriptor
  movw $0x10, %ax      # kernel data segment descriptor
  movw %ax, %ds
  movw %ax, %es
  movw %ax, %fs
  movw %ax, %gs

	push %esp # registers_t *r
  # 2. Call C handler
  cld # C code following the sysV ABI requires DF to be clear on function entry
  call isr_handler
  pop %ebx  # Different than the ISR code

  # 3. Restore state
  popl %eax 
  movw %ax, %ds
  movw %ax, %es
  movw %ax, %fs
  movw %ax, %gs
  popa
  addl $8, %esp        # Cleans up the pushed error code and pushed ISR number
  iret                 # pops 5 things at once: CS, EIP, EFLAGS, SS, and ESP

# Common IRQ code. Identical to ISR code except for the 'call' 
# and the 'pop ebx'
irq_common_stub:
  pusha 
  movw %ds, %ax
  pushl %eax
  movw $0x10, %ax
  movw %ax, %ds
  movw %ax, %es
  movw %ax, %fs
  movw %ax, %gs
  push %esp
  cld
  call irq_handler       # Different than the ISR code
  popl %ebx              # Different than the ISR code
  pop %ebx
  movw %bx, %ds
  movw %bx, %es
  movw %bx, %fs
  movw %bx, %gs
  popa
  addl $8, %esp
  iret 

# We don't get information about which interrupt was caller
# when the handler is run, so we will need to have a different handler
# for every interrupt.
# Furthermore, some interrupts push an error code onto the stack but others
# don't, so we will push a dummy error code for those which don't, so that
# we have a consistent stack for all of them.

# First make the ISRs global
.global isr0
.global isr1
.global isr2
.global isr3
.global isr4
.global isr5
.global isr6
.global isr7
.global isr8
.global isr9
.global isr10
.global isr11
.global isr12
.global isr13
.global isr14
.global isr15
.global isr16
.global isr17
.global isr18
.global isr19
.global isr20
.global isr21
.global isr22
.global isr23
.global isr24
.global isr25
.global isr26
.global isr27
.global isr28
.global isr29
.global isr30
.global isr31
# IRQs
.global irq0
.global irq1
.global irq2
.global irq3
.global irq4
.global irq5
.global irq6
.global irq7
.global irq8
.global irq9
.global irq10
.global irq11
.global irq12
.global irq13
.global irq14
.global irq15

# 0: Divide By Zero Exception
isr0:
  pushl $0
  pushl $0
  jmp isr_common_stub

# 1: Debug Exception
isr1:
  pushl $0
  pushl $1
  jmp isr_common_stub

# 2: Non Maskable Interrupt Exception
isr2:
  pushl $0
  pushl $2
  jmp isr_common_stub

# 3: Int 3 Exception
isr3:
  pushl $0
  pushl $3
  jmp isr_common_stub

# 4: INTO Exception
isr4:
  pushl $0
  pushl $4
  jmp isr_common_stub

# 5: Out of Bounds Exception
isr5:
  pushl $0
  pushl $5
  jmp isr_common_stub

# 6: Invalid Opcode Exception
isr6:
  pushl $0
  pushl $6
  jmp isr_common_stub

# 7: Coprocessor Not Available Exception
isr7:
  pushl $0
  pushl $7
  jmp isr_common_stub

# 8: Double Fault Exception (With Error Code!)
isr8:
  pushl $8
  jmp isr_common_stub

# 9: Coprocessor Segment Overrun Exception
isr9:
  pushl $0
  pushl $9
  jmp isr_common_stub

# 10: Bad TSS Exception (With Error Code!)
isr10:
  pushl $10
  jmp isr_common_stub

# 11: Segment Not Present Exception (With Error Code!)
isr11:
  pushl $11
  jmp isr_common_stub

# 12: Stack Fault Exception (With Error Code!)
isr12:
  pushl $12
  jmp isr_common_stub

# 13: General Protection Fault Exception (With Error Code!)
isr13:
  pushl $13
  jmp isr_common_stub

# 14: Page Fault Exception (With Error Code!)
isr14:
  pushl $14
  jmp isr_common_stub

# 15: Reserved Exception
isr15:
  pushl $0
  pushl $15
  jmp isr_common_stub

# 16: Floating Point Exception
isr16:
  pushl $0
  pushl $16
  jmp isr_common_stub

# 17: Alignment Check Exception
isr17:
  pushl $0
  pushl $17
  jmp isr_common_stub

# 18: Machine Check Exception
isr18:
  pushl $0
  pushl $18
  jmp isr_common_stub

# 19: Reserved
isr19:
  pushl $0
  pushl $19
  jmp isr_common_stub

# 20: Reserved
isr20:
  pushl $0
  pushl $20
  jmp isr_common_stub

# 21: Reserved
isr21:
  pushl $0
  pushl $21
  jmp isr_common_stub

# 22: Reserved
isr22:
  pushl $0
  pushl $22
  jmp isr_common_stub

# 23: Reserved
isr23:
  pushl $0
  pushl $23
  jmp isr_common_stub

# 24: Reserved
isr24:
  pushl $0
  pushl $24
  jmp isr_common_stub

# 25: Reserved
isr25:
  pushl $0
  pushl $25
  jmp isr_common_stub

# 26: Reserved
isr26:
  pushl $0
  pushl $26
  jmp isr_common_stub

# 27: Reserved
isr27:
  pushl $0
  pushl $27
  jmp isr_common_stub

# 28: Reserved
isr28:
  pushl $0
  pushl $28
  jmp isr_common_stub

# 29: Reserved
isr29:
  pushl $0
  pushl $29
  jmp isr_common_stub

# 30: Reserved
isr30:
  pushl $0
  pushl $30
  jmp isr_common_stub

# 31: Reserved
isr31:
  pushl $0
  pushl $31
  jmp isr_common_stub

# IRQ handlers
irq0:
  pushl $0
  pushl $32
  jmp irq_common_stub

irq1:
  pushl $1
  pushl $33
  jmp irq_common_stub

irq2:
  pushl $2
  pushl $34
  jmp irq_common_stub

irq3:
  pushl $3
  pushl $35
  jmp irq_common_stub

irq4:
  pushl $4
  pushl $36
  jmp irq_common_stub

irq5:
  pushl $5
  pushl $37
  jmp irq_common_stub

irq6:
  pushl $6
  pushl $38
  jmp irq_common_stub

irq7:
  pushl $7
  pushl $39
  jmp irq_common_stub

irq8:
  pushl $8
  pushl $40
  jmp irq_common_stub

irq9:
  pushl $9
  pushl $41
  jmp irq_common_stub

irq10:
  pushl $10
  pushl $42
  jmp irq_common_stub

irq11:
  pushl $11
  pushl $43
  jmp irq_common_stub

irq12:
  pushl $12
  pushl $44
  jmp irq_common_stub

irq13:
  pushl $13
  pushl $45
  jmp irq_common_stub

irq14:
  pushl $14
  pushl $46
  jmp irq_common_stub

irq15:
  pushl $15
  pushl $47
  jmp irq_common_stub
.section .note.GNU-stack,"",%progbits
