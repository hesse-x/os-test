/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

# vectors.S - ISR stubs for vector 0-127 (64-bit)
# 64-bit: pushq $0 (dummy error) + pushq $vector_num, then jmp to the entry.
# CPU-exception vectors that push error code (8,10-14,17) only push the vector num.
#
# (frame_opt.md 块一) Two entry points, mirroring Linux idtentry vs
# common_interrupt:
#   vector0..31  (synchronous exceptions) → jmp __alltraps  (stays on task stack)
#   vector32..127 + vector236 (hardware IRQs)  → jmp __irq_entry (switches to
#                                                  the per-CPU IRQ stack)
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
  jmp __irq_entry
.globl vector33
vector33:
  pushq $0
  pushq $33
  jmp __irq_entry
.globl vector34
vector34:
  pushq $0
  pushq $34
  jmp __irq_entry
.globl vector35
vector35:
  pushq $0
  pushq $35
  jmp __irq_entry
.globl vector36
vector36:
  pushq $0
  pushq $36
  jmp __irq_entry
.globl vector37
vector37:
  pushq $0
  pushq $37
  jmp __irq_entry
.globl vector38
vector38:
  pushq $0
  pushq $38
  jmp __irq_entry
.globl vector39
vector39:
  pushq $0
  pushq $39
  jmp __irq_entry
.globl vector40
vector40:
  pushq $0
  pushq $40
  jmp __irq_entry
.globl vector41
vector41:
  pushq $0
  pushq $41
  jmp __irq_entry
.globl vector42
vector42:
  pushq $0
  pushq $42
  jmp __irq_entry
.globl vector43
vector43:
  pushq $0
  pushq $43
  jmp __irq_entry
.globl vector44
vector44:
  pushq $0
  pushq $44
  jmp __irq_entry
.globl vector45
vector45:
  pushq $0
  pushq $45
  jmp __irq_entry
.globl vector46
vector46:
  pushq $0
  pushq $46
  jmp __irq_entry
.globl vector47
vector47:
  pushq $0
  pushq $47
  jmp __irq_entry
.globl vector48
vector48:
  pushq $0
  pushq $48
  jmp __irq_entry
.globl vector49
vector49:
  pushq $0
  pushq $49
  jmp __irq_entry
.globl vector50
vector50:
  pushq $0
  pushq $50
  jmp __irq_entry
.globl vector51
vector51:
  pushq $0
  pushq $51
  jmp __irq_entry
.globl vector52
vector52:
  pushq $0
  pushq $52
  jmp __irq_entry
.globl vector53
vector53:
  pushq $0
  pushq $53
  jmp __irq_entry
.globl vector54
vector54:
  pushq $0
  pushq $54
  jmp __irq_entry
.globl vector55
vector55:
  pushq $0
  pushq $55
  jmp __irq_entry
.globl vector56
vector56:
  pushq $0
  pushq $56
  jmp __irq_entry
.globl vector57
vector57:
  pushq $0
  pushq $57
  jmp __irq_entry
.globl vector58
vector58:
  pushq $0
  pushq $58
  jmp __irq_entry
.globl vector59
vector59:
  pushq $0
  pushq $59
  jmp __irq_entry
.globl vector60
vector60:
  pushq $0
  pushq $60
  jmp __irq_entry
.globl vector61
vector61:
  pushq $0
  pushq $61
  jmp __irq_entry
.globl vector62
vector62:
  pushq $0
  pushq $62
  jmp __irq_entry
.globl vector63
vector63:
  pushq $0
  pushq $63
  jmp __irq_entry
.globl vector64
vector64:
  pushq $0
  pushq $64
  jmp __irq_entry
.globl vector65
vector65:
  pushq $0
  pushq $65
  jmp __irq_entry
.globl vector66
vector66:
  pushq $0
  pushq $66
  jmp __irq_entry
.globl vector67
vector67:
  pushq $0
  pushq $67
  jmp __irq_entry
.globl vector68
vector68:
  pushq $0
  pushq $68
  jmp __irq_entry
.globl vector69
vector69:
  pushq $0
  pushq $69
  jmp __irq_entry
.globl vector70
vector70:
  pushq $0
  pushq $70
  jmp __irq_entry
.globl vector71
vector71:
  pushq $0
  pushq $71
  jmp __irq_entry
.globl vector72
vector72:
  pushq $0
  pushq $72
  jmp __irq_entry
.globl vector73
vector73:
  pushq $0
  pushq $73
  jmp __irq_entry
.globl vector74
vector74:
  pushq $0
  pushq $74
  jmp __irq_entry
.globl vector75
vector75:
  pushq $0
  pushq $75
  jmp __irq_entry
.globl vector76
vector76:
  pushq $0
  pushq $76
  jmp __irq_entry
.globl vector77
vector77:
  pushq $0
  pushq $77
  jmp __irq_entry
