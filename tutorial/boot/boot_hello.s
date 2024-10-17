_start:
  movb $0x0e, %ah // tty mode
  movb $'H', %al
  int $0x10
  movb $'e', %al
  int $0x10
  movb $'l', %al
  int $0x10
  int $0x10 // 'l' is still on al, remember?
  movb $'o', %al
  int $0x10

jmp . // jump to current address = infinite loop

_end:
  // padding and magic number
  .fill 510 - (_end - _start), 1, 0
  .word 0xaa55
  
.org 0x7c00
