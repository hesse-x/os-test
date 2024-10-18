.code16
# receiving the data in 'dx'
# For the examples we'll assume that we're called with dx=0x1234
print_hex:
    pusha

    movw $0, %cx # our index variable

# Strategy: get the last char of 'dx', then convert to ASCII
# Numeric ASCII values: '0' (ASCII 0x30) to '9' (0x39), so just add 0x30 to byte N.
# For alphabetic characters A-F: 'A' (ASCII 0x41) to 'F' (0x46) we'll add 0x40
# Then, move the ASCII byte to the correct position on the resulting string
hex_loop:
    cmpw $4, %cx # loop 4 times
    je end
    
    # 1. convert last char of 'dx' to ascii
    movw %dx, %ax # we will use 'ax' as our working register
    andw $0x000f, %ax # 0x1234 -> 0x0004 by masking first three to zeros
    addb $0x30, %al # add 0x30 to N to convert it to ASCII "N"
    cmpb $0x39, %al # if > 9, add extra 8 to represent 'A' to 'F'
    jle step2
    addb $7, %al # 'A' is ASCII 65 instead of 58, so 65-58=7

step2:
    # 2. get the correct position of the string to place our ASCII char
    # bx <- base address + string length - index of char
    movw $HEX_OUT + 5, %bx # base + length
    subw %cx, %bx  # our index variable
    movb %al, (%bx) # copy the ASCII char on 'al' to the position pointed by 'bx'
    rorw $4, %dx # 0x1234 -> 0x4123 -> 0x3412 -> 0x2341 -> 0x1234

    # increment index and loop
    addw $1, %cx
    jmp hex_loop

end:
    # prepare the parameter and call the function
    # remember that print receives parameters in 'bx'
    movw $HEX_OUT, %bx
    call print

    popa
    ret

HEX_OUT:
    .ascii "0x0000"
    .byte 0 # reserve memory for our new string
