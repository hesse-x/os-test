#!/bin/bash
# 构建期断言：共享库不引用主 ELF 的 hidden 链接器符号
# 用法：verify_so_init_array.sh <so_file>
# 失败（找到 __init_array_*/__fini_array_* 引用）→ exit 1
so="$1"
if readelf --dyn-syms "$so" 2>/dev/null | grep -E '__init_array_(start|end)|__fini_array_(start|end)'; then
    echo "FATAL: $so references hidden linker symbols __init_array_*/__fini_array_*"
    echo "  shared libs cannot extern-reference main ELF hidden symbols"
    echo "  (pass init/fini array range via __libc_start_main args instead)"
    exit 1
fi
