add_subdirectory(mem)

add_kernel_object(kernel_obj
    SOURCES kernel.cc serial.cc trap.cc proc.cc fb.cc ata.cc
)
