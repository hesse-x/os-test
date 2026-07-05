#!/bin/bash
# Build-time assertion: shared library must not reference hidden linker symbols of the main ELF
# Usage: verify_so_init_array.sh <so_file>
# Failure (found __init_array_*/__fini_array_* references) → exit 1
so="$1"
if readelf --dyn-syms "$so" 2>/dev/null | grep -E '__init_array_(start|end)|__fini_array_(start|end)'; then
    echo "FATAL: $so references hidden linker symbols __init_array_*/__fini_array_*"
    echo "  shared libs cannot extern-reference main ELF hidden symbols"
    echo "  (pass init/fini array range via __libc_start_main args instead)"
    exit 1
fi
