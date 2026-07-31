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
# 层间依赖图（reface_cmake.md §4.1/§3.5）：bsd_obj 依赖 xcore_obj（传递 xcore_mem/arch_x64）。
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

# ===================== mman cross-consistency guard (musl_worklist mman §4) =====================
# mman_sync_check — a zero-code OBJECT library that exists ONLY to fail the
# build if the userspace bits/mman.h (OS-specific mman constants musl's generic
# <sys/mman.h> lacks) and the kernel's shared UAPI include/uapi/xos/mman.h
# drift apart. After the mman module switched to musl the userspace libc gets
# the common MAP_*/PROT_* set from musl's <sys/mman.h> and the OS-specific
# MAP_FIXED_NOREPLACE/MAP_SHARED_VALIDATE/MAP_GROWSUP/PROT_SEM/MFD_*/MAP_32BIT
# from user/include/bits/mman.h; the kernel reads those raw flags out of
# xos/mman.h. kernel/bsd/mman_sync.c captures bits/mman.h's values into an
# enum, clears them, includes xos/mman.h, and _Static_asserts each matches.
# Same kernel-TU shape as fcntl_sync_check, BUT the include order is INVERTED
# relative to fcntl_sync_check: user/include is placed AHEAD of musl so that
# <bits/mman.h> resolves to the REPO's user/include/bits/mman.h (the OS-specific
# MAP_FIXED_NOREPLACE/MAP_SHARED_VALIDATE/MAP_GROWSUP/PROT_SEM/MFD_*/MAP_32BIT
# set), NOT musl's arch/x86_64/bits/mman.h (which carries only MAP_32BIT).
# fcntl_sync_check does the opposite (musl first) because it intentionally
# captures musl's full bits/fcntl.h; here the repo's bits/mman.h is the
# userspace source of truth being asserted against xos/mman.h. musl's FULL
# <sys/mman.h> is still NOT pulled (its MAP_SHARED/PROT_READ would collide
# with xos/mman.h) — only the macro-only bits/mman.h is included.
add_library(mman_sync_check OBJECT mman_sync.c)
target_include_directories(mman_sync_check PRIVATE
    ${CMAKE_SOURCE_DIR}/user/include
    ${CMAKE_SOURCE_DIR}/include/uapi
    ${CMAKE_SOURCE_DIR}/third_party/musl/include
    ${CMAKE_SOURCE_DIR}/third_party/musl/arch/x86_64
    ${CMAKE_SOURCE_DIR}/third_party/musl/arch/generic
    ${CMAKE_SOURCE_DIR})
target_link_libraries(mman_sync_check PRIVATE os_base_options os_warn os_kernel_warn)
target_compile_options(mman_sync_check PRIVATE
    -fPIE -std=gnu17 -fvisibility=hidden -mno-sse -mno-sse2 -mno-mmx)
target_compile_definitions(mman_sync_check PRIVATE __KERNEL__)
set_target_properties(mman_sync_check PROPERTIES POSITION_INDEPENDENT_CODE OFF)
