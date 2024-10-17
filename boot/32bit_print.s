.code32 # using 32-bit protected mode

# this is how constants are defined
.equ VIDEO_MEMORY, 0xb8000
.equ WHITE_OB_BLACK, 0x0f # the color byte for each character

print_string_pm:
    pusha
    movl $VIDEO_MEMORY, %edx

print_string_pm_loop:
    movb (%ebx), %al # [ebx] is the address of our character
    movb $WHITE_OB_BLACK, %ah

    cmpb $0, %al # check if end of string
    je print_string_pm_done

    movw %ax, (%edx) # store character + attribute in video memory
    addl $1, %ebx # next char
    addl $2, %edx # next video memory position

    jmp print_string_pm_loop

print_string_pm_done:
    popa
    ret
