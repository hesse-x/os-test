.code16
# load 'dh' sectors from drive 'dl' into ES:BX
disk_load:
    pusha
    # reading from disk requires setting specific values in all registers
    # so we will overwrite our input parameters from 'dx'. Let's save it
    # to the stack for later use.
    pushw %dx

    movb $0x02, %ah # ah <- int 0x13 function. 0x02 = 'read'
    movb %dh, %al  # al <- number of sectors to read (0x01 .. 0x80)
    movb $0x02, %cl # cl <- sector (0x01 .. 0x11)
                   # 0x01 is our boot sector, 0x02 is the first 'available' sector
    movb $0x00, %ch # ch <- cylinder (0x0 .. 0x3FF, upper 2 bits in 'cl')
    # dl <- drive number. Our caller sets it as a parameter and gets it from BIOS
    # (0 = floppy, 1 = floppy2, 0x80 = hdd, 0x81 = hdd2)
    movb $0x00, %dh # dh <- head number (0x0 .. 0xF)

    # [es:bx] <- pointer to buffer where the data will be stored
    # caller sets it up for us, and it is actually the standard location for int 13h
    int $0x13      # BIOS interrupt
    jc disk_error # if error (stored in the carry bit)

    popw %dx
    cmpb %dh, %al    # BIOS also sets 'al' to the # of sectors read. Compare it.
    jne sectors_error
    popa
    ret


disk_error:
    jmp disk_loop

sectors_error:

disk_loop:
    jmp .

DISK_ERROR: .asciz "Disk read error"
SECTORS_ERROR: .asciz "Incorrect number of sectors read"
