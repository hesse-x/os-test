.global _start
.code16
_start:
  .equ KERNEL_OFFSET, 0x9000 # The same one we used when linking the kernel

  movb %dl, BOOT_DRIVE # Remember that the BIOS sets us the boot drive in 'dl' on boot
  movw $0x9000, %bp
  movw %bp, %sp

  call load_init # read the kernel from disk
  call switch_to_pm # disable interrupts, load GDT,  etc. Finally jumps to 'BEGIN_PM'
  jmp . # Never executed

.include "os-test/boot/disk.s"
.include "os-test/boot/gdt.s"
.include "os-test/boot/switch_pm.s"
.include "os-test/boot/mem.s"

.code16
load_init:
  call probe_memory
  movw $KERNEL_OFFSET, %bx # Read from disk and store in 0x9000
  movb $4, %dh # Our future kernel will be larger, make this big
  movb BOOT_DRIVE, %dl
  call disk_load
  ret

.code32
BEGIN_PM:
  call KERNEL_OFFSET # Give control to the kernel
  jmp . # Stay here when the kernel returns control to us (if ever)

BOOT_DRIVE: .byte 0 # It is a good idea to store it in memory because 'dl' may get overwritten

_end:
  # padding
  .fill (510 - (_end - _start)), 1, 0
  .word 0xaa55
