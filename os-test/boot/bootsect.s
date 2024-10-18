.code16
.global _start
_start:
  .equ KERNEL_OFFSET, 0x1000 # The same one we used when linking the kernel

  movb %dl, BOOT_DRIVE # Remember that the BIOS sets us the boot drive in 'dl' on boot
  movw $0x9000, %bp
  movw %bp, %sp

  movw $MSG_REAL_MODE, %bx
  call print
  call print_nl

  call load_kernel # read the kernel from disk
  call switch_to_pm # disable interrupts, load GDT,  etc. Finally jumps to 'BEGIN_PM'
  jmp . # Never executed

.include "os-test/boot/print.s"
.include "os-test/boot/print_hex.s"
.include "os-test/boot/disk.s"
.include "os-test/boot/gdt.s"
.include "os-test/boot/32bit_print.s"
.include "os-test/boot/switch_pm.s"

.code16
load_kernel:
  movw $MSG_LOAD_KERNEL, %bx
  call print
  call print_nl

  movw $KERNEL_OFFSET, %bx # Read from disk and store in 0x1000
  movb $31, %dh # Our future kernel will be larger, make this big
  movb BOOT_DRIVE, %dl
  call disk_load
  ret

.code32
BEGIN_PM:
  movl $MSG_PROT_MODE, %ebx
  call print_string_pm
  call KERNEL_OFFSET # Give control to the kernel
  jmp . # Stay here when the kernel returns control to us (if ever)

BOOT_DRIVE: .byte 0 # It is a good idea to store it in memory because 'dl' may get overwritten
MSG_REAL_MODE: .asciz "Started in 16-bit Real Mode"
MSG_PROT_MODE: .asciz "Landed in 32-bit Protected Mode"
MSG_LOAD_KERNEL: .asciz "Loading kernel into memory"

_end:
  # padding
  .fill (510 - (_end - _start)), 1, 0
  .word 0xaa55
