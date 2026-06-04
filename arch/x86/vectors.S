# vectors.S - ISR stubs for vector 0-47
.text
.globl vector0
vector0:
  pushl $0
  pushl $0
  jmp __alltraps
.globl vector1
vector1:
  pushl $0
  pushl $1
  jmp __alltraps
.globl vector2
vector2:
  pushl $0
  pushl $2
  jmp __alltraps
.globl vector3
vector3:
  pushl $0
  pushl $3
  jmp __alltraps
.globl vector4
vector4:
  pushl $0
  pushl $4
  jmp __alltraps
.globl vector5
vector5:
  pushl $0
  pushl $5
  jmp __alltraps
.globl vector6
vector6:
  pushl $0
  pushl $6
  jmp __alltraps
.globl vector7
vector7:
  pushl $0
  pushl $7
  jmp __alltraps
.globl vector8
vector8:
  pushl $8
  jmp __alltraps
.globl vector9
vector9:
  pushl $0
  pushl $9
  jmp __alltraps
.globl vector10
vector10:
  pushl $10
  jmp __alltraps
.globl vector11
vector11:
  pushl $11
  jmp __alltraps
.globl vector12
vector12:
  pushl $12
  jmp __alltraps
.globl vector13
vector13:
  pushl $13
  jmp __alltraps
.globl vector14
vector14:
  pushl $14
  jmp __alltraps
.globl vector15
vector15:
  pushl $0
  pushl $15
  jmp __alltraps
.globl vector16
vector16:
  pushl $0
  pushl $16
  jmp __alltraps
.globl vector17
vector17:
  pushl $17
  jmp __alltraps
.globl vector18
vector18:
  pushl $0
  pushl $18
  jmp __alltraps
.globl vector19
vector19:
  pushl $0
  pushl $19
  jmp __alltraps
.globl vector20
vector20:
  pushl $0
  pushl $20
  jmp __alltraps
.globl vector21
vector21:
  pushl $0
  pushl $21
  jmp __alltraps
.globl vector22
vector22:
  pushl $0
  pushl $22
  jmp __alltraps
.globl vector23
vector23:
  pushl $0
  pushl $23
  jmp __alltraps
.globl vector24
vector24:
  pushl $0
  pushl $24
  jmp __alltraps
.globl vector25
vector25:
  pushl $0
  pushl $25
  jmp __alltraps
.globl vector26
vector26:
  pushl $0
  pushl $26
  jmp __alltraps
.globl vector27
vector27:
  pushl $0
  pushl $27
  jmp __alltraps
.globl vector28
vector28:
  pushl $0
  pushl $28
  jmp __alltraps
.globl vector29
vector29:
  pushl $0
  pushl $29
  jmp __alltraps
.globl vector30
vector30:
  pushl $0
  pushl $30
  jmp __alltraps
.globl vector31
vector31:
  pushl $0
  pushl $31
  jmp __alltraps
.globl vector32
vector32:
  pushl $0
  pushl $32
  jmp __alltraps
.globl vector33
vector33:
  pushl $0
  pushl $33
  jmp __alltraps
.globl vector34
vector34:
  pushl $0
  pushl $34
  jmp __alltraps
.globl vector35
vector35:
  pushl $0
  pushl $35
  jmp __alltraps
.globl vector36
vector36:
  pushl $0
  pushl $36
  jmp __alltraps
.globl vector37
vector37:
  pushl $0
  pushl $37
  jmp __alltraps
.globl vector38
vector38:
  pushl $0
  pushl $38
  jmp __alltraps
.globl vector39
vector39:
  pushl $0
  pushl $39
  jmp __alltraps
.globl vector40
vector40:
  pushl $0
  pushl $40
  jmp __alltraps
.globl vector41
vector41:
  pushl $0
  pushl $41
  jmp __alltraps
.globl vector42
vector42:
  pushl $0
  pushl $42
  jmp __alltraps
.globl vector43
vector43:
  pushl $0
  pushl $43
  jmp __alltraps
.globl vector44
vector44:
  pushl $0
  pushl $44
  jmp __alltraps
.globl vector45
vector45:
  pushl $0
  pushl $45
  jmp __alltraps
.globl vector46
vector46:
  pushl $0
  pushl $46
  jmp __alltraps
.globl vector47
vector47:
  pushl $0
  pushl $47
  jmp __alltraps

.globl vector128
vector128:
  pushl $0
  pushl $128
  jmp syscall_entry
