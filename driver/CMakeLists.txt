# driver object library

add_library(driver_obj OBJECT
    kbd.cc
    fb.cc
    ata.cc
)

target_include_directories(driver_obj PRIVATE ${CMAKE_SOURCE_DIR})
set_target_properties(driver_obj PROPERTIES
    CXX_STANDARD 17
    POSITION_INDEPENDENT_CODE OFF
)
