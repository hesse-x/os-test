/*
 * Copyright (c) 2026 hesse
 * SPDX-License-Identifier: MIT
 */
#ifndef OS_COMPOSITOR_CORE_SERVER_H
#define OS_COMPOSITOR_CORE_SERVER_H

// Owns the server lifecycle. All wl_listener objects are detached by their
// owning wlroots object before this function releases the display.
int os_compositor_run(int argc, char *argv[]);

#endif
