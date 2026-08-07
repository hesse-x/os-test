# Cross toolchain for building LLVM/Clang as target-side applications.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

get_filename_component(OS_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(OS_SYSROOT "${OS_ROOT}/build/sysroot" CACHE PATH "Target OS sysroot")

set(CMAKE_C_COMPILER clang)
set(CMAKE_CXX_COMPILER clang++)
set(CMAKE_C_COMPILER_TARGET x86_64-unknown-linux-musl)
set(CMAKE_CXX_COMPILER_TARGET x86_64-unknown-linux-musl)
set(CMAKE_SYSROOT "${OS_SYSROOT}")

set(OS_COMMON_FLAGS "-m64 -fPIC -nodefaultlibs")
set(CMAKE_C_FLAGS_INIT "${OS_COMMON_FLAGS}")
set(CMAKE_CXX_FLAGS_INIT
    "${OS_COMMON_FLAGS} -stdlib=libc++ -nostdinc++ -I${OS_SYSROOT}/usr/include/c++/v1")
set(CMAKE_EXE_LINKER_FLAGS_INIT
    "--rtlib=compiler-rt -nodefaultlibs -Wl,--hash-style=gnu -Wl,--dynamic-linker,/lib/ld-musl-x86_64.so.1")
set(CMAKE_SHARED_LINKER_FLAGS_INIT
    "--rtlib=compiler-rt -nodefaultlibs -Wl,--hash-style=gnu")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "${CMAKE_SHARED_LINKER_FLAGS_INIT}")

# CMake appends these after object files, preserving static-library resolution.
set(CMAKE_C_STANDARD_LIBRARIES "-lclang_rt -lc" CACHE STRING "" FORCE)
set(CMAKE_CXX_STANDARD_LIBRARIES
    "-lc++ -lc++abi -lunwind -lclang_rt -lc" CACHE STRING "" FORCE)

set(CMAKE_FIND_ROOT_PATH "${OS_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
