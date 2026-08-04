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
// the controlling terminal as foreground, install the SIGCHLD self-pipe.
// Called once from main; idempotent.
void shell_init_jobcontrol(void);

// M2-B: drain the SIGCHLD self-pipe wake bytes.
void shell_drain_sigchld(void);
// M2-B: non-blocking reap of reportable child state changes into the job
// table + background completion notices. Called by the main loop after poll
// wakes, before the prompt, and after each foreground wait.
void shell_reap_jobs(void);
// M2-B: SIGHUP+SIGCONT every running/stopped job on shell exit, then a
// bounded drain. Called from the interactive loop on `exit` / EOF.
void shell_hangup_jobs(void);
// M2-B: block until stdin is readable or a SIGCHLD wake byte arrives, so a
// background child exit can never leave the shell stuck in a blocking read.
// Returns 1 = stdin ready, 0 = wake-only.
int shell_wait_input(void);
// Single-shot poll for the linenoise edit loop: return on EITHER stdin ready OR
// a SIGCHLD wake (drained here, reaped by caller) without re-polling on wake,
// so a background completion can be surfaced mid-edit. Returns 1 = stdin ready,
// 0 = SIGCHLD wake only.
int shell_poll_input(void);

#endif // USER_SHELL_COMMAND_H
