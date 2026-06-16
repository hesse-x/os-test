# vectors.S - ISR stubs for vector 0-63 (64-bit)
# 64-bit: pushq $0 (dummy error) + pushq $vector_num, then jmp __alltraps
# CPU-exception vectors that push error code (8,10-14,17) only push the vector num
.text
.code64
.globl vector0
vector0:
  pushq $0
  pushq $0
  jmp __alltraps
.globl vector1
vector1:
  pushq $0
  pushq $1
  jmp __alltraps
.globl vector2
vector2:
  pushq $0
  pushq $2
  jmp __alltraps
.globl vector3
vector3:
  pushq $0
  pushq $3
  jmp __alltraps
.globl vector4
vector4:
  pushq $0
  pushq $4
  jmp __alltraps
.globl vector5
vector5:
  pushq $0
  pushq $5
  jmp __alltraps
.globl vector6
vector6:
  pushq $0
  pushq $6
  jmp __alltraps
.globl vector7
vector7:
  pushq $0
  pushq $7
  jmp __alltraps
.globl vector8
vector8:
  pushq $8
  jmp __alltraps
.globl vector9
vector9:
  pushq $0
  pushq $9
  jmp __alltraps
.globl vector10
vector10:
  pushq $10
  jmp __alltraps
.globl vector11
vector11:
  pushq $11
  jmp __alltraps
.globl vector12
vector12:
  pushq $12
  jmp __alltraps
.globl vector13
vector13:
  pushq $13
  jmp __alltraps
.globl vector14
vector14:
  pushq $14
  jmp __alltraps
.globl vector15
vector15:
  pushq $0
  pushq $15
  jmp __alltraps
.globl vector16
vector16:
  pushq $0
  pushq $16
  jmp __alltraps
.globl vector17
vector17:
  pushq $17
  jmp __alltraps
.globl vector18
vector18:
  pushq $0
  pushq $18
  jmp __alltraps
.globl vector19
vector19:
  pushq $0
  pushq $19
  jmp __alltraps
.globl vector20
vector20:
  pushq $0
  pushq $20
  jmp __alltraps
.globl vector21
vector21:
  pushq $0
  pushq $21
  jmp __alltraps
.globl vector22
vector22:
  pushq $0
  pushq $22
  jmp __alltraps
.globl vector23
vector23:
  pushq $0
  pushq $23
  jmp __alltraps
.globl vector24
vector24:
  pushq $0
  pushq $24
  jmp __alltraps
.globl vector25
vector25:
  pushq $0
  pushq $25
  jmp __alltraps
.globl vector26
vector26:
  pushq $0
  pushq $26
  jmp __alltraps
.globl vector27
vector27:
  pushq $0
  pushq $27
  jmp __alltraps
.globl vector28
vector28:
  pushq $0
  pushq $28
  jmp __alltraps
.globl vector29
vector29:
  pushq $0
  pushq $29
  jmp __alltraps
.globl vector30
vector30:
  pushq $0
  pushq $30
  jmp __alltraps
.globl vector31
vector31:
  pushq $0
  pushq $31
  jmp __alltraps
.globl vector32
vector32:
  pushq $0
  pushq $32
  jmp __alltraps
.globl vector33
vector33:
  pushq $0
  pushq $33
  jmp __alltraps
.globl vector34
vector34:
  pushq $0
  pushq $34
  jmp __alltraps
.globl vector35
vector35:
  pushq $0
  pushq $35
  jmp __alltraps
.globl vector36
vector36:
  pushq $0
  pushq $36
  jmp __alltraps
.globl vector37
vector37:
  pushq $0
  pushq $37
  jmp __alltraps
.globl vector38
vector38:
  pushq $0
  pushq $38
  jmp __alltraps
.globl vector39
vector39:
  pushq $0
  pushq $39
  jmp __alltraps
.globl vector40
vector40:
  pushq $0
  pushq $40
  jmp __alltraps
.globl vector41
vector41:
  pushq $0
  pushq $41
  jmp __alltraps
.globl vector42
vector42:
  pushq $0
  pushq $42
  jmp __alltraps
.globl vector43
vector43:
  pushq $0
  pushq $43
  jmp __alltraps
.globl vector44
vector44:
  pushq $0
  pushq $44
  jmp __alltraps
.globl vector45
vector45:
  pushq $0
  pushq $45
  jmp __alltraps
.globl vector46
vector46:
  pushq $0
  pushq $46
  jmp __alltraps
.globl vector47
vector47:
  pushq $0
  pushq $47
  jmp __alltraps
.globl vector48
vector48:
  pushq $0
  pushq $48
  jmp __alltraps
.globl vector49
vector49:
  pushq $0
  pushq $49
  jmp __alltraps
.globl vector50
vector50:
  pushq $0
  pushq $50
  jmp __alltraps
.globl vector51
vector51:
  pushq $0
  pushq $51
  jmp __alltraps
.globl vector52
vector52:
  pushq $0
  pushq $52
  jmp __alltraps
.globl vector53
vector53:
  pushq $0
  pushq $53
  jmp __alltraps
.globl vector54
vector54:
  pushq $0
  pushq $54
  jmp __alltraps
.globl vector55
vector55:
  pushq $0
  pushq $55
  jmp __alltraps
.globl vector56
vector56:
  pushq $0
  pushq $56
  jmp __alltraps
.globl vector57
vector57:
  pushq $0
  pushq $57
  jmp __alltraps
.globl vector58
vector58:
  pushq $0
  pushq $58
  jmp __alltraps
.globl vector59
vector59:
  pushq $0
  pushq $59
  jmp __alltraps
.globl vector60
vector60:
  pushq $0
  pushq $60
  jmp __alltraps
.globl vector61
vector61:
  pushq $0
  pushq $61
  jmp __alltraps
.globl vector62
vector62:
  pushq $0
  pushq $62
  jmp __alltraps
.globl vector63
vector63:
  pushq $0
  pushq $63
  jmp __alltraps
