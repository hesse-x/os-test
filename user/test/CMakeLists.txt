# Copyright (c) 2026 hesse
#
# SPDX-License-Identifier: MIT

# user/test/ — Unity-based test ELF targets

# Test runner (no Unity needed)
if(TEST)
    add_user_dyn_elf(test_runner C SOURCES test_runner.c LINK_LIBS c DEFS TEST)
else()
    add_user_dyn_elf(test_runner C SOURCES test_runner.c LINK_LIBS c)
endif()

# Layer 1: unistd (pipe/read/write/close)
add_user_dyn_elf(pipe C SOURCES test_pipe.c ${CMAKE_SOURCE_DIR}/third_party/Unity/src/unity.c LINK_LIBS c DEFS UNITY_EXCLUDE_SETJMP_H UNITY_EXCLUDE_MATH_H)

# Layer 2: fcntl (open/close/dup2/fcntl/lseek)
add_user_dyn_elf(fcntl C SOURCES test_fcntl.c ${CMAKE_SOURCE_DIR}/third_party/Unity/src/unity.c LINK_LIBS c DEFS UNITY_EXCLUDE_SETJMP_H UNITY_EXCLUDE_MATH_H)

# Layer 3: string / malloc / stdio
add_user_dyn_elf(string C SOURCES test_string.c ${CMAKE_SOURCE_DIR}/third_party/Unity/src/unity.c LINK_LIBS c DEFS UNITY_EXCLUDE_SETJMP_H UNITY_EXCLUDE_MATH_H)
add_user_dyn_elf(malloc C SOURCES test_malloc.c ${CMAKE_SOURCE_DIR}/third_party/Unity/src/unity.c LINK_LIBS c DEFS UNITY_EXCLUDE_SETJMP_H UNITY_EXCLUDE_MATH_H)
add_user_dyn_elf(stdio C SOURCES test_stdio.c ${CMAKE_SOURCE_DIR}/third_party/Unity/src/unity.c LINK_LIBS c DEFS UNITY_EXCLUDE_SETJMP_H UNITY_EXCLUDE_MATH_H)

# Layer 4: sys/mman
add_user_dyn_elf(mmap C SOURCES test_mmap.c ${CMAKE_SOURCE_DIR}/third_party/Unity/src/unity.c LINK_LIBS c DEFS UNITY_EXCLUDE_SETJMP_H UNITY_EXCLUDE_MATH_H)

# Layer 5: IPC
add_user_dyn_elf(ipc C SOURCES test_ipc.c ${CMAKE_SOURCE_DIR}/third_party/Unity/src/unity.c LINK_LIBS c DEFS UNITY_EXCLUDE_SETJMP_H UNITY_EXCLUDE_MATH_H)

# Layer 6: socket
add_user_dyn_elf(socket C SOURCES test_socket.c ${CMAKE_SOURCE_DIR}/third_party/Unity/src/unity.c LINK_LIBS c DEFS UNITY_EXCLUDE_SETJMP_H UNITY_EXCLUDE_MATH_H)

# Layer 7: process
add_user_dyn_elf(process C SOURCES test_process.c ${CMAKE_SOURCE_DIR}/third_party/Unity/src/unity.c LINK_LIBS c DEFS UNITY_EXCLUDE_SETJMP_H UNITY_EXCLUDE_MATH_H)

# Layer 8: signal
add_user_dyn_elf(signal C SOURCES test_signal.c ${CMAKE_SOURCE_DIR}/third_party/Unity/src/unity.c LINK_LIBS c DEFS UNITY_EXCLUDE_SETJMP_H UNITY_EXCLUDE_MATH_H)

# Layer 9: poll
add_user_dyn_elf(poll C SOURCES test_poll.c ${CMAKE_SOURCE_DIR}/third_party/Unity/src/unity.c LINK_LIBS c DEFS UNITY_EXCLUDE_SETJMP_H UNITY_EXCLUDE_MATH_H)

# Layer 10: PCI/device
add_user_dyn_elf(pci C SOURCES test_pci.c ${CMAKE_SOURCE_DIR}/third_party/Unity/src/unity.c LINK_LIBS c DEFS UNITY_EXCLUDE_SETJMP_H UNITY_EXCLUDE_MATH_H)

# Layer 11: ioctl/fstat/isatty
add_user_dyn_elf(ioctl C SOURCES test_ioctl.c ${CMAKE_SOURCE_DIR}/third_party/Unity/src/unity.c LINK_LIBS c DEFS UNITY_EXCLUDE_SETJMP_H UNITY_EXCLUDE_MATH_H)