.globl vector78
vector78:
  pushq $0
  pushq $78
  jmp __irq_entry
.globl vector79
vector79:
  pushq $0
  pushq $79
  jmp __irq_entry
.globl vector80
vector80:
  pushq $0
  pushq $80
  jmp __irq_entry
.globl vector81
vector81:
  pushq $0
  pushq $81
  jmp __irq_entry
.globl vector82
vector82:
  pushq $0
  pushq $82
  jmp __irq_entry
.globl vector83
vector83:
  pushq $0
  pushq $83
  jmp __irq_entry
.globl vector84
vector84:
  pushq $0
  pushq $84
  jmp __irq_entry
.globl vector85
vector85:
  pushq $0
  pushq $85
  jmp __irq_entry
.globl vector86
vector86:
  pushq $0
  pushq $86
  jmp __irq_entry
.globl vector87
vector87:
  pushq $0
  pushq $87
  jmp __irq_entry
.globl vector88
vector88:
  pushq $0
  pushq $88
  jmp __irq_entry
.globl vector89
vector89:
  pushq $0
  pushq $89
  jmp __irq_entry
.globl vector90
vector90:
  pushq $0
  pushq $90
  jmp __irq_entry
.globl vector91
vector91:
  pushq $0
  pushq $91
  jmp __irq_entry
.globl vector92
vector92:
  pushq $0
  pushq $92
  jmp __irq_entry
.globl vector93
vector93:
  pushq $0
  pushq $93
  jmp __irq_entry
.globl vector94
vector94:
  pushq $0
  pushq $94
  jmp __irq_entry
.globl vector95
vector95:
  pushq $0
  pushq $95
  jmp __irq_entry
.globl vector96
vector96:
  pushq $0
  pushq $96
  jmp __irq_entry
.globl vector97
vector97:
  pushq $0
  pushq $97
  jmp __irq_entry
.globl vector98
vector98:
  pushq $0
  pushq $98
  jmp __irq_entry
.globl vector99
vector99:
  pushq $0
  pushq $99
  jmp __irq_entry
.globl vector100
vector100:
  pushq $0
  pushq $100
  jmp __irq_entry
.globl vector101
vector101:
  pushq $0
  pushq $101
  jmp __irq_entry
.globl vector102
vector102:
  pushq $0
  pushq $102
  jmp __irq_entry
.globl vector103
vector103:
  pushq $0
  pushq $103
  jmp __irq_entry
.globl vector104
vector104:
  pushq $0
  pushq $104
  jmp __irq_entry
.globl vector105
vector105:
  pushq $0
  pushq $105
  jmp __irq_entry
.globl vector106
vector106:
  pushq $0
  pushq $106
  jmp __irq_entry
.globl vector107
vector107:
  pushq $0
  pushq $107
  jmp __irq_entry
.globl vector108
vector108:
  pushq $0
  pushq $108
  jmp __irq_entry
.globl vector109
vector109:
  pushq $0
  pushq $109
  jmp __irq_entry
.globl vector110
vector110:
  pushq $0
  pushq $110
  jmp __irq_entry
.globl vector111
vector111:
  pushq $0
  pushq $111
  jmp __irq_entry
.globl vector112
vector112:
  pushq $0
  pushq $112
  jmp __irq_entry
.globl vector113
vector113:
  pushq $0
  pushq $113
  jmp __irq_entry
.globl vector114
vector114:
  pushq $0
  pushq $114
  jmp __irq_entry
.globl vector115
vector115:
  pushq $0
  pushq $115
  jmp __irq_entry
.globl vector116
vector116:
  pushq $0
  pushq $116
  jmp __irq_entry
.globl vector117
vector117:
  pushq $0
  pushq $117
  jmp __irq_entry
.globl vector118
vector118:
  pushq $0
  pushq $118
  jmp __irq_entry
.globl vector119
vector119:
  pushq $0
  pushq $119
  jmp __irq_entry
.globl vector120
vector120:
  pushq $0
  pushq $120
  jmp __irq_entry
.globl vector121
vector121:
  pushq $0
  pushq $121
  jmp __irq_entry
.globl vector122
vector122:
  pushq $0
  pushq $122
  jmp __irq_entry
.globl vector123
vector123:
  pushq $0
  pushq $123
  jmp __irq_entry
.globl vector124
vector124:
  pushq $0
  pushq $124
  jmp __irq_entry
.globl vector125
vector125:
  pushq $0
  pushq $125
  jmp __irq_entry
.globl vector126
vector126:
  pushq $0
  pushq $126
  jmp __irq_entry
.globl vector127
vector127:
  pushq $0
  pushq $127
  jmp __irq_entry

.globl vector236
vector236:
  pushq $0
  pushq $236
  jmp __irq_entry

    .section .note.GNU-stack,"",@progbits
