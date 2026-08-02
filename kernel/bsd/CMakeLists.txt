# Copyright (c) 2026 hesse
#
# SPDX-License-Identifier: MIT

add_kernel_object(bsd_obj
    SOURCES init.c syscall.c proc.c proc_create.c signal.c
            vfs.c fat32.c inode.c page_cache.c devtmpfs.c mount.c sysfs.c tmpfs.c procfs.c
            socket.c pty.c elf_loader.c futex.c
            file_wq.c file_poll.c file_lock.c eventpoll.c eventfd.c timerfd.c signalfd.c
            netlink.c fops.c ipcfd.c random.c file_fault.c
            evdev_broker.c kfifo.c
)
# Layer dependency graph: bsd_obj -> xcore_obj (transitively xcore_mem/arch_x64).
target_link_libraries(bsd_obj PUBLIC xcore_obj)

# ===================== fcntl cross-consistency guard (fcntl_worklist §3c) =====================
# fcntl_sync_check — a zero-code OBJECT library that exists ONLY to fail the
# build if the userspace musl <fcntl.h> and the kernel-private kernel/bsd/kfcntl.h
# drift apart. After the fcntl header split (§3b) the two no longer share a UAPI
# header, so a silent constant/struct drift would corrupt the kernel↔user ABI
# (e.g. the kernel reading a struct flock the user filled via musl's layout).
# kernel/bsd/fcntl_sync.c pulls musl's arch/x86_64 bits/fcntl.h (pure macros),
# captures every shared constant into an enum, clears musl's macros, includes
# kernel/bsd/kfcntl.h, and _Static_asserts each value matches — plus struct
# flock / f_owner_ex layout and UTIME_NOW/OMIT literals.
#
# Built as a kernel TU (os_base_options + os_warn + os_kernel_warn + -fPIE
# -std=gnu17 -mno-sse/-mmx, __KERNEL__) but with musl/include +
# musl/arch/x86_64 on the include path ahead of the repo root, so <bits/fcntl.h>
# resolves to musl's. musl's FULL <fcntl.h> is deliberately NOT on the path:
# its struct flock would collide with kfcntl.h's. The .o is empty (every check
# is compile-time); it is merged into the kernel link via $<TARGET_OBJECTS:...>
# in the root CMakeLists myos.elf target_sources purely to gate the build.
add_library(fcntl_sync_check OBJECT fcntl_sync.c)
target_include_directories(fcntl_sync_check PRIVATE
    ${CMAKE_BINARY_DIR}/musl_gen
    ${CMAKE_SOURCE_DIR}/third_party/musl/include
    ${CMAKE_SOURCE_DIR}/third_party/musl/arch/x86_64
    ${CMAKE_SOURCE_DIR}/third_party/musl/arch/generic
    ${CMAKE_SOURCE_DIR}/user/include
    ${CMAKE_SOURCE_DIR}/include/uapi
    ${CMAKE_SOURCE_DIR})
target_link_libraries(fcntl_sync_check PRIVATE os_base_options os_warn os_kernel_warn)
target_compile_options(fcntl_sync_check PRIVATE
    -fPIE -std=gnu17 -fvisibility=hidden -mno-sse -mno-sse2 -mno-mmx)
target_compile_definitions(fcntl_sync_check PRIVATE __KERNEL__)
set_target_properties(fcntl_sync_check PROPERTIES POSITION_INDEPENDENT_CODE OFF)
add_dependencies(fcntl_sync_check musl_headers)
