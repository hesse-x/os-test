# Copyright (c) 2026 hesse
#
# SPDX-License-Identifier: MIT

# init/ — init process (PID 3)

# IMAGE_PATH init.elf IMAGE_PARTITION 1: init.elf lives in the ESP (stub loads it
# into memory and passes it to the kernel, initrd-style — see boot.md). The ESP
# partition placement is explicit (default would be root partition 2).
if(TEST)
    add_user_elf(init C SOURCES init.c LINK_LIBS c DEFS TEST IMAGE_PATH init.elf IMAGE_PARTITION 1)
else()
    add_user_elf(init C SOURCES init.c LINK_LIBS c IMAGE_PATH init.elf IMAGE_PARTITION 1)
endif()
