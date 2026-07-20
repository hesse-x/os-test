# cmake/do_link.cmake — Link script called by add_custom_command
# OBJ_LIST is passed as a string where groups from same target are semicolon-separated
# and different targets are space-separated. We need to normalize to a clean list.

# First, replace spaces with semicolons to merge all groups
string(REPLACE " " ";" OBJ_LIST "${OBJ_LIST}")

# Now OBJ_LIST is a proper CMake list (all semicolons)
#
# --no-relax: the kernel is built with -fPIE (small code model), so the
# compiler emits R_X86_64_REX_GOTPCRELX for address-of-global references
# (e.g. (uint64_t)vector2 in trap.c). ld's GOTPCREL->LEA relaxation only
# accepts link addresses inside the kernel-code-model window
# [0xFFFFFFFF80000000, 0xFFFFFFFFFFFFFFFF]; once VMA_BASE moved down to
# 0xFFFFFF8000000000 (outside that window) the relaxation fails with
# "failed to convert GOTPCREL relocation ... relink with --no-relax".
# --no-relax keeps those as GOT loads (one extra indirection per
# address-of-global, negligible for a microkernel) and links cleanly at any
# canonical high-half address.
execute_process(
    COMMAND ld -m elf_x86_64 --no-relax -T ${LINKER_SCRIPT} ${OBJ_LIST} -o ${BIN_FILE}
    RESULT_VARIABLE ret
)

if(NOT ret EQUAL 0)
    message(FATAL_ERROR "Linking failed with code: ${ret}")
endif()
