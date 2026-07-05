#!/bin/bash
# Build-time assertion: ld.so's .rela.plt must be empty
# Usage: verify_ldso_rela_plt.sh <ldso_elf>
# Failure (contains JUMP_SLOT relocations) → exit 1
# After ld.so uses global -fvisibility=hidden, internal functions don't go through PLT, .rela.plt should be empty
# If non-empty, some internal function is not marked hidden; before bootstrap the GOT is unfilled and would jump to lazy stub and crash
elf="$1"
if readelf -r "$elf" 2>/dev/null | grep 'R_X86_64_JUMP_SLO'; then
    echo "FATAL: $elf has JUMP_SLOT relocations in .rela.plt"
    echo "  ld.so internal functions must be hidden visibility"
    echo "  (add -fvisibility=hidden to add_user_ldso COMPILE_FLAGS)"
    echo "  PLT calls before GOT is filled jump to lazy stub and crash"
    exit 1
fi
