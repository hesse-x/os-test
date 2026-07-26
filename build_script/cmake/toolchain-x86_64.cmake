set(CMAKE_SYSTEM_NAME Generic)

# Compiler selection: default clang, switch to gcc with -DOS_COMPILER=gcc
# (or build.sh --gcc). Both toolchains are supported; clang is the default.
if(NOT DEFINED OS_COMPILER)
    set(OS_COMPILER clang)
endif()
if(OS_COMPILER STREQUAL "gcc")
    set(OS_C_COMPILER gcc)
    set(OS_CXX_COMPILER g++)
else()
    set(OS_C_COMPILER clang)
    set(OS_CXX_COMPILER clang++)
endif()

set(CMAKE_C_COMPILER ${OS_C_COMPILER})
set(CMAKE_CXX_COMPILER ${OS_CXX_COMPILER})
set(CMAKE_ASM_COMPILER ${OS_C_COMPILER})
set(CMAKE_C_FLAGS "-m64" CACHE STRING "")
set(CMAKE_CXX_FLAGS "-m64" CACHE STRING "")
set(CMAKE_ASM_FLAGS "-m64" CACHE STRING "")

set(BUILD_SHARED_LIBS OFF)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
