# posix_spawn closure. Deliberately excludes fork/vfork/execve/wait/system;
# those entry points are already provided by existing libc modules.
file(GLOB MUSL_PROCESS_SOURCES CONFIGURE_DEPENDS
    ${MUSL_DIR}/src/process/posix_spawn.c
    ${MUSL_DIR}/src/process/posix_spawnp.c
    ${MUSL_DIR}/src/process/posix_spawnattr_*.c
    ${MUSL_DIR}/src/process/posix_spawn_file_actions_*.c)
# The repository-owned implementation explicitly rejects scheduling flags that
# this musl version otherwise accepts but silently ignores in posix_spawn().
list(REMOVE_ITEM MUSL_PROCESS_SOURCES
    ${MUSL_DIR}/src/process/posix_spawnattr_setflags.c)
list(APPEND MUSL_PROCESS_SOURCES
    ${MUSL_DIR}/src/process/execvp.c
    ${CMAKE_SOURCE_DIR}/user/lib/posix_spawn_policy.c)

add_musl_lib(musl_process_objs SOURCES ${MUSL_PROCESS_SOURCES})
