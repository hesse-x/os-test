# Copyright (c) 2026 hesse
#
# SPDX-License-Identifier: MIT

# init/ — init process (PID 3)

# IMAGE_PATH init.elf IMAGE_PARTITION 1: init.elf lives in the ESP (stub loads it
# into memory and passes it to the kernel, initrd-style — see boot.md). The ESP
# partition placement is explicit (default would be root partition 2).
set(_init_defs)
if(TEST)
    list(APPEND _init_defs TEST)
endif()
if(TINYWL)
    list(APPEND _init_defs TINYWL)
endif()
add_user_elf(init C SOURCES init.c LINK_LIBS c DEFS ${_init_defs} IMAGE_PATH init.elf IMAGE_PARTITION 1)
