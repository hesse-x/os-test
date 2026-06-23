add_kernel_object(kernel_mem
    SOURCES alloc.c user_mapping.c slab.c
)

if(SANITIZE)
    target_sources(kernel_mem PRIVATE ${CMAKE_SOURCE_DIR}/kernel/mem/kasan.c)
endif()
