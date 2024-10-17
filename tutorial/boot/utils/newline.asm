; print '\n'
newline:
    mov al, 0x0a  ; set '\n'
    int 0x10      ; call print
    mov al, 0x0d  ; set '\r'
    int 0x10      ; call print
    ret           ; return


