add_kernel_object(xcore_mem
    SOURCES alloc.c user_mapping.c slab.c copy_user.c
)

if(SANITIZE)
    target_sources(xcore_mem PRIVATE ${CMAKE_SOURCE_DIR}/kernel/xcore/mem/kasan.c)
endif()
