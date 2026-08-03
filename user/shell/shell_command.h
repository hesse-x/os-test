/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef USER_SHELL_COMMAND_H
#define USER_SHELL_COMMAND_H

int shell_run_command(const char *source);

// -1 if no `exit` was requested by the last shell_run_command, else the exit
// status. The interactive loop uses this to distinguish `exit` from a command
// that happened to return the same code.
int shell_requested_exit(void);

// Interactive-shell setup: own process group, ignore terminal signals, take
// the controlling terminal as foreground. Called once from main; idempotent.
void shell_init_jobcontrol(void);

#endif
