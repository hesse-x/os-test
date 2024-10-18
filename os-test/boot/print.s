.code16
print:
    pusha

# keep this in mind:
# while (string[i] != 0) { print string[i]# i++ }

# the comparison for string end (null byte)
loop:
    movb (%bx), %al # 'bx' is the base address for the string
    cmpb $0, %al  
    je done

    # the part where we print with the BIOS help
    movb $0x0e, %ah
    int $0x10 # 'al' already contains the char

    # increment pointer and do next loop
    addw $1, %bx
    jmp loop

done:
    popa
    ret

print_nl:
    pusha
    
    movb $0x0e, %ah
    movb $0x0a, %al # newline char
    int $0x10
    movb $0x0d, %al # carriage return
    int $0x10
    
    popa
    ret
