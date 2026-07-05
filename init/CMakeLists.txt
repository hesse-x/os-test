# Copyright (c) 2026 hesse
#
# SPDX-License-Identifier: MIT

# init/ — init process (PID 3)

if(TEST)
    add_user_elf(init C SOURCES init.c LINK_LIBS c DEFS TEST)
else()
    add_user_elf(init C SOURCES init.c LINK_LIBS c)
endif()
