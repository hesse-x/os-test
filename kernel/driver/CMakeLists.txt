# Copyright (c) 2026 hesse
#
# SPDX-License-Identifier: MIT

add_kernel_object(driver_obj
    SOURCES init.c registry.c
            ahci.c xhci.c pci.c display.c serial.c blk_dev.c
)
