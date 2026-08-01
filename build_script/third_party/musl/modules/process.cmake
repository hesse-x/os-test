# POSIX process group: exec family + wait family + posix_spawn closure.
#   exec: execl/execle/execlp/execv/execve/execvp/fexecve (src/process/{execl,...,
#         execve,execvp,fexecve}.c) — pure forwarders to execv/execve/execvp,
#         no new deps. execve.c is `syscall(SYS_execve)`; the kernel tolerates
#         envp==NULL (kernel/bsd/proc.c:1718 `if (envp_ptr)`), so it replaces
#         the former hand-written execve in user/lib/sys_process.cc verbatim.
#   wait: wait/waitpid/waitid (src/process/{wait,waitpid,waitid}.c). waitpid
#         routes through sys_wait4_cp (a cancellation point, unlike the old
#         bare sys_waitpid wrapper); the syscall_cp machinery is already built
#         by the pthread module (src/thread/syscall_cp.c + x86_64 syscall_cp.s).
# fork/vfork/_Fork/system are deliberately NOT here: they stay in the
# hand-written user/lib/sys_process.cc (fork is a bare sys_fork; musl's fork
# pulls the full atfork lock table + TLS reset, a larger change out of scope).
file(GLOB MUSL_PROCESS_SOURCES CONFIGURE_DEPENDS
    ${MUSL_DIR}/src/process/execl.c
    ${MUSL_DIR}/src/process/execle.c
    ${MUSL_DIR}/src/process/execlp.c
    ${MUSL_DIR}/src/process/execv.c
    ${MUSL_DIR}/src/process/execve.c
    ${MUSL_DIR}/src/process/execvp.c
    ${MUSL_DIR}/src/process/fexecve.c
    ${MUSL_DIR}/src/process/wait.c
    ${MUSL_DIR}/src/process/waitpid.c
    ${MUSL_DIR}/src/process/waitid.c
    ${MUSL_DIR}/src/process/posix_spawn.c
    ${MUSL_DIR}/src/process/posix_spawnp.c
    ${MUSL_DIR}/src/process/posix_spawnattr_*.c
    ${MUSL_DIR}/src/process/posix_spawn_file_actions_*.c)
# The repository-owned implementation explicitly rejects scheduling flags that
# this musl version otherwise accepts but silently ignores in posix_spawn().
list(REMOVE_ITEM MUSL_PROCESS_SOURCES
    ${MUSL_DIR}/src/process/posix_spawnattr_setflags.c)
list(APPEND MUSL_PROCESS_SOURCES
    ${CMAKE_SOURCE_DIR}/user/lib/posix_spawn_policy.c)

add_musl_lib(musl_process_objs SOURCES ${MUSL_PROCESS_SOURCES})
