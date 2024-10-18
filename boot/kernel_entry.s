.code32
.extern kernel_start # Define calling point. Must have same name as kernel.c 'main' function
call kernel_start # Calls the C function. The linker will know where it is placed in memory
jmp .
