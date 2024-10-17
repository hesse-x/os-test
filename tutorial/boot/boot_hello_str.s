.code16
.global _start
_start:

  movw $MSG_HELLO, %bx
  call print
  call print_nl
jmp .
  
.include "utils/print.s"

jmp . // jump to current address = infinite loop

MSG_HELLO:
  .asciz "Hello world!"

_end:
  // padding and magic number
  .fill 510 - (_end - _start), 1, 0
  .word 0xaa55