# Layer 12: device VFS unified path
add_user_dyn_elf(dev_vfs C SOURCES test_dev_vfs.c ${CMAKE_SOURCE_DIR}/third_party/Unity/src/unity.c LINK_LIBS c DEFS UNITY_EXCLUDE_SETJMP_H UNITY_EXCLUDE_MATH_H)

# FPU/SSE smoke test: explicit inline-asm movsd+mulsd to verify user-mode
# SSE path (CR4.OSFXSR + lazy FPU #NM handler). No Unity — simple printf+exit.
add_user_dyn_elf(test_sse_smoke C SOURCES test_sse_smoke.c LINK_LIBS c)

# CPU-bound stress test for work-stealing balance verification (launched
# manually as multiple instances; not a test_runner regression)
add_user_dyn_elf(test_cpu_burn C SOURCES test_cpu_burn.c LINK_LIBS c)

# Layer 13: pthread
add_user_dyn_elf(pthread C SOURCES test_pthread.c ${CMAKE_SOURCE_DIR}/third_party/Unity/src/unity.c LINK_LIBS c DEFS UNITY_EXCLUDE_SETJMP_H UNITY_EXCLUDE_MATH_H)

# DRM virtio-gpu test (no Unity — diagnostic printf)
add_user_dyn_elf(drm_test C SOURCES drm_test.c LINK_LIBS c)

# Layer 14: DRM ioctl (GET_MAGIC, AUTH_MAGIC, ADDFB2, GEM_CLOSE, GETFB, caps)
add_user_dyn_elf(drm_ioctl C SOURCES test_drm_ioctl.c ${CMAKE_SOURCE_DIR}/third_party/Unity/src/unity.c LINK_LIBS c DEFS UNITY_EXCLUDE_SETJMP_H UNITY_EXCLUDE_MATH_H)

# evdev EVIOCG* ioctl test (only in TEST builds)
# Target named test_evdev (not evdev): user/CMakeLists.txt already defines the
# evdev driver target -> build/evdev.elf (/driver/evdev.dev). Same target name
# would collide on the output file build/evdev.elf and the test ELF would
# overwrite the driver, so init would spawn the test instead of the driver.
if(TEST)
    add_user_dyn_elf(test_evdev C SOURCES test_evdev.c ${CMAKE_SOURCE_DIR}/third_party/Unity/src/unity.c LINK_LIBS c DEFS UNITY_EXCLUDE_SETJMP_H UNITY_EXCLUDE_MATH_H TEST INCLUDE_DIRS ${CMAKE_SOURCE_DIR}/third_party/libinput/include/linux)
endif()

# Layer 15: ioctl variable-length (user-space driver proxy, RECV_IOCTL path)
add_user_dyn_elf(ioctl_varlen C SOURCES test_ioctl_varlen.c ${CMAKE_SOURCE_DIR}/third_party/Unity/src/unity.c LINK_LIBS c DEFS UNITY_EXCLUDE_SETJMP_H UNITY_EXCLUDE_MATH_H)

# Layer 16: epoll
add_user_dyn_elf(epoll C SOURCES test_epoll.c ${CMAKE_SOURCE_DIR}/third_party/Unity/src/unity.c LINK_LIBS c DEFS UNITY_EXCLUDE_SETJMP_H UNITY_EXCLUDE_MATH_H)

# Layer 17: eventfd
add_user_dyn_elf(eventfd C SOURCES test_eventfd.c ${CMAKE_SOURCE_DIR}/third_party/Unity/src/unity.c LINK_LIBS c DEFS UNITY_EXCLUDE_SETJMP_H UNITY_EXCLUDE_MATH_H)

# Layer 18: timerfd
add_user_dyn_elf(timerfd C SOURCES test_timerfd.c ${CMAKE_SOURCE_DIR}/third_party/Unity/src/unity.c LINK_LIBS c DEFS UNITY_EXCLUDE_SETJMP_H UNITY_EXCLUDE_MATH_H)

# Layer 19: signalfd
add_user_dyn_elf(signalfd C SOURCES test_signalfd.c ${CMAKE_SOURCE_DIR}/third_party/Unity/src/unity.c LINK_LIBS c DEFS UNITY_EXCLUDE_SETJMP_H UNITY_EXCLUDE_MATH_H)

# mount framework test
add_user_dyn_elf(mount C SOURCES test_mount.c ${CMAKE_SOURCE_DIR}/third_party/Unity/src/unity.c LINK_LIBS c DEFS UNITY_EXCLUDE_SETJMP_H UNITY_EXCLUDE_MATH_H)
